// RSX scene: a checkered floor running to the horizon, a cut-out ring standing on it, and two
// panes of tinted glass in front. Exercises the texture and pixel features 3D homebrew uses
// beyond rsx_cube:
//  - the floor's texture is swizzled and mipmapped, sampled with trilinear filtering, so the
//    checks blur smoothly into the distance;
//  - the ring's texture is 16-bit (A4R4G4B4), in main memory, and its transparent texels are
//    dropped by the alpha test;
//  - the glass is blended over what's behind it (and the far pane under the near one).
// It draws into a viewport and scissor smaller than the screen. The emulator checks the frame
// against a known hash.

#include <malloc.h>
#include <math.h>
#include <rsx/rsx.h>
#include <stdio.h>
#include <string.h>
#include <sysutil/video.h>
#include <unistd.h>

#define COMMAND_BUFFER_SIZE 0x10000
#define HOST_SIZE (1024 * 1024)
#define FLOOR_SIZE 64
#define FLOOR_LEVELS 7 // 64x64 down to 1x1
#define RING_SIZE 32

extern const u8 rsx_scene_vp_vpo[];
extern const u8 rsx_scene_fp_fpo[];

struct vertex {
    float position[3];
    float color[4];
    float texcoord[2];
};

typedef float matrix[4][4];

// Each quad is drawn as a triangle fan.
static const struct vertex quads[][4] = {
    // The floor, 20 units wide and running from just in front of the camera to 40 units away,
    // its texture repeating every 2 units.
    {{{-10, 0, -0.5f}, {1, 1, 1, 1}, {-5, -0.25f}},
     {{10, 0, -0.5f}, {1, 1, 1, 1}, {5, -0.25f}},
     {{10, 0, -40}, {1, 1, 1, 1}, {5, -20}},
     {{-10, 0, -40}, {1, 1, 1, 1}, {-5, -20}}},
    // The ring.
    {{{-2.6f, 0, -8}, {1, 1, 1, 1}, {0, 1}},
     {{-0.6f, 0, -8}, {1, 1, 1, 1}, {1, 1}},
     {{-0.6f, 2, -8}, {1, 1, 1, 1}, {1, 0}},
     {{-2.6f, 2, -8}, {1, 1, 1, 1}, {0, 0}}},
    // The far pane of glass, blue, then the near one, red.
    {{{-0.2f, 0, -6.5f}, {0.3f, 0.6f, 1, 0.5f}, {0, 0}},
     {{1.6f, 0, -6.5f}, {0.3f, 0.6f, 1, 0.5f}, {0, 0}},
     {{1.6f, 1.4f, -6.5f}, {0.3f, 0.6f, 1, 0.5f}, {0, 0}},
     {{-0.2f, 1.4f, -6.5f}, {0.3f, 0.6f, 1, 0.5f}, {0, 0}}},
    {{{0.8f, 0.3f, -5}, {1, 0.35f, 0.2f, 0.5f}, {0, 0}},
     {{2.6f, 0.3f, -5}, {1, 0.35f, 0.2f, 0.5f}, {0, 0}},
     {{2.6f, 1.7f, -5}, {1, 0.35f, 0.2f, 0.5f}, {0, 0}},
     {{0.8f, 1.7f, -5}, {1, 0.35f, 0.2f, 0.5f}, {0, 0}}},
};
enum { FLOOR, RING, FAR_PANE, NEAR_PANE };

static void check(const char* what, s32 result) {
    if (result != 0) printf("%s failed: %#x\n", what, (unsigned int)result);
}

static void* rsx_alloc(u32 size, u32* offset) {
    void* buffer = rsxMemalign(128, size);
    check("rsxAddressToOffset", rsxAddressToOffset(buffer, offset));
    return buffer;
}

static void multiply(matrix out, const matrix a, const matrix b) {
    matrix result;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            result[r][c] = 0;
            for (int k = 0; k < 4; ++k)
                result[r][c] += a[r][k] * b[k][c];
        }
    }
    memcpy(out, result, sizeof(result));
}

