// RSX cube: two lit, textured cubes turning over three frames, the small one cutting through the
// big one. Exercises what 3D homebrew needs beyond a triangle: matrices in vertex program
// constants, an index array, a depth buffer (where the cubes intersect), back-face culling and a
// bilinear-filtered texture (rsx_cube.vcg and .fcg, assembled by cgcomp). The emulator checks the
// frames it displays against known hashes.

#include <malloc.h>
#include <math.h>
#include <rsx/rsx.h>
#include <stdio.h>
#include <string.h>
#include <sysutil/video.h>
#include <unistd.h>

#define COMMAND_BUFFER_SIZE 0x10000
#define HOST_SIZE (1024 * 1024)
#define BUFFERS 2
#define FRAMES 3
#define TEXTURE_SIZE 64

extern const u8 rsx_cube_vp_vpo[];
extern const u8 rsx_cube_fp_fpo[];

struct vertex {
    float position[3];
    float normal[3];
    float texcoord[2];
};

// Each face: its normal and two edge directions u and v, with u x v = normal, so corners listed
// -u-v, +u-v, +u+v, -u+v run counterclockwise seen from outside.
static const float faces[6][3][3] = {
    {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}}, {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
    {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}}, {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
    {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
};

typedef float matrix[4][4];

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

// A turn about the y axis, then the x axis.
static void rotation(matrix out, float yaw, float pitch) {
    const float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
    const matrix rotate_y = {{cy, 0, sy, 0}, {0, 1, 0, 0}, {-sy, 0, cy, 0}, {0, 0, 0, 1}};
    const matrix rotate_x = {{1, 0, 0, 0}, {0, cp, -sp, 0}, {0, sp, cp, 0}, {0, 0, 0, 1}};
    multiply(out, rotate_y, rotate_x);
}

// An OpenGL-style perspective projection, with the camera 7 units in front of the cube.
static void view_projection(matrix out, float aspect) {
    const float near = 1, far = 20, f = 1.0f / tanf(30.0f * (float)M_PI / 180.0f);
    const matrix projection = {{f / aspect, 0, 0, 0},
                               {0, f, 0, 0},
                               {0, 0, (far + near) / (near - far), 2 * far * near / (near - far)},
                               {0, 0, -1, 0}};
    const matrix view = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, -7}, {0, 0, 0, 1}};
    multiply(out, projection, view);
}

// 8x8 checks in two colors, so filtering shows at their edges.
static void make_texture(u32* texels) {
    for (int y = 0; y < TEXTURE_SIZE; ++y) {
        for (int x = 0; x < TEXTURE_SIZE; ++x)
            texels[y * TEXTURE_SIZE + x] = ((x / 8 + y / 8) % 2) ? 0xffe07020 : 0xfff0e0b0;
    }
}

static void make_cube(struct vertex* vertices, u16* indices) {
    static const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (int face = 0; face < 6; ++face) {
        const float* n = faces[face][0];
        const float* u = faces[face][1];
        const float* v = faces[face][2];
        for (int corner = 0; corner < 4; ++corner) {
            struct vertex* out = &vertices[face * 4 + corner];
            const float a = corners[corner][0], b = corners[corner][1];
            for (int i = 0; i < 3; ++i) {
                out->position[i] = n[i] + a * u[i] + b * v[i];
                out->normal[i] = n[i];
            }
            out->texcoord[0] = (a + 1) / 2;
            out->texcoord[1] = (1 - b) / 2;
        }
        const u16 first = face * 4;
        const u16 face_indices[6] = {first, first + 1, first + 2, first, first + 2, first + 3};
        memcpy(&indices[face * 6], face_indices, sizeof(face_indices));
    }
}

