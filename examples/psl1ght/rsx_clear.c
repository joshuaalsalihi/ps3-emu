// RSX clear: sets up the display with two buffers, clears each to a different color with the RSX,
// and flips to it. The emulator checks the frames it displays against known hashes.

#include <malloc.h>
#include <rsx/rsx.h>
#include <stdio.h>
#include <sysutil/video.h>
#include <unistd.h>

#define COMMAND_BUFFER_SIZE 0x10000
#define HOST_SIZE (1024 * 1024) // main memory the RSX can reach; holds the command buffer
#define BUFFERS 2

static const u32 colors[BUFFERS] = {0x00336699, 0x00ff8000};

static void check(const char* what, s32 result) {
    if (result != 0) printf("%s failed: %#x\n", what, (unsigned int)result);
}

static void wait_for_flip(void) {
    while (gcmGetFlipStatus() != 0)
        usleep(200);
    gcmResetFlipStatus();
}

int main(void) {
    void* host = memalign(1024 * 1024, HOST_SIZE);
    gcmContextData* context;
    check("rsxInit", rsxInit(&context, COMMAND_BUFFER_SIZE, HOST_SIZE, host));

    videoState state;
    check("videoGetState", videoGetState(0, 0, &state));
    videoResolution resolution;
    check("videoGetResolution", videoGetResolution(state.displayMode.resolution, &resolution));
    const u32 pitch = resolution.width * 4;

    videoConfiguration video = {0};
    video.resolution = state.displayMode.resolution;
    video.format = VIDEO_BUFFER_FORMAT_XRGB;
    video.aspect = state.displayMode.aspect;
    video.pitch = pitch;
    check("videoConfigure", videoConfigure(0, &video, NULL, 0));
    gcmSetFlipMode(GCM_FLIP_VSYNC);

    u32 offsets[BUFFERS];
    for (int i = 0; i < BUFFERS; ++i) {
        void* buffer = rsxMemalign(64, pitch * resolution.height);
        check("rsxAddressToOffset", rsxAddressToOffset(buffer, &offsets[i]));
        check("gcmSetDisplayBuffer",
              gcmSetDisplayBuffer(i, offsets[i], pitch, resolution.width, resolution.height));
    }
    gcmResetFlipStatus();

    for (int i = 0; i < BUFFERS; ++i) {
        gcmSurface surface = {0};
        surface.type = GCM_SURFACE_TYPE_LINEAR;
        surface.antiAlias = GCM_SURFACE_CENTER_1;
        surface.colorFormat = GCM_SURFACE_X8R8G8B8;
        surface.colorTarget = GCM_SURFACE_TARGET_0;
        surface.colorLocation[0] = GCM_LOCATION_RSX;
        surface.colorOffset[0] = offsets[i];
        surface.colorPitch[0] = pitch;
        for (int j = 1; j < GCM_MAX_MRT_COUNT; ++j) {
            surface.colorLocation[j] = GCM_LOCATION_RSX;
            surface.colorPitch[j] = 64;
        }
        surface.depthFormat = GCM_SURFACE_ZETA_Z16;
        surface.depthLocation = GCM_LOCATION_RSX;
        surface.depthPitch = 64;
        surface.width = resolution.width;
        surface.height = resolution.height;
        rsxSetSurface(context, &surface);

        rsxSetClearColor(context, colors[i]);
        rsxClearSurface(context, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);

        check("gcmSetFlip", gcmSetFlip(context, i));
        rsxFlushBuffer(context);
        gcmSetWaitFlip(context);
        wait_for_flip();
    }

    printf("display: %ux%u, %d frames flipped\n", resolution.width, resolution.height, BUFFERS);
    return 0;
}
