#include "gpu/metal_backend.h"

#include "gpu/msl.h"
#include "gpu/texture.h"
#include "gpu/vertex_input.h"
#include "rsx/methods.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <tuple>
#include <vector>

#import <Metal/Metal.h>

namespace gpu {

struct MetalBackend::Device {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    // Compiled pipelines by their shader source, color write mask, depth format and blending
    // (factors and equations).
    std::map<std::tuple<std::string, MTLColorWriteMask, MTLPixelFormat,
                        std::optional<std::array<u32, 6>>>,
             id<MTLRenderPipelineState>>
        pipelines;
};

namespace {

// The viewport block the vertex function reads (see gpu/msl.h).
struct Viewport {
    float scale[4];
    float offset[4];
    float size[4];
};

float host_float(u32 bits) {
    return std::bit_cast<float>(bits);
}

std::string describe(NSError* error) {
    return error ? std::string([[error localizedDescription] UTF8String]) : "unknown error";
}

MTLColorWriteMask write_mask(const rsx::DrawRequest& request) {
    MTLColorWriteMask mask = MTLColorWriteMaskNone;
    if (request.color_mask & 0x0001'0000) mask |= MTLColorWriteMaskRed;
    if (request.color_mask & 0x0000'0100) mask |= MTLColorWriteMaskGreen;
    if (request.color_mask & 0x0000'0001) mask |= MTLColorWriteMaskBlue;
    // Without an alpha channel, the X byte (held in alpha) keeps what memory had.
    if ((request.color_mask & 0x0100'0000) && request.target.format == rsx::ColorFormat::a8r8g8b8) {
        mask |= MTLColorWriteMaskAlpha;
    }
    return mask;
}

bool uses_mipmaps(rsx::TextureFilter filter) {
    return filter != rsx::TextureFilter::nearest && filter != rsx::TextureFilter::linear;
}

MTLSamplerMinMagFilter min_mag_filter(rsx::TextureFilter filter) {
    switch (filter) {
    case rsx::TextureFilter::linear:
    case rsx::TextureFilter::linear_mipmap_nearest:
    case rsx::TextureFilter::linear_mipmap_linear: return MTLSamplerMinMagFilterLinear;
    default: return MTLSamplerMinMagFilterNearest;
    }
}

MTLSamplerAddressMode address_mode(rsx::TextureWrap wrap) {
    switch (wrap) {
    case rsx::TextureWrap::repeat: return MTLSamplerAddressModeRepeat;
    case rsx::TextureWrap::mirrored_repeat: return MTLSamplerAddressModeMirrorRepeat;
    case rsx::TextureWrap::clamp_to_edge: return MTLSamplerAddressModeClampToEdge;
    case rsx::TextureWrap::border: return MTLSamplerAddressModeClampToBorderColor;
    }
    return MTLSamplerAddressModeRepeat;
}

// How many levels a texture keeps: those its filter can reach, as many as Metal allows.
u32 level_count(const rsx::Texture& texture) {
    if (!texture.normalized || !uses_mipmaps(texture.minify)) return 1;
    const u32 largest = std::max(texture.width, texture.height);
    return std::min(texture.levels, static_cast<u32>(std::bit_width(largest)));
}

MTLCompareFunction compare_function(rsx::Compare compare) {
    switch (compare) {
    case rsx::Compare::never: return MTLCompareFunctionNever;
    case rsx::Compare::less: return MTLCompareFunctionLess;
    case rsx::Compare::equal: return MTLCompareFunctionEqual;
    case rsx::Compare::less_equal: return MTLCompareFunctionLessEqual;
    case rsx::Compare::greater: return MTLCompareFunctionGreater;
    case rsx::Compare::not_equal: return MTLCompareFunctionNotEqual;
    case rsx::Compare::greater_equal: return MTLCompareFunctionGreaterEqual;
    case rsx::Compare::always: return MTLCompareFunctionAlways;
    }
    return MTLCompareFunctionAlways;
}

constexpr float k_z24_maximum = 16777215.0f;

MTLBlendFactor blend_factor(rsx::BlendFactor factor) {
    using rsx::BlendFactor;
    switch (factor) {
    case BlendFactor::zero: return MTLBlendFactorZero;
    case BlendFactor::one: return MTLBlendFactorOne;
    case BlendFactor::source_color: return MTLBlendFactorSourceColor;
    case BlendFactor::one_minus_source_color: return MTLBlendFactorOneMinusSourceColor;
    case BlendFactor::source_alpha: return MTLBlendFactorSourceAlpha;
    case BlendFactor::one_minus_source_alpha: return MTLBlendFactorOneMinusSourceAlpha;
    case BlendFactor::destination_alpha: return MTLBlendFactorDestinationAlpha;
    case BlendFactor::one_minus_destination_alpha: return MTLBlendFactorOneMinusDestinationAlpha;
    case BlendFactor::destination_color: return MTLBlendFactorDestinationColor;
    case BlendFactor::one_minus_destination_color: return MTLBlendFactorOneMinusDestinationColor;
    case BlendFactor::source_alpha_saturate: return MTLBlendFactorSourceAlphaSaturated;
    case BlendFactor::constant_color: return MTLBlendFactorBlendColor;
    case BlendFactor::one_minus_constant_color: return MTLBlendFactorOneMinusBlendColor;
    case BlendFactor::constant_alpha: return MTLBlendFactorBlendAlpha;
    case BlendFactor::one_minus_constant_alpha: return MTLBlendFactorOneMinusBlendAlpha;
    }
    return MTLBlendFactorOne;
}

MTLBlendOperation blend_operation(rsx::BlendEquation equation) {
    switch (equation) {
    case rsx::BlendEquation::add: return MTLBlendOperationAdd;
    case rsx::BlendEquation::subtract: return MTLBlendOperationSubtract;
    case rsx::BlendEquation::reverse_subtract: return MTLBlendOperationReverseSubtract;
    case rsx::BlendEquation::minimum: return MTLBlendOperationMin;
    case rsx::BlendEquation::maximum: return MTLBlendOperationMax;
    }
    return MTLBlendOperationAdd;
}

bool reads_destination_alpha(const rsx::Blend& blend) {
    using rsx::BlendFactor;
    for (const BlendFactor factor :
         {blend.source_rgb, blend.source_alpha, blend.destination_rgb, blend.destination_alpha}) {
        if (factor == BlendFactor::destination_alpha ||
            factor == BlendFactor::one_minus_destination_alpha ||
            factor == BlendFactor::source_alpha_saturate) {
            return true;
        }
    }
    return false;
}

// Uploads a texture's levels, decoded (and remapped) the reference backend's way, as RGBA8.
id<MTLTexture> upload_texture(id<MTLDevice> device, const Memory& memory,
                              const rsx::Texture& texture) {
    const u32 levels = level_count(texture);
    MTLTextureDescriptor* descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:texture.width
                                                          height:texture.height
                                                       mipmapped:NO];
    descriptor.mipmapLevelCount = levels;
    descriptor.usage = MTLTextureUsageShaderRead;
    descriptor.storageMode = MTLStorageModeShared;
    id<MTLTexture> result = [device newTextureWithDescriptor:descriptor];
    for (u32 level = 0; level < levels; ++level) {
        const LevelSize size = level_size(texture, level);
        std::vector<u8> texels;
        texels.reserve(std::size_t{size.width} * size.height * 4);
        for (u32 y = 0; y < size.height; ++y) {
            for (u32 x = 0; x < size.width; ++x) {
                for (const u32 channel : read_texel(memory, texture, level, x, y)) {
                    const float value = std::clamp(host_float(channel), 0.0f, 1.0f);
                    texels.push_back(static_cast<u8>(std::lround(value * 255.0f)));
                }
            }
        }
        [result replaceRegion:MTLRegionMake2D(0, 0, size.width, size.height)
                  mipmapLevel:level
                    withBytes:texels.data()
                  bytesPerRow:size.width * 4];
    }
    return result;
}

// The sampler for a texture; nil, with `error` set, if Metal can't sample it as the RSX would.
id<MTLSamplerState> make_sampler(id<MTLDevice> device, const rsx::Texture& texture,
                                 std::string& error) {
    MTLSamplerDescriptor* descriptor = [MTLSamplerDescriptor new];
    descriptor.minFilter = min_mag_filter(texture.minify);
    descriptor.magFilter = min_mag_filter(texture.magnify);
    descriptor.mipFilter = level_count(texture) == 1 ? MTLSamplerMipFilterNotMipmapped
                           : (texture.minify == rsx::TextureFilter::nearest_mipmap_linear ||
                              texture.minify == rsx::TextureFilter::linear_mipmap_linear)
                               ? MTLSamplerMipFilterLinear
                               : MTLSamplerMipFilterNearest;
    descriptor.sAddressMode = address_mode(texture.wrap_s);
    descriptor.tAddressMode = address_mode(texture.wrap_t);
    descriptor.normalizedCoordinates = texture.normalized;
    descriptor.lodMinClamp = static_cast<float>(texture.min_lod) / 256.0f;
    descriptor.lodMaxClamp = static_cast<float>(texture.max_lod) / 256.0f;
    if (!texture.normalized &&
        (texture.wrap_s != rsx::TextureWrap::clamp_to_edge ||
         texture.wrap_t != rsx::TextureWrap::clamp_to_edge || texture.minify != texture.magnify)) {
        error = "Metal samples unnormalized textures only clamped, with one filter";
        return nil;
    }
    if (texture.wrap_s == rsx::TextureWrap::border || texture.wrap_t == rsx::TextureWrap::border) {
        switch (texture.border_color) {
        case 0x0000'0000: descriptor.borderColor = MTLSamplerBorderColorTransparentBlack; break;
        case 0xff00'0000: descriptor.borderColor = MTLSamplerBorderColorOpaqueBlack; break;
        case 0xffff'ffff: descriptor.borderColor = MTLSamplerBorderColorOpaqueWhite; break;
        default:
            error = std::format("Metal can't use the border color {:#010x}", texture.border_color);
            return nil;
        }
    }
    return [device newSamplerStateWithDescriptor:descriptor];
}

// Copies the drawing area's depth back to guest memory; Z24S8 keeps its stencil byte.
void write_back_depth(Memory& memory, const rsx::DrawRequest& request, id<MTLBuffer> depth_buffer,
                      u32 width) {
    const rsx::DepthSurface& surface = request.depth_test->surface;
    const rsx::Rect& area = request.area;
    const auto* values = static_cast<const u8*>(depth_buffer.contents);
    for (u32 y = area.y; y < area.y + area.height; ++y) {
        for (u32 x = area.x; x < area.x + area.width; ++x) {
            const std::size_t index = std::size_t{y} * width + x;
            if (surface.format == rsx::DepthFormat::z16) {
                u16 value;
                std::memcpy(&value, values + index * 2, 2);
                memory.write<u16>(surface.address + y * surface.pitch + x * 2, value);
                continue;
            }
            float value;
            std::memcpy(&value, values + index * 4, 4);
            const u32 depth =
                static_cast<u32>(std::lround(std::clamp(value, 0.0f, 1.0f) * k_z24_maximum));
            const u32 address = surface.address + y * surface.pitch + x * 4;
            memory.write<u32>(address, depth << 8 | (memory.read<u32>(address) & 0xff));
        }
    }
}

} // namespace

std::unique_ptr<MetalBackend> MetalBackend::create(Memory& memory, std::string& error) {
    auto device = std::make_unique<Device>();
    device->device = MTLCreateSystemDefaultDevice();
    if (!device->device) {
        error = "this host has no Metal device";
        return nullptr;
    }
    device->queue = [device->device newCommandQueue];
    return std::unique_ptr<MetalBackend>(new MetalBackend(memory, std::move(device)));
}

MetalBackend::MetalBackend(Memory& memory, std::unique_ptr<Device> device)
    : memory_(memory), clears_(memory), device_(std::move(device)) {}

MetalBackend::~MetalBackend() = default;

std::optional<std::string> MetalBackend::draw(const rsx::DrawRequest& request) {
    if (request.cull == rsx::Cull::front_and_back) return std::nullopt; // nothing is drawn
    // The texture holds an X8 target's X byte as alpha, where the reference reads 1.
    if (request.blend && reads_destination_alpha(*request.blend) &&
        request.target.format == rsx::ColorFormat::x8r8g8b8_z8r8g8b8) {
        return "Metal can't blend with the destination alpha of an X8R8G8B8 (Z) target yet";
    }
    const MslTranslation msl = translate_to_msl(request);
    if (msl.error) return msl.error;

    // The pipeline: compile the shaders the first time this program, mask and depth format are
    // seen. Z24 depth is held as 32-bit floats, since Apple GPUs have no 24-bit format.
    const MTLColorWriteMask mask = write_mask(request);
    MTLPixelFormat depth_format = MTLPixelFormatInvalid;
    if (request.depth_test) {
        depth_format = request.depth_test->surface.format == rsx::DepthFormat::z16
                           ? MTLPixelFormatDepth16Unorm
                           : MTLPixelFormatDepth32Float;
    }
    std::optional<std::array<u32, 6>> blend_key;
    if (const auto& blend = request.blend) {
        blend_key = {
            static_cast<u32>(blend->source_rgb),      static_cast<u32>(blend->source_alpha),
            static_cast<u32>(blend->destination_rgb), static_cast<u32>(blend->destination_alpha),
            static_cast<u32>(blend->equation_rgb),    static_cast<u32>(blend->equation_alpha)};
    }
    __strong id<MTLRenderPipelineState>& pipeline =
        device_->pipelines[{msl.source, mask, depth_format, blend_key}];
    if (!pipeline) {
        MTLCompileOptions* options = [MTLCompileOptions new];
        options.mathMode = MTLMathModeSafe;
        NSError* error = nil;
        id<MTLLibrary> library =
            [device_->device newLibraryWithSource:[NSString stringWithUTF8String:msl.source.c_str()]
                                          options:options
                                            error:&error];
        if (!library) return "Metal shader compilation failed: " + describe(error);
        MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = [library newFunctionWithName:@"vertex_main"];
        descriptor.fragmentFunction = [library newFunctionWithName:@"fragment_main"];
        descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        descriptor.colorAttachments[0].writeMask = mask;
        descriptor.depthAttachmentPixelFormat = depth_format;
        if (const auto& blend = request.blend) {
            MTLRenderPipelineColorAttachmentDescriptor* color = descriptor.colorAttachments[0];
            color.blendingEnabled = YES;
            color.sourceRGBBlendFactor = blend_factor(blend->source_rgb);
            color.sourceAlphaBlendFactor = blend_factor(blend->source_alpha);
            color.destinationRGBBlendFactor = blend_factor(blend->destination_rgb);
            color.destinationAlphaBlendFactor = blend_factor(blend->destination_alpha);
            color.rgbBlendOperation = blend_operation(blend->equation_rgb);
            color.alphaBlendOperation = blend_operation(blend->equation_alpha);
        }
        pipeline = [device_->device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (!pipeline) return "Metal pipeline creation failed: " + describe(error);
    }

    // The vertices, triangle by triangle, as float4 inputs. Metal's flat shading takes the first
    // vertex's values and ours the last's, so flat triangles start from their last vertex.
    std::vector<float> inputs;
    std::size_t vertex_count = 0;
    for (auto triangle : assemble(request.primitive, request.vertices.size())) {
        if (request.flat_shading) triangle = {triangle[2], triangle[0], triangle[1]};
        for (const std::size_t position : triangle) {
            for (const rsx::Vec4& input :
                 fetch_vertex(memory_, request, request.vertices[position])) {
                for (const u32 component : input) {
                    inputs.push_back(host_float(component));
                }
            }
            ++vertex_count;
        }
    }
    if (vertex_count == 0) return std::nullopt;

    std::vector<float> constants;
    for (const rsx::Vec4& constant : request.vertex_program.constants) {
        for (const u32 component : constant) {
            constants.push_back(host_float(component));
        }
    }

    // The render target: the pixels from the origin to the far corner of the drawing area.
    const rsx::Rect& area = request.area;
    if (area.width == 0 || area.height == 0) return std::nullopt;
    const u32 width = area.x + area.width;
    const u32 height = area.y + area.height;
    Viewport viewport{};
    for (int i = 0; i < 4; ++i) {
        viewport.scale[i] = host_float(request.viewport_scale[i]);
        viewport.offset[i] = host_float(request.viewport_offset[i]);
    }
    viewport.size[0] = static_cast<float>(width);
    viewport.size[1] = static_cast<float>(height);

    // Guest pixels are big-endian 0xXXRRGGBB; the texture is B, G, R, A bytes, X held in A.
    const rsx::ColorSurface& target = request.target;
    std::vector<u8> pixels(std::size_t{width} * height * 4);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const u32 pixel = memory_.read<u32>(target.address + y * target.pitch + x * 4);
            u8* out = &pixels[(std::size_t{y} * width + x) * 4];
            out[0] = static_cast<u8>(pixel);
            out[1] = static_cast<u8>(pixel >> 8);
            out[2] = static_cast<u8>(pixel >> 16);
            out[3] = static_cast<u8>(pixel >> 24);
        }
    }
    MTLTextureDescriptor* texture_descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    texture_descriptor.usage = MTLTextureUsageRenderTarget;
    texture_descriptor.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [device_->device newTextureWithDescriptor:texture_descriptor];
    const MTLRegion region = MTLRegionMake2D(0, 0, width, height);
    [texture replaceRegion:region mipmapLevel:0 withBytes:pixels.data() bytesPerRow:width * 4];

    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;

    id<MTLCommandBuffer> commands = [device_->queue commandBuffer];

    // The depth buffer, loaded from guest memory through a buffer (depth textures are private).
    id<MTLTexture> depth = nil;
    id<MTLBuffer> depth_buffer = nil;
    const std::size_t depth_bytes = depth_format == MTLPixelFormatDepth16Unorm ? 2 : 4;
    if (request.depth_test) {
        const rsx::DepthSurface& surface = request.depth_test->surface;
        depth_buffer =
            [device_->device newBufferWithLength:std::size_t{width} * height * depth_bytes
                                         options:MTLResourceStorageModeShared];
        auto* values = static_cast<u8*>(depth_buffer.contents);
        for (u32 y = 0; y < height; ++y) {
            for (u32 x = 0; x < width; ++x) {
                u8* out = values + (std::size_t{y} * width + x) * depth_bytes;
                if (depth_bytes == 2) {
                    const u16 value =
                        memory_.read<u16>(surface.address + y * surface.pitch + x * 2);
                    std::memcpy(out, &value, 2);
                } else {
                    const u32 stored =
                        memory_.read<u32>(surface.address + y * surface.pitch + x * 4);
                    const float value = static_cast<float>(stored >> 8) / k_z24_maximum;
                    std::memcpy(out, &value, 4);
                }
            }
        }
        MTLTextureDescriptor* depth_descriptor =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:depth_format
                                                               width:width
                                                              height:height
                                                           mipmapped:NO];
        depth_descriptor.usage = MTLTextureUsageRenderTarget;
        depth_descriptor.storageMode = MTLStorageModePrivate;
        depth = [device_->device newTextureWithDescriptor:depth_descriptor];
        id<MTLBlitCommandEncoder> upload = [commands blitCommandEncoder];
        [upload copyFromBuffer:depth_buffer
                   sourceOffset:0
              sourceBytesPerRow:width * depth_bytes
            sourceBytesPerImage:std::size_t{width} * height * depth_bytes
                     sourceSize:MTLSizeMake(width, height, 1)
                      toTexture:depth
               destinationSlice:0
               destinationLevel:0
              destinationOrigin:MTLOriginMake(0, 0, 0)];
        [upload endEncoding];
        pass.depthAttachment.texture = depth;
        pass.depthAttachment.loadAction = MTLLoadActionLoad;
        pass.depthAttachment.storeAction = MTLStoreActionStore;
    }

    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:pipeline];
    if (request.depth_test) {
        MTLDepthStencilDescriptor* depth_state = [MTLDepthStencilDescriptor new];
        depth_state.depthCompareFunction = compare_function(request.depth_test->compare);
        depth_state.depthWriteEnabled = request.depth_test->write;
        [encoder
            setDepthStencilState:[device_->device newDepthStencilStateWithDescriptor:depth_state]];
        // The reference clamps depth to 0..1 rather than clipping.
        [encoder setDepthClipMode:MTLDepthClipModeClamp];
    }
    switch (request.cull) {
    case rsx::Cull::none: [encoder setCullMode:MTLCullModeNone]; break;
    case rsx::Cull::front: [encoder setCullMode:MTLCullModeFront]; break;
    case rsx::Cull::back: [encoder setCullMode:MTLCullModeBack]; break;
    case rsx::Cull::front_and_back: break; // returned early
    }
    [encoder
        setFrontFacingWinding:request.front_ccw ? MTLWindingCounterClockwise : MTLWindingClockwise];
    if (request.blend) {
        const u32 color = request.blend->color;
        [encoder setBlendColorRed:static_cast<float>((color >> 16) & 0xff) / 255.0f
                            green:static_cast<float>((color >> 8) & 0xff) / 255.0f
                             blue:static_cast<float>(color & 0xff) / 255.0f
                            alpha:static_cast<float>(color >> 24) / 255.0f];
    }
    for (u32 unit = 0; unit < request.textures.size(); ++unit) {
        if (!request.textures[unit]) continue;
        std::string error;
        id<MTLSamplerState> sampler = make_sampler(device_->device, *request.textures[unit], error);
        if (!sampler) {
            [encoder endEncoding];
            return error;
        }
        [encoder
            setFragmentTexture:upload_texture(device_->device, memory_, *request.textures[unit])
                       atIndex:unit];
        [encoder setFragmentSamplerState:sampler atIndex:unit];
    }
    [encoder
        setViewport:{0.0, 0.0, static_cast<double>(width), static_cast<double>(height), 0.0, 1.0}];
    [encoder setScissorRect:{area.x, area.y, area.width, area.height}];
    [encoder setVertexBytes:&viewport length:sizeof(viewport) atIndex:2];
    id<MTLBuffer> input_buffer = [device_->device newBufferWithBytes:inputs.data()
                                                              length:inputs.size() * sizeof(float)
                                                             options:MTLResourceStorageModeShared];
    id<MTLBuffer> constant_buffer =
        [device_->device newBufferWithBytes:constants.data()
                                     length:constants.size() * sizeof(float)
                                    options:MTLResourceStorageModeShared];
    [encoder setVertexBuffer:input_buffer offset:0 atIndex:0];
    [encoder setVertexBuffer:constant_buffer offset:0 atIndex:1];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:vertex_count];
    [encoder endEncoding];
    const bool writes_depth = request.depth_test && request.depth_test->write;
    if (writes_depth) {
        id<MTLBlitCommandEncoder> download = [commands blitCommandEncoder];
        [download copyFromTexture:depth
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(width, height, 1)
                            toBuffer:depth_buffer
                   destinationOffset:0
              destinationBytesPerRow:width * depth_bytes
            destinationBytesPerImage:std::size_t{width} * height * depth_bytes];
        [download endEncoding];
    }
    [commands commit];
    [commands waitUntilCompleted];
    if (commands.error) return "Metal rendering failed: " + describe(commands.error);

    // Write the drawing area back to guest memory.
    [texture getBytes:pixels.data() bytesPerRow:width * 4 fromRegion:region mipmapLevel:0];
    for (u32 y = area.y; y < height; ++y) {
        for (u32 x = area.x; x < width; ++x) {
            const u8* in = &pixels[(std::size_t{y} * width + x) * 4];
            memory_.write<u32>(target.address + y * target.pitch + x * 4,
                               u32{in[3]} << 24 | u32{in[2]} << 16 | u32{in[1]} << 8 | in[0]);
        }
    }
    if (writes_depth) write_back_depth(memory_, request, depth_buffer, width);
    return std::nullopt;
}

} // namespace gpu