static void set_surface(gcmContextData* context, u32 color_offset, u32 depth_offset, u32 width,
                        u32 height) {
    gcmSurface surface = {0};
    surface.type = GCM_SURFACE_TYPE_LINEAR;
    surface.antiAlias = GCM_SURFACE_CENTER_1;
    surface.colorFormat = GCM_SURFACE_X8R8G8B8;
    surface.colorTarget = GCM_SURFACE_TARGET_0;
    surface.colorLocation[0] = GCM_LOCATION_RSX;
    surface.colorOffset[0] = color_offset;
    surface.colorPitch[0] = width * 4;
    for (int i = 1; i < GCM_MAX_MRT_COUNT; ++i) {
        surface.colorLocation[i] = GCM_LOCATION_RSX;
        surface.colorPitch[i] = 64;
    }
    surface.depthFormat = GCM_SURFACE_ZETA_Z24S8;
    surface.depthLocation = GCM_LOCATION_RSX;
    surface.depthOffset = depth_offset;
    surface.depthPitch = width * 4;
    surface.width = width;
    surface.height = height;
    rsxSetSurface(context, &surface);
}

struct cube {
    float size;
    float position[3];
    float yaw, pitch;
};

// The vertex program's constants: c[0..3] the model-view-projection matrix, c[4..6] the rotation
// (for normals), c[7] the light direction and c[8] the ambient light.
static void set_constants(gcmContextData* context, const matrix view_projection,
                          const struct cube* cube, const float light[4], const float ambient[4]) {
    matrix turn, model, model_view_projection;
    rotation(turn, cube->yaw, cube->pitch);
    const matrix place = {{cube->size, 0, 0, cube->position[0]},
                          {0, cube->size, 0, cube->position[1]},
                          {0, 0, cube->size, cube->position[2]},
                          {0, 0, 0, 1}};
    multiply(model, place, turn);
    multiply(model_view_projection, view_projection, model);

    float constants[9][4];
    memcpy(constants, model_view_projection, sizeof(model_view_projection));
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c)
            constants[4 + r][c] = c < 3 ? turn[r][c] : 0;
    }
    memcpy(constants[7], light, 4 * sizeof(float));
    memcpy(constants[8], ambient, 4 * sizeof(float));
    // Not rsxSetVertexProgramConstants: in this PSL1GHT it sends each batch's first 16 floats
    // twice, and converts the rest to integers.
    rsxLoadVertexProgramParameterBlock(context, 0, 9, &constants[0][0]);
}

