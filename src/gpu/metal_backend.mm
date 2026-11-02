#include "gpu/metal_backend.h"

#include "gpu/msl.h"
#include "gpu/vertex_input.h"
#include "rsx/methods.h"

#include <bit>
#include <map>
#include <vector>

#import <Metal/Metal.h>

namespace gpu {

struct MetalBackend::Device {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    // Compiled pipelines by their shader source and color write mask.
    std::map<std::pair<std::string, MTLColorWriteMask>, id<MTLRenderPipelineState>> pipelines;
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
    const MslTranslation msl = translate_to_msl(request.vertex_program.instructions,
                                                request.fragment_program, request.flat_shading);
    if (msl.error) return msl.error;

    // The pipeline: compile the shaders the first time this program and mask are seen.
    const MTLColorWriteMask mask = write_mask(request);
    __strong id<MTLRenderPipelineState>& pipeline = device_->pipelines[{msl.source, mask}];
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
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:pipeline];
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
    return std::nullopt;
}

} // namespace gpu
