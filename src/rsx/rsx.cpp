#include "rsx/rsx.h"

#include "rsx/methods.h"

#include <algorithm>
#include <bit>
#include <format>
#include <ostream>

namespace rsx {

namespace {

// FIFO command words. envytools, "DMA submission to FIFOs on NV4" (NV4-style pusher).
constexpr u32 k_return_command = 0x0002'0000;
constexpr u32 k_method_count_shift = 18;
constexpr u32 k_method_count_mask = 0x7ff;
constexpr u32 k_non_increasing = 0x4000'0000;

bool is_old_jump(u32 word) {
    return (word & 0xe000'0003) == 0x2000'0000;
}
bool is_jump(u32 word) {
    return (word & 3) == 1;
}
bool is_call(u32 word) {
    return (word & 3) == 2;
}
bool is_methods(u32 word) {
    return (word & 0xa003'0003) == 0;
}

// Splits a {position, size} register: position in the low 16 bits, size in the high 16.
Rect rect_from(u32 horizontal, u32 vertical) {
    return {horizontal & 0xffff, vertical & 0xffff, horizontal >> 16, vertical >> 16};
}

Rect intersect(const Rect& a, const Rect& b) {
    const u32 left = std::max(a.x, b.x);
    const u32 top = std::max(a.y, b.y);
    const u32 right = std::min(a.x + a.width, b.x + b.width);
    const u32 bottom = std::min(a.y + a.height, b.y + b.height);
    if (right <= left || bottom <= top) return {};
    return {left, top, right - left, bottom - top};
}

} // namespace

Rsx::Rsx(Memory& memory, VirtualClock& clock) : memory_(memory), clock_(clock) {
    // UNVERIFIED: the scissor libgcm leaves after initialization. Everything, so a program that
    // never sets one still draws.
    registers_[0][method::k_scissor_horizontal / 4] = 4096u << 16;
    registers_[0][method::k_scissor_vertical / 4] = 4096u << 16;
    registers_[0][method::k_color_mask / 4] = 0x0101'0101;
    // UNVERIFIED: the rest start as OpenGL's defaults.
    registers_[0][method::k_alpha_func / 4] = method::k_always;
    registers_[0][method::k_blend_func_source / 4] = 1 | 1 << 16;        // ONE
    registers_[0][method::k_blend_equation / 4] = 0x8006 | 0x8006 << 16; // FUNC_ADD
    registers_[0][method::k_depth_func / 4] = method::k_less;
    registers_[0][method::k_cull_face / 4] = method::k_cull_back;
    registers_[0][method::k_front_face / 4] = method::k_front_face_ccw;
}

// ---- Memory ----

bool Rsx::initialize(u32 io_address, u32 io_size) {
    if (io_address % k_io_page_size != 0 || io_size % k_io_page_size != 0 || io_size == 0 ||
        io_size / k_io_page_size > k_io_page_count) {
        return false;
    }
    if (!initialized_) memory_.map(k_local_base, k_local_size, Memory::k_read_write);
    initialized_ = true;
    io_size_ = io_size;
    io_pages_.fill(std::nullopt);
    for (u32 page = 0; page < io_size / k_io_page_size; ++page) {
        io_pages_[page] = io_address + page * k_io_page_size;
    }
    memory_.write<u32>(k_control_register + 8, 0xffff'ffff); // REF starts at all ones
    for (u32 i = 0; i < k_label_count; ++i) {
        memory_.write<u32>(k_labels + 16 * i, 0);
    }
    return true;
}

std::optional<u32> Rsx::map_main_memory(u32 address, u32 size) {
    if (address % k_io_page_size != 0 || size % k_io_page_size != 0 || size == 0) {
        return std::nullopt;
    }
    const u32 pages = size / k_io_page_size;
    for (u32 first = 0; first + pages <= k_io_page_count; ++first) {
        const bool free = std::all_of(io_pages_.begin() + first, io_pages_.begin() + first + pages,
                                      [](const auto& page) { return !page; });
        if (!free) continue;
        for (u32 i = 0; i < pages; ++i) {
            io_pages_[first + i] = address + i * k_io_page_size;
        }
        return first * k_io_page_size;
    }
    return std::nullopt;
}

std::optional<u32> Rsx::address_to_offset(u32 address) const {
    if (address >= k_local_base && address - k_local_base < k_local_size) {
        return address - k_local_base;
    }
    for (u32 page = 0; page < k_io_page_count; ++page) {
        if (io_pages_[page] && address - *io_pages_[page] < k_io_page_size) {
            return page * k_io_page_size + (address - *io_pages_[page]);
        }
    }
    return std::nullopt;
}

std::optional<u32> Rsx::io_offset_to_address(u32 offset) const {
    const u32 page = offset / k_io_page_size;
    if (page >= k_io_page_count || !io_pages_[page]) return std::nullopt;
    return *io_pages_[page] + offset % k_io_page_size;
}

std::optional<u32> Rsx::resolve(u32 context, u32 offset) const {
    switch (context) {
    case method::k_context_local:
        if (offset >= k_local_size) return std::nullopt;
        return k_local_base + offset;
    case method::k_context_main: return io_offset_to_address(offset);
    default: return std::nullopt;
    }
}

// ---- The FIFO ----

u32 Rsx::put() const {
    return memory_.read<u32>(k_control_register);
}

u32 Rsx::get() const {
    return memory_.read<u32>(k_control_register + 4);
}

bool Rsx::has_work() const {
    if (!initialized_ || error_) return false;
    switch (waiting_) {
    case Waiting::nothing: return get() != put();
    case Waiting::semaphore: return memory_.read<u32>(semaphore_address_) == semaphore_value_;
    case Waiting::flip: return !queued_flip_;
    }
    return false;
}

void Rsx::run() {
    if (!initialized_ || error_) return;
    try {
        if (queued_flip_ && clock_.cycles() >= next_vblank()) complete_flip();
        run_commands();
    } catch (const MemoryFault& fault) {
        fail(fault.what()); // a command made the RSX touch memory that isn't mapped
    }
}

void Rsx::run_commands() {

    for (u64 done = 0; done < k_commands_per_turn && has_work(); ++done) {
        waiting_ = Waiting::nothing;
        u32 position = get();
        const auto address = io_offset_to_address(position);
        if (!address) return fail(std::format("FIFO GET {:#x} isn't mapped", position));
        const u32 word = memory_.read<u32>(*address);

        if (is_old_jump(word)) {
            position = word & 0x1fff'fffc;
        } else if (is_jump(word)) {
            position = word & ~3u;
        } else if (is_call(word)) {
            if (return_offset_) return fail(std::format("FIFO call at {:#x} nests", position));
            return_offset_ = position + 4;
            position = word & ~3u;
        } else if (word == k_return_command) {
            if (!return_offset_)
                return fail(std::format("FIFO return at {:#x} without a call", position));
            position = *return_offset_;
            return_offset_.reset();
        } else if (is_methods(word)) {
            const u32 count = (word >> k_method_count_shift) & k_method_count_mask;
            const u32 subchannel = (word >> 13) & 7;
            const u32 first = word & 0x1ffc;
            // libgcm moves PUT past whole commands, so a command's data is always there.
            for (u32 i = 0; i < count; ++i) {
                const auto data = io_offset_to_address(position + 4 + 4 * i);
                if (!data) return fail(std::format("FIFO data at {:#x} isn't mapped", position));
                const u32 method = (word & k_non_increasing) ? first : (first + 4 * i) & 0x1ffc;
                execute(subchannel, method, memory_.read<u32>(*data));
                if (error_) return;
            }
            position += 4 + 4 * count;
        } else {
            return fail(std::format("invalid FIFO command {:#010x} at {:#x}", word, position));
        }
        memory_.write<u32>(k_control_register + 4, position);
    }
}

void Rsx::execute(u32 subchannel, u32 method, u32 value) {
    if (trace) *trace << std::format("    RSX {}:{:#06x} = {:#010x}\n", subchannel, method, value);
    registers_[subchannel][method / 4] = value;

    // Methods below 0x100 belong to the FIFO channel itself, whatever the subchannel.
    switch (method) {
    case method::k_set_reference: memory_.write<u32>(k_control_register + 8, value); return;
    case method::k_semaphore_acquire:
        // UNVERIFIED: semaphore offsets count from the label area, whatever the context.
        semaphore_address_ = k_labels + reg(method::k_semaphore_offset, subchannel);
        semaphore_value_ = value;
        waiting_ = Waiting::semaphore;
        return;
    case method::k_semaphore_release:
        memory_.write<u32>(k_labels + reg(method::k_semaphore_offset, subchannel), value);
        return;
    default: break;
    }
    if (method < 0x100) return;

    if (subchannel == method::k_hle_subchannel) {
        if (method == method::k_hle_flip) queue_flip(value);
        if (method == method::k_hle_wait_flip && queued_flip_) waiting_ = Waiting::flip;
        return;
    }
    execute_3d(method, value);
}

void Rsx::execute_3d(u32 method, u32 value) {
    // Windows of methods that stream data: each write takes the next word.
    if (method >= method::k_transform_program && method < method::k_transform_program + 0x80) {
        transform_program_[transform_program_position_++ % transform_program_.size()] = value;
        return;
    }
    if (method >= method::k_transform_constant && method < method::k_transform_constant + 0x80) {
        const u32 position = transform_constant_position_++ % (vertex::k_constant_count * 4);
        transform_constants_[position / 4][position % 4] = value;
        return;
    }

    switch (method) {
    case method::k_transform_program_load: transform_program_position_ = value * 4; break;
    case method::k_transform_constant_load: transform_constant_position_ = value * 4; break;
    case method::k_begin_end:
        if (value != 0) {
            primitive_ = value;
            draw_vertices_.clear();
        } else if (primitive_) {
            end_primitive();
        }
        break;
    case method::k_draw_arrays: {
        const u32 first = value & 0xff'ffff;
        const u32 count = (value >> 24) + 1;
        for (u32 i = 0; i < count; ++i) {
            draw_vertices_.push_back(first + i);
        }
        break;
    }
    case method::k_draw_index_array: read_indices(value & 0xff'ffff, (value >> 24) + 1); break;
    case method::k_clear_surface: clear_surface(value); break;
    case method::k_back_end_write_semaphore_release:
    case method::k_texture_read_semaphore_release:
        memory_.write<u32>(k_labels + reg(method::k_semaphore_offset_3d), value);
        break;
    default: break;
    }
}

// ---- Rendering ----

// Appends `count` vertex indices from the index array, starting at index `first`.
void Rsx::read_indices(u32 first, u32 count) {
    const u32 format = reg(method::k_index_array_format);
    const u32 context = (format & 0xf) == 0 ? method::k_context_local : method::k_context_main;
    const u32 type = format >> 4;
    if (type != method::k_index_16 && type != method::k_index_32) {
        return fail(std::format("index array type {} is not supported yet", type));
    }
    const u32 size = type == method::k_index_16 ? 2 : 4;
    const auto address = resolve(context, reg(method::k_index_array_offset));
    if (!address) return fail("the index array is at an unmapped offset");
    for (u32 i = first; i < first + count; ++i) {
        const u32 at = *address + size * i;
        draw_vertices_.push_back(size == 2 ? memory_.read<u16>(at) : memory_.read<u32>(at));
    }
}

void Rsx::clear_surface(u32 mask) {
    const u32 format = reg(method::k_surface_format);
    const u32 color_format = format & 0x1f;
    const u32 depth_format = (format >> 5) & 7;

    ClearRequest request;
    request.mask = mask;
    request.color = reg(method::k_color_clear_value);
    request.depth_value = reg(method::k_zstencil_clear_value);
    request.area = drawing_area();

    struct Target {
        u32 context, offset, pitch;
    };
    const Target targets[] = {
        {method::k_set_color_context_a, method::k_surface_color_offset_a,
         method::k_surface_pitch_a},
        {method::k_set_color_context_b, method::k_surface_color_offset_b,
         method::k_surface_pitch_b},
        {method::k_set_color_context_c, method::k_surface_color_offset_c,
         method::k_surface_pitch_c},
        {method::k_set_color_context_d, method::k_surface_color_offset_d,
         method::k_surface_pitch_d},
    };
    const u32 enabled = reg(method::k_surface_color_target);
    for (u32 i = 0; i < 4; ++i) {
        if ((enabled & (1u << i)) == 0) continue;
        if (color_format != 4 && color_format != 5 && color_format != 8) {
            return fail(std::format("color surface format {} is not supported yet", color_format));
        }
        const Target& target = targets[i];
        const auto address = resolve(reg(target.context), reg(target.offset));
        if (!address) return fail(std::format("color surface {} is at an unmapped offset", i));
        request.colors.push_back(
            {*address, reg(target.pitch), static_cast<ColorFormat>(color_format)});
    }
    if (depth_format == 1 || depth_format == 2) {
        const auto address =
            resolve(reg(method::k_set_zeta_context), reg(method::k_surface_zeta_offset));
        if (address) {
            request.depth = DepthSurface{*address, reg(method::k_surface_pitch_zeta),
                                         static_cast<DepthFormat>(depth_format)};
        }
    }
    if (backend_) backend_->clear(request);
}

// The first color target, where draws go.
std::optional<ColorSurface> Rsx::color_surface() {
    const u32 format = reg(method::k_surface_format) & 0x1f;
    if (format != 4 && format != 5 && format != 8) {
        fail(std::format("color surface format {} is not supported yet", format));
        return std::nullopt;
    }
    if ((reg(method::k_surface_color_target) & 0xf) != 1) {
        fail("drawing to color targets other than the first alone is not supported yet");
        return std::nullopt;
    }
    const auto address =
        resolve(reg(method::k_set_color_context_a), reg(method::k_surface_color_offset_a));
    if (!address) {
        fail("color surface 0 is at an unmapped offset");
        return std::nullopt;
    }
    return ColorSurface{*address, reg(method::k_surface_pitch_a), static_cast<ColorFormat>(format)};
}

Rect Rsx::drawing_area() const {
    return intersect(
        rect_from(reg(method::k_surface_clip_horizontal), reg(method::k_surface_clip_vertical)),
        rect_from(reg(method::k_scissor_horizontal), reg(method::k_scissor_vertical)));
}

// BEGIN_END(0): the vertices since BEGIN_END(type) are complete, so draw them.
void Rsx::end_primitive() {
    const u32 type = *primitive_;
    primitive_.reset();

    struct Unsupported {
        u32 method;
        const char* what;
    };
    for (const auto& [enable, what] :
         {Unsupported{method::k_stencil_test_enable, "stencil testing"},
          Unsupported{method::k_logic_op_enable, "logic operations"}}) {
        if (reg(enable) != 0) return fail(std::format("{} is not supported yet", what));
    }

    DrawRequest request;
    switch (type) {
    case method::k_triangles: request.primitive = Primitive::triangles; break;
    case method::k_triangle_strip: request.primitive = Primitive::triangle_strip; break;
    case method::k_triangle_fan: request.primitive = Primitive::triangle_fan; break;
    default: return fail(std::format("primitive type {} is not supported yet", type));
    }
    request.vertices = std::move(draw_vertices_);

    for (u32 i = 0; i < vertex::k_input_count; ++i) {
        const u32 format = reg(method::k_vertex_data_array_format + 4 * i);
        const u32 size = (format >> 4) & 0xf;
        if (size == 0) continue; // not an array: the input reads 0, 0, 0, 1
        const u32 type = format & 0xf;
        if (type != method::k_vertex_float && type != method::k_vertex_ubyte) {
            return fail(std::format("vertex attribute type {} is not supported yet", type));
        }
        const u32 offset = reg(method::k_vertex_data_array_offset + 4 * i);
        const u32 context = (offset >> 31) ? method::k_context_main : method::k_context_local;
        const auto address = resolve(context, offset & 0x7fff'ffff);
        if (!address) return fail(std::format("vertex array {} is at an unmapped offset", i));
        request.inputs[i] = VertexInput{*address, type, size, (format >> 8) & 0xff};
    }

    auto instructions =
        vertex::decode_program(transform_program_, reg(method::k_transform_program_start));
    if (!instructions) return fail("the vertex program has no last instruction");
    request.vertex_program.instructions = std::move(*instructions);
    request.vertex_program.constants = transform_constants_;

    const u32 program = reg(method::k_shader_program);
    const u32 program_context =
        (program & 3) == 2 ? method::k_context_main : method::k_context_local;
    const auto program_address = resolve(program_context, program & ~3u);
    if (!program_address) return fail("the fragment program is at an unmapped offset");
    auto fragment_program = fragment::decode_program(memory_, *program_address);
    if (!fragment_program) return fail("the fragment program has no end");
    request.fragment_program = std::move(*fragment_program);

    for (u32 i = 0; i < 4; ++i) {
        request.viewport_scale[i] = reg(method::k_viewport_scale + 4 * i);
        request.viewport_offset[i] = reg(method::k_viewport_offset + 4 * i);
    }
    request.area = drawing_area();
    const auto target = color_surface();
    if (!target) return;
    request.target = *target;
    request.color_mask = reg(method::k_color_mask);
    request.flat_shading = reg(method::k_shade_mode) == method::k_shade_flat;
    if (!set_alpha_test(request) || !set_depth_test(request) || !set_blend(request) ||
        !set_culling(request)) {
        return;
    }
    for (const fragment::Instruction& instruction : request.fragment_program) {
        if (!fragment::is_texture_lookup(instruction)) continue;
        const u32 unit = instruction.texture_unit;
        if (request.textures[unit]) continue;
        request.textures[unit] = texture(unit);
        if (!request.textures[unit]) return;
    }

    if (backend_) {
        if (auto error = backend_->draw(request)) fail(std::move(*error));
    }
}

bool Rsx::set_alpha_test(DrawRequest& request) {
    if (reg(method::k_alpha_test_enable) == 0) return true;
    const u32 compare = reg(method::k_alpha_func);
    if (compare < method::k_never || compare > method::k_always) {
        fail(std::format("alpha function {:#x} is invalid", compare));
        return false;
    }
    request.alpha_test = AlphaTest{static_cast<Compare>(compare), reg(method::k_alpha_ref) & 0xff};
    return true;
}

bool Rsx::set_blend(DrawRequest& request) {
    if (reg(method::k_blend_enable) == 0) return true;
    const auto factor = [&](u32 value) -> std::optional<BlendFactor> {
        if (value <= 1 || (value >= 0x300 && value <= 0x308) ||
            (value >= 0x8001 && value <= 0x8004)) {
            return static_cast<BlendFactor>(value);
        }
        fail(std::format("blend factor {:#x} is not supported yet", value));
        return std::nullopt;
    };
    const auto equation = [&](u32 value) -> std::optional<BlendEquation> {
        if (value == 0x8006 || value == 0x8007 || value == 0x8008 || value == 0x800a ||
            value == 0x800b) {
            return static_cast<BlendEquation>(value);
        }
        fail(std::format("blend equation {:#x} is not supported yet", value));
        return std::nullopt;
    };
    const u32 source = reg(method::k_blend_func_source);
    const u32 destination = reg(method::k_blend_func_destination);
    const u32 equations = reg(method::k_blend_equation);
    const auto source_rgb = factor(source & 0xffff);
    const auto source_alpha = factor(source >> 16);
    const auto destination_rgb = factor(destination & 0xffff);
    const auto destination_alpha = factor(destination >> 16);
    const auto equation_rgb = equation(equations & 0xffff);
    const auto equation_alpha = equation(equations >> 16);
    if (error_) return false;
    request.blend =
        Blend{*source_rgb,   *source_alpha,   *destination_rgb,          *destination_alpha,
              *equation_rgb, *equation_alpha, reg(method::k_blend_color)};
    return true;
}

bool Rsx::set_depth_test(DrawRequest& request) {
    if (reg(method::k_depth_test_enable) == 0) return true;
    const u32 format = (reg(method::k_surface_format) >> 5) & 7;
    if (format != 1 && format != 2) {
        fail(std::format("depth format {} is not supported yet", format));
        return false;
    }
    const auto address =
        resolve(reg(method::k_set_zeta_context), reg(method::k_surface_zeta_offset));
    if (!address) {
        fail("the depth surface is at an unmapped offset");
        return false;
    }
    const u32 compare = reg(method::k_depth_func);
    if (compare < method::k_never || compare > method::k_always) {
        fail(std::format("depth function {:#x} is invalid", compare));
        return false;
    }
    request.depth_test =
        DepthTest{{*address, reg(method::k_surface_pitch_zeta), static_cast<DepthFormat>(format)},
                  static_cast<Compare>(compare),
                  reg(method::k_depth_write_enable) != 0};
    return true;
}

bool Rsx::set_culling(DrawRequest& request) {
    // UNVERIFIED: the front face is judged as triangles appear on the display, which is the
    // window origin (top left) libgcm sets in COORD_CONVENTIONS.
    request.front_ccw = reg(method::k_front_face) != method::k_front_face_cw;
    if (reg(method::k_cull_face_enable) == 0) return true;
    switch (reg(method::k_cull_face)) {
    case method::k_cull_front: request.cull = Cull::front; return true;
    case method::k_cull_back: request.cull = Cull::back; return true;
    case method::k_cull_front_and_back: request.cull = Cull::front_and_back; return true;
    default:
        fail(std::format("cull face {:#x} is invalid", reg(method::k_cull_face)));
        return false;
    }
}

// Texture unit `unit` as the methods describe it; none (and a fault) if it can't be sampled.
// Register fields: nouveau's nv30-40_3d.xml.h (NV40_3D_TEX_*), checked against what PSL1GHT's
// libgcm writes.
std::optional<Texture> Rsx::texture(u32 unit) {
    const auto field = [&](u32 method, u32 shift, u32 width) {
        return (reg(method + unit * method::k_texture_stride) >> shift) & ((1u << width) - 1);
    };
    const auto unsupported = [&](const std::string& what) {
        fail(std::format("texture unit {}: {} is not supported yet", unit, what));
        return std::nullopt;
    };
    if (field(method::k_texture_control, 31, 1) == 0) {
        fail(std::format("the fragment program samples disabled texture unit {}", unit));
        return std::nullopt;
    }

    Texture texture;
    const u32 location = field(method::k_texture_format, 0, 2);
    const u32 context = location == 2 ? method::k_context_main : method::k_context_local;
    const auto address =
        resolve(context, reg(method::k_texture_offset + unit * method::k_texture_stride));
    if (location == 0 || location == 3 || !address) {
        fail(std::format("texture unit {} is at an unmapped offset", unit));
        return std::nullopt;
    }
    texture.address = *address;
    if (field(method::k_texture_format, 2, 1)) return unsupported("a cube map");
    if (const u32 dimensions = field(method::k_texture_format, 4, 4); dimensions != 2) {
        return unsupported(std::format("a {}D texture", dimensions));
    }

    // The format byte: GCM_TEXTURE_FORMAT_* in bits 0-4, then LIN and UNRM.
    const u32 format = field(method::k_texture_format, 8, 5);
    switch (format) {
    case 2:
    case 3:
    case 4:
    case 5:
    case 29:
    case 30: texture.format = static_cast<TextureFormat>(format); break;
    default: return unsupported(std::format("format {}", format));
    }
    texture.linear = field(method::k_texture_format, 13, 1) != 0;
    texture.normalized = field(method::k_texture_format, 14, 1) == 0;
    texture.levels = std::max(field(method::k_texture_format, 16, 4), 1u);
    texture.width = field(method::k_texture_image_rect, 16, 16);
    texture.height = field(method::k_texture_image_rect, 0, 16);
    texture.pitch = field(method::k_texture_size, 0, 16);
    if (texture.width == 0 || texture.height == 0) return unsupported("an empty texture");
    if (!texture.linear &&
        (!std::has_single_bit(texture.width) || !std::has_single_bit(texture.height))) {
        fail(std::format("texture unit {} is swizzled but not a power of two in size", unit));
        return std::nullopt;
    }

    texture.remap = field(method::k_texture_remap, 0, 16);
    for (const auto& [shift, wrap] : {std::pair{0u, &texture.wrap_s}, {8u, &texture.wrap_t}}) {
        const u32 mode = field(method::k_texture_address, shift, 4);
        if (mode < 1 || mode > 4) return unsupported(std::format("wrap mode {}", mode));
        *wrap = static_cast<TextureWrap>(mode);
    }
    const u32 minify = field(method::k_texture_filter, 16, 4);
    const u32 magnify = field(method::k_texture_filter, 24, 4);
    if (minify < 1 || minify > 6) return unsupported(std::format("minification filter {}", minify));
    if (magnify < 1 || magnify > 2) {
        return unsupported(std::format("magnification filter {}", magnify));
    }
    texture.minify = static_cast<TextureFilter>(minify);
    texture.magnify = static_cast<TextureFilter>(magnify);
    // The LOD range is unsigned 4.8 fixed point (libgcm writes 12.0 as 0xc00 << 7). UNVERIFIED:
    // the bias as a signed 5.8 number in bits 0-12.
    const u32 bias = field(method::k_texture_filter, 0, 13);
    texture.lod_bias = static_cast<s32>(bias << 19) >> 19;
    texture.max_lod = field(method::k_texture_control, 7, 12);
    texture.min_lod = field(method::k_texture_control, 19, 12);
    texture.border_color = reg(method::k_texture_border_color + unit * method::k_texture_stride);
    return texture;
}

// ---- Display ----

void Rsx::queue_flip(u32 id) {
    if (id >= k_display_buffer_count) return fail(std::format("flip to display buffer {}", id));
    queued_flip_ = id;
    flip_queued_at_ = clock_.cycles();
    if (flip_mode_ == FlipMode::hsync) complete_flip();
}

// Vblanks come at fixed times on the virtual clock: the next one after the flip was queued.
u64 Rsx::next_vblank() const {
    constexpr u64 k_cycles = VirtualClock::k_cpu_frequency;
    const u64 count = flip_queued_at_ * k_vblank_frequency / k_cycles + 1;
    return (count * k_cycles + k_vblank_frequency - 1) / k_vblank_frequency;
}

std::optional<u64> Rsx::next_deadline() const {
    if (!queued_flip_ || flip_mode_ != FlipMode::vsync) return std::nullopt;
    return next_vblank();
}

void Rsx::complete_flip() {
    const DisplayBuffer& buffer = display_buffers_[*queued_flip_];
    queued_flip_.reset();
    flip_status_ = 0;
    ++frames_;
    // A frame shown before isn't shown again when a debugger replays the run up to it.
    if (!on_frame || frames_ <= frames_shown_) return;
    frames_shown_ = frames_;

    if (backend_) backend_->flush();
    Frame frame;
    frame.number = frames_;
    frame.width = buffer.width;
    frame.height = buffer.height;
    frame.rgb.reserve(std::size_t{buffer.width} * buffer.height * 3);
    for (u32 y = 0; y < buffer.height; ++y) {
        for (u32 x = 0; x < buffer.width; ++x) {
            // The display reads X8R8G8B8 pixels and ignores the X byte.
            const u32 pixel =
                memory_.read<u32>(k_local_base + buffer.offset + y * buffer.pitch + 4 * x);
            frame.rgb.push_back(static_cast<u8>(pixel >> 16));
            frame.rgb.push_back(static_cast<u8>(pixel >> 8));
            frame.rgb.push_back(static_cast<u8>(pixel));
        }
    }
    on_frame(frame);
}

std::string Rsx::describe_wait() const {
    switch (waiting_) {
    case Waiting::nothing: return "";
    case Waiting::semaphore:
        return std::format("RSX waits for label {} to be {:#x}",
                           (semaphore_address_ - k_labels) / 16, semaphore_value_);
    case Waiting::flip: return "RSX waits for a flip";
    }
    return "";
}

void Rsx::fail(std::string message) {
    error_ = "RSX: " + std::move(message);
}

} // namespace rsx