// An OpenGL-style perspective projection, from a camera 1 unit above the floor, looking a little
// down.
static void view_projection(matrix out, float aspect) {
    const float near = 1, far = 50, f = 1.0f / tanf(30.0f * (float)M_PI / 180.0f);
    const float c = cosf(0.2f), s = sinf(0.2f);
    const matrix projection = {{f / aspect, 0, 0, 0},
                               {0, f, 0, 0},
                               {0, 0, (far + near) / (near - far), 2 * far * near / (near - far)},
                               {0, 0, -1, 0}};
    const matrix tilt = {{1, 0, 0, 0}, {0, c, -s, 0}, {0, s, c, 0}, {0, 0, 0, 1}};
    const matrix lift = {{1, 0, 0, 0}, {0, 1, 0, -1}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    matrix view;
    multiply(view, tilt, lift);
    multiply(out, projection, view);
}

// A swizzled texture stores texel (x, y) at the index whose bits interleave x's and y's, x's
// lowest first (both sizes here are equal powers of two).
static u32 swizzle(u32 x, u32 y) {
    u32 index = 0;
    for (u32 bit = 0; bit < 16; ++bit) {
        index |= ((x >> bit) & 1) << (2 * bit);
        index |= ((y >> bit) & 1) << (2 * bit + 1);
    }
    return index;
}

// The floor's 8x8 checks in two greens, then each smaller level as the average of 2x2 texels.
static void make_floor_texture(u32* texels) {
    static u32 levels[FLOOR_LEVELS][FLOOR_SIZE * FLOOR_SIZE];
    for (u32 y = 0; y < FLOOR_SIZE; ++y) {
        for (u32 x = 0; x < FLOOR_SIZE; ++x)
            levels[0][y * FLOOR_SIZE + x] = ((x / 8 + y / 8) % 2) ? 0xff2e5d34 : 0xffb8d8a0;
    }
    for (int level = 1; level < FLOOR_LEVELS; ++level) {
        const u32 size = FLOOR_SIZE >> level;
        const u32* above = levels[level - 1];
        for (u32 y = 0; y < size; ++y) {
            for (u32 x = 0; x < size; ++x) {
                u32 average = 0;
                for (int shift = 0; shift < 32; shift += 8) {
                    u32 sum = 0;
                    for (u32 i = 0; i < 4; ++i)
                        sum += (above[(2 * y + i / 2) * size * 2 + 2 * x + i % 2] >> shift) & 0xff;
                    average |= ((sum + 2) / 4) << shift;
                }
                levels[level][y * size + x] = average;
            }
        }
    }
    for (int level = 0; level < FLOOR_LEVELS; ++level) {
        const u32 size = FLOOR_SIZE >> level;
        for (u32 y = 0; y < size; ++y) {
            for (u32 x = 0; x < size; ++x)
                texels[swizzle(x, y)] = levels[level][y * size + x];
        }
        texels += size * size;
    }
}

// An orange ring, opaque, on fully transparent texels.
static void make_ring_texture(u16* texels) {
    for (int y = 0; y < RING_SIZE; ++y) {
        for (int x = 0; x < RING_SIZE; ++x) {
            const int dx = 2 * x + 1 - RING_SIZE, dy = 2 * y + 1 - RING_SIZE;
            const int distance_squared = dx * dx + dy * dy;
            const int inside = distance_squared >= 14 * 14 && distance_squared <= 30 * 30;
            texels[y * RING_SIZE + x] = inside ? 0xffa3 : 0x0000;
        }
    }
}

static void load_texture(gcmContextData* context, u8 format, u32 size, u32 pitch, u8 levels,
                         u32 location, u32 offset, u8 minify, u8 magnify, u8 wrap) {
    gcmTexture texture = {0};
    texture.format = format;
    texture.mipmap = levels;
    texture.dimension = GCM_TEXTURE_DIMS_2D;
    texture.cubemap = GCM_FALSE;
    texture.remap = GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_B_SHIFT |
                    GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_G_SHIFT |
                    GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_R_SHIFT |
                    GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_A_SHIFT |
                    GCM_TEXTURE_REMAP_COLOR_B << GCM_TEXTURE_REMAP_COLOR_B_SHIFT |
                    GCM_TEXTURE_REMAP_COLOR_G << GCM_TEXTURE_REMAP_COLOR_G_SHIFT |
                    GCM_TEXTURE_REMAP_COLOR_R << GCM_TEXTURE_REMAP_COLOR_R_SHIFT |
                    GCM_TEXTURE_REMAP_COLOR_A << GCM_TEXTURE_REMAP_COLOR_A_SHIFT;
    texture.width = size;
    texture.height = size;
    texture.depth = 1;
    texture.location = location;
    texture.pitch = pitch;
    texture.offset = offset;
    rsxInvalidateTextureCache(context, GCM_INVALIDATE_TEXTURE);
    rsxLoadTexture(context, 0, &texture);
    rsxTextureControl(context, 0, GCM_TRUE, 0 << 8, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
    rsxTextureFilter(context, 0, 0, minify, magnify, GCM_TEXTURE_CONVOLUTION_QUINCUNX);
    rsxTextureWrapMode(context, 0, wrap, wrap, wrap, 0, GCM_TEXTURE_ZFUNC_LESS, 0);
}

static void draw_quad(gcmContextData* context, int quad) {
    rsxDrawVertexArray(context, GCM_TYPE_TRIANGLE_FAN, quad * 4, 4);
}

int main(void) {
    void* host = memalign(1024 * 1024, HOST_SIZE);
    gcmContextData* context;
    check("rsxInit", rsxInit(&context, COMMAND_BUFFER_SIZE, HOST_SIZE, host));

    videoState state;
    check("videoGetState", videoGetState(0, 0, &state));
    videoResolution resolution;
    check("videoGetResolution", videoGetResolution(state.displayMode.resolution, &resolution));
    const u32 width = resolution.width, height = resolution.height, pitch = width * 4;
    videoConfiguration video = {0};
    video.resolution = state.displayMode.resolution;
    video.format = VIDEO_BUFFER_FORMAT_XRGB;
    video.aspect = state.displayMode.aspect;
    video.pitch = pitch;
    check("videoConfigure", videoConfigure(0, &video, NULL, 0));
    gcmSetFlipMode(GCM_FLIP_VSYNC);

    u32 color_offset, depth_offset;
    rsx_alloc(pitch * height, &color_offset);
    rsx_alloc(pitch * height, &depth_offset);
    check("gcmSetDisplayBuffer", gcmSetDisplayBuffer(0, color_offset, pitch, width, height));
    gcmResetFlipStatus();

    // Textures: the floor's levels in local memory; the ring's in main memory, in the half of
    // the RSX's main memory the command buffer doesn't use; a white texel for the glass, which
    // only has its vertex color.
    u32 floor_offset, white_offset, vertex_offset, fragment_offset, ring_offset;
    make_floor_texture(rsx_alloc(FLOOR_SIZE * FLOOR_SIZE * 4 * 2, &floor_offset));
    u16* ring = (u16*)((u8*)host + HOST_SIZE / 2);
    make_ring_texture(ring);
    check("rsxAddressToOffset", rsxAddressToOffset(ring, &ring_offset));
    *(u32*)rsx_alloc(4, &white_offset) = 0xffffffff;
    memcpy(rsx_alloc(sizeof(quads), &vertex_offset), quads, sizeof(quads));

    rsxVertexProgram* vertex_program = (rsxVertexProgram*)rsx_scene_vp_vpo;
    rsxFragmentProgram* fragment_program = (rsxFragmentProgram*)rsx_scene_fp_fpo;
    void* vertex_ucode;
    void* fragment_ucode;
    u32 size;
    rsxVertexProgramGetUCode(vertex_program, &vertex_ucode, &size);
    rsxFragmentProgramGetUCode(fragment_program, &fragment_ucode, &size);
    memcpy(rsx_alloc(size, &fragment_offset), fragment_ucode, size);

    gcmSurface surface = {0};
    surface.type = GCM_SURFACE_TYPE_LINEAR;
    surface.antiAlias = GCM_SURFACE_CENTER_1;
    surface.colorFormat = GCM_SURFACE_X8R8G8B8;
    surface.colorTarget = GCM_SURFACE_TARGET_0;
    surface.colorLocation[0] = GCM_LOCATION_RSX;
    surface.colorOffset[0] = color_offset;
    surface.colorPitch[0] = pitch;
    for (int i = 1; i < GCM_MAX_MRT_COUNT; ++i) {
        surface.colorLocation[i] = GCM_LOCATION_RSX;
        surface.colorPitch[i] = 64;
    }
    surface.depthFormat = GCM_SURFACE_ZETA_Z16;
    surface.depthLocation = GCM_LOCATION_RSX;
    surface.depthOffset = depth_offset;
    surface.depthPitch = width * 2;
    surface.width = width;
    surface.height = height;
    rsxSetSurface(context, &surface);

    // A dark frame around a 640x360 view of the scene, in the middle of the screen.
    rsxSetClearColor(context, 0x00101010);
    rsxClearSurface(context, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
    const u16 view_x = (width - 640) / 2, view_y = (height - 360) / 2;
    const f32 scale[4] = {320.0f, -180.0f, 0.5f, 0.0f};
    const f32 offset[4] = {view_x + 320.0f, view_y + 180.0f, 0.5f, 0.0f};
    rsxSetViewport(context, view_x, view_y, 640, 360, 0.0f, 1.0f, scale, offset);
    rsxSetScissor(context, view_x, view_y, 640, 360);
    rsxSetClearColor(context, 0x0090b8e0);
    rsxSetClearDepthStencil(context, 0xffff);
    rsxClearSurface(context, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A | GCM_CLEAR_Z);

    matrix model_view_projection;
    view_projection(model_view_projection, 640.0f / 360.0f);
    rsxLoadVertexProgram(context, vertex_program, vertex_ucode);
    // Not rsxSetVertexProgramConstants: in this PSL1GHT it sends each batch's first 16 floats
    // twice, and converts the rest to integers.
    rsxLoadVertexProgramParameterBlock(context, 0, 4, &model_view_projection[0][0]);
    rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_POS, 0, vertex_offset,
                             sizeof(struct vertex), 3, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_COLOR0, 0, vertex_offset + 12,
                             sizeof(struct vertex), 4, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_TEX0, 0, vertex_offset + 28,
                             sizeof(struct vertex), 2, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxLoadFragmentProgramLocation(context, fragment_program, fragment_offset, GCM_LOCATION_RSX);
    rsxSetDepthTestEnable(context, GCM_TRUE);
    rsxSetDepthFunc(context, GCM_LESS);
    rsxSetDepthWriteEnable(context, GCM_TRUE);

    // The floor, then the ring, whose transparent texels the alpha test drops.
    load_texture(context, GCM_TEXTURE_FORMAT_A8R8G8B8 | GCM_TEXTURE_FORMAT_SWZ, FLOOR_SIZE, 0,
                 FLOOR_LEVELS, GCM_LOCATION_RSX, floor_offset, GCM_TEXTURE_LINEAR_MIPMAP_LINEAR,
                 GCM_TEXTURE_LINEAR, GCM_TEXTURE_REPEAT);
    draw_quad(context, FLOOR);
    load_texture(context, GCM_TEXTURE_FORMAT_A4R4G4B4 | GCM_TEXTURE_FORMAT_LIN, RING_SIZE,
                 RING_SIZE * 2, 1, GCM_LOCATION_CELL, ring_offset, GCM_TEXTURE_NEAREST,
                 GCM_TEXTURE_NEAREST, GCM_TEXTURE_CLAMP_TO_EDGE);
    rsxSetAlphaFunc(context, GCM_GREATER, 0x80);
    rsxSetAlphaTestEnable(context, GCM_TRUE);
    draw_quad(context, RING);
    rsxSetAlphaTestEnable(context, GCM_FALSE);

    // The glass, far to near, blended over what's behind it and leaving the depth alone.
    load_texture(context, GCM_TEXTURE_FORMAT_A8R8G8B8 | GCM_TEXTURE_FORMAT_LIN, 1, 4, 1,
                 GCM_LOCATION_RSX, white_offset, GCM_TEXTURE_NEAREST, GCM_TEXTURE_NEAREST,
                 GCM_TEXTURE_CLAMP_TO_EDGE);
    rsxSetDepthWriteEnable(context, GCM_FALSE);
    rsxSetBlendFunc(context, GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA, GCM_ZERO, GCM_ONE);
    rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
    rsxSetBlendEnable(context, GCM_TRUE);
    draw_quad(context, FAR_PANE);
    draw_quad(context, NEAR_PANE);
    rsxSetBlendEnable(context, GCM_FALSE);

    check("gcmSetFlip", gcmSetFlip(context, 0));
    rsxFlushBuffer(context);
    gcmSetWaitFlip(context);
    while (gcmGetFlipStatus() != 0)
        usleep(200);

    printf("drew a scene with mipmaps, an alpha-tested ring and blended glass\n");
    return 0;
}