static void load_texture(gcmContextData* context, u32 offset) {
    gcmTexture texture = {0};
    texture.format = GCM_TEXTURE_FORMAT_A8R8G8B8 | GCM_TEXTURE_FORMAT_LIN;
    texture.mipmap = 1;
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
    texture.width = TEXTURE_SIZE;
    texture.height = TEXTURE_SIZE;
    texture.depth = 1;
    texture.location = GCM_LOCATION_RSX;
    texture.pitch = TEXTURE_SIZE * 4;
    texture.offset = offset;
    rsxLoadTexture(context, 0, &texture);
    rsxTextureControl(context, 0, GCM_TRUE, 0 << 8, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
    rsxTextureFilter(context, 0, 0, GCM_TEXTURE_LINEAR, GCM_TEXTURE_LINEAR,
                     GCM_TEXTURE_CONVOLUTION_QUINCUNX);
    rsxTextureWrapMode(context, 0, GCM_TEXTURE_REPEAT, GCM_TEXTURE_REPEAT, GCM_TEXTURE_REPEAT, 0,
                       GCM_TEXTURE_ZFUNC_LESS, 0);
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

    u32 color_offsets[BUFFERS];
    for (int i = 0; i < BUFFERS; ++i) {
        rsx_alloc(pitch * height, &color_offsets[i]);
        check("gcmSetDisplayBuffer",
              gcmSetDisplayBuffer(i, color_offsets[i], pitch, width, height));
    }
    u32 depth_offset;
    rsx_alloc(pitch * height, &depth_offset);
    gcmResetFlipStatus();

    u32 texture_offset, vertex_offset, index_offset, fragment_offset;
    make_texture(rsx_alloc(TEXTURE_SIZE * TEXTURE_SIZE * 4, &texture_offset));
    struct vertex* vertices = rsx_alloc(24 * sizeof(struct vertex), &vertex_offset);
    u16* indices = rsx_alloc(36 * sizeof(u16), &index_offset);
    make_cube(vertices, indices);

    rsxVertexProgram* vertex_program = (rsxVertexProgram*)rsx_cube_vp_vpo;
    rsxFragmentProgram* fragment_program = (rsxFragmentProgram*)rsx_cube_fp_fpo;
    void* vertex_ucode;
    void* fragment_ucode;
    u32 size;
    rsxVertexProgramGetUCode(vertex_program, &vertex_ucode, &size);
    rsxFragmentProgramGetUCode(fragment_program, &fragment_ucode, &size);
    memcpy(rsx_alloc(size, &fragment_offset), fragment_ucode, size);

    matrix view_projection_matrix;
    view_projection(view_projection_matrix, (float)width / height);
    // Light from the upper left, in front; scaled so diffuse plus ambient never exceeds 1.
    const float light_length = sqrtf(0.4f * 0.4f + 0.6f * 0.6f + 0.7f * 0.7f);
    const float light[4] = {-0.4f * 0.75f / light_length, 0.6f * 0.75f / light_length,
                            0.7f * 0.75f / light_length, 1};
    const float ambient[4] = {0.25f, 0.25f, 0.25f, 0};

    for (int frame = 0; frame < FRAMES; ++frame) {
        const int buffer = frame % BUFFERS;
        set_surface(context, color_offsets[buffer], depth_offset, width, height);

        // Clip space to pixels, and depth from -1..1 to 0..1.
        const f32 scale[4] = {width * 0.5f, height * -0.5f, 0.5f, 0.0f};
        const f32 offset[4] = {width * 0.5f, height * 0.5f, 0.5f, 0.0f};
        rsxSetViewport(context, 0, 0, width, height, 0.0f, 1.0f, scale, offset);
        rsxSetClearColor(context, 0x00203040);
        rsxSetClearDepthStencil(context, 0xffffff00);
        rsxClearSurface(context, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A |
                                     GCM_CLEAR_Z | GCM_CLEAR_S);

        rsxSetDepthTestEnable(context, GCM_TRUE);
        rsxSetDepthFunc(context, GCM_LESS);
        rsxSetDepthWriteEnable(context, GCM_TRUE);
        rsxSetFrontFace(context, GCM_FRONTFACE_CCW);
        rsxSetCullFace(context, GCM_CULL_BACK);
        rsxSetCullFaceEnable(context, GCM_TRUE);

        rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_POS, 0, vertex_offset,
                                 sizeof(struct vertex), 3, GCM_VERTEX_DATA_TYPE_F32,
                                 GCM_LOCATION_RSX);
        rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_NORMAL, 0, vertex_offset + 12,
                                 sizeof(struct vertex), 3, GCM_VERTEX_DATA_TYPE_F32,
                                 GCM_LOCATION_RSX);
        rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_TEX0, 0, vertex_offset + 24,
                                 sizeof(struct vertex), 2, GCM_VERTEX_DATA_TYPE_F32,
                                 GCM_LOCATION_RSX);
        rsxLoadVertexProgram(context, vertex_program, vertex_ucode);
        load_texture(context, texture_offset);
        rsxLoadFragmentProgramLocation(context, fragment_program, fragment_offset,
                                       GCM_LOCATION_RSX);

        const struct cube cubes[2] = {
            {1.0f, {0, 0, 0}, 0.5f + 0.35f * frame, 0.4f + 0.2f * frame},
            {0.55f, {0.9f, -0.5f, 0.6f}, -0.3f * frame, 0.8f + 0.25f * frame},
        };
        for (int i = 0; i < 2; ++i) {
            set_constants(context, view_projection_matrix, &cubes[i], light, ambient);
            rsxDrawIndexArray(context, GCM_TYPE_TRIANGLES, index_offset, 36, GCM_INDEX_TYPE_16B,
                              GCM_LOCATION_RSX);
        }

        check("gcmSetFlip", gcmSetFlip(context, buffer));
        rsxFlushBuffer(context);
        gcmSetWaitFlip(context);
        while (gcmGetFlipStatus() != 0)
            usleep(200);
        gcmResetFlipStatus();
    }

    printf("drew %d frames of two textured cubes at %ux%u\n", FRAMES, width, height);
    return 0;
}
