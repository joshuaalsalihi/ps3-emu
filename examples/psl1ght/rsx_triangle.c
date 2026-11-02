// RSX triangle: draws one triangle with a red, a green and a blue corner, using a vertex and a
// fragment program (rsx_triangle.vcg and .fcg, assembled by cgcomp). The emulator checks the
// frame it displays against a known hash.

#include <malloc.h>
#include <rsx/rsx.h>
#include <stdio.h>
#include <string.h>
#include <sysutil/video.h>
#include <unistd.h>

#define COMMAND_BUFFER_SIZE 0x10000
#define HOST_SIZE (1024 * 1024)

// The shaders, embedded by bin2s.
extern const u8 rsx_triangle_vp_vpo[];
extern const u8 rsx_triangle_fp_fpo[];

struct vertex {
    float x, y, z;
    float r, g, b, a;
};

static const struct vertex triangle[3] = {
    {0.0f, 0.75f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f},
    {-0.75f, -0.75f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f},
    {0.75f, -0.75f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f},
};

static void check(const char* what, s32 result) {
    if (result != 0) printf("%s failed: %#x\n", what, (unsigned int)result);
}

// Copies `size` bytes into RSX memory and returns their offset.
static u32 upload(const void* data, u32 size) {
    void* buffer = rsxMemalign(64, size);
    memcpy(buffer, data, size);
    u32 offset;
    check("rsxAddressToOffset", rsxAddressToOffset(buffer, &offset));
    return offset;
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

    void* color = rsxMemalign(64, pitch * height);
    u32 color_offset;
    check("rsxAddressToOffset", rsxAddressToOffset(color, &color_offset));
    check("gcmSetDisplayBuffer", gcmSetDisplayBuffer(0, color_offset, pitch, width, height));
    gcmResetFlipStatus();

    rsxVertexProgram* vertex_program = (rsxVertexProgram*)rsx_triangle_vp_vpo;
    rsxFragmentProgram* fragment_program = (rsxFragmentProgram*)rsx_triangle_fp_fpo;
    void* vertex_ucode;
    void* fragment_ucode;
    u32 size;
    rsxVertexProgramGetUCode(vertex_program, &vertex_ucode, &size);
    rsxFragmentProgramGetUCode(fragment_program, &fragment_ucode, &size);
    const u32 fragment_offset = upload(fragment_ucode, size);
    const u32 vertices = upload(triangle, sizeof(triangle));

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
    surface.depthPitch = 64;
    surface.width = width;
    surface.height = height;
    rsxSetSurface(context, &surface);

    // Clip space to pixels: x from -1..1 to 0..width, y from -1..1 to height..0.
    const f32 scale[4] = {width * 0.5f, height * -0.5f, 0.5f, 0.0f};
    const f32 offset[4] = {width * 0.5f, height * 0.5f, 0.5f, 0.0f};
    rsxSetViewport(context, 0, 0, width, height, 0.0f, 1.0f, scale, offset);
    rsxSetClearColor(context, 0x00202020);
    rsxClearSurface(context, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);

    rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_POS, 0, vertices, sizeof(struct vertex), 3,
                             GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_COLOR0, 0, vertices + 12,
                             sizeof(struct vertex), 4, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxLoadVertexProgram(context, vertex_program, vertex_ucode);
    rsxLoadFragmentProgramLocation(context, fragment_program, fragment_offset, GCM_LOCATION_RSX);
    rsxDrawVertexArray(context, GCM_TYPE_TRIANGLES, 0, 3);

    check("gcmSetFlip", gcmSetFlip(context, 0));
    rsxFlushBuffer(context);
    gcmSetWaitFlip(context);
    while (gcmGetFlipStatus() != 0)
        usleep(200);

    printf("drew a triangle at %ux%u\n", width, height);
    return 0;
}
