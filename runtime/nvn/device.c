/*
 * device.c - NVNdevice, NVNqueue, NVNsync, NVNevent and their builders.
 *
 * Builders are plain "fill in the fields, then Initialize" structs. For the queue nothing really
 * happens: submitting a command list runs it right away (exec.c), so every fence is signalled
 * as soon as it is submitted.
 */
#include "nvn.h"
#include <stdio.h>
#include <time.h>

/* ---- device ---- */

/* nvnDeviceGetInteger(device, NVNdeviceInfo, int *value): limits and alignments the game
 * asks about, in NVNdeviceInfo order. */
static const int32_t device_info[] = {
    [0] = 53, [1] = 207,                         /* API major / minor version */
    [2] = 14,                                    /* uniform buffer bindings per stage */
    [3] = 65536,                                 /* max uniform buffer size */
    [4] = 256,                                   /* uniform buffer alignment */
    [5] = 8, [6] = 16, [7] = 4, [8] = 16,        /* color buffers, vertex buffers, tf buffers, vertex attribs */
    [9] = 32, [10] = 16, [11] = 4, [12] = 4,     /* textures/stage, storage buffers/stage, ... */
    [13] = 4, [14] = 16, [15] = 32, [16] = 32,   /* storage buffer / transform feedback / counter alignment */
    [17] = 256, [18] = 256,                      /* reserved texture / sampler descriptors */
    [19] = 4, [20] = 8,                          /* command buffer command / control memory alignment */
    [21] = 0x10000, [22] = 0x1000,               /* minimum command / control memory chunk */
    [23] = 256, [24] = 256,                      /* (texture / sampler descriptor sizes are 32 below) */
    [25] = 4096,                                 /* memory pool page size */
    [26] = 0x20000,                              /* max shader scratch memory ... */
    [27] = 16, [28] = 1536, [29] = 1024, [30] = 64, [31] = 1536,
    [32] = 0x7FFFFFFF, [33] = 65535, [34] = 65535,
    [35] = 8, [36] = 1048576, [37] = 4096, [38] = 16, [39] = 0x10000,
    [40] = 1, [41] = 1, [42] = 1,                /* min/max filtering, stencil8 format, ASTC */
    [43] = 0x40000, [44] = 15, [45] = 2048,
    [46] = 1, [47] = 5, [48] = 1, [49] = 15,
    [50] = 256, [51] = 1, [52] = 0, [53] = 0, [54] = 0, [55] = 256, [56] = 1, [57] = 0x1000,
    [58] = 1, [59] = 1, [60] = 1, [61] = 1, [62] = 1, [63] = 1, [64] = 1,
};

static uint32_t window_origin_mode, depth_mode;

static void nvnDeviceBuilderSetDefaults(CPU *c) { g_st32(ARG(0), 0); }
static void nvnDeviceBuilderSetFlags(CPU *c)    { g_st32(ARG(0), (uint32_t)ARG(1)); }
static void nvnDeviceBuilderGetFlags(CPU *c)    { RET(g_ld32(ARG(0))); }
static void nvnDeviceInitialize(CPU *c)         { RET(1); }
static void nvnDeviceFinalize(CPU *c)           { (void)c; }
void nvn_nothing(CPU *c)                        { (void)c; }
void nvn_true(CPU *c)                           { RET(1); }
void nvn_zero(CPU *c)                           { RET(0); }

static void nvnDeviceGetInteger(CPU *c) {
    uint32_t which = (uint32_t)ARG(1);
    int32_t v = which < sizeof device_info / sizeof *device_info ? device_info[which] : 0;
    if (which >= sizeof device_info / sizeof *device_info) fprintf(stderr, "nvn: unknown device info %u\n", which);
    g_st32(ARG(2), (uint32_t)v);
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
/* GPU timestamps count in units of 625/384 ns (NVN converts them with * 625 / 384).
 * Counters we write use the same unit. */
uint64_t nvn_gpu_ticks(void) { return (uint64_t)((unsigned __int128)now_ns() * 384 / 625); }

static void nvnDeviceGetCurrentTimestampInNanoseconds(CPU *c) { RET(now_ns()); }
/* (device, const NVNcounterData *): {u64 counter, u64 timestamp} */
static void nvnDeviceGetTimestampInNanoseconds(CPU *c) {
    RET((uint64_t)((unsigned __int128)g_ld64(ARG(1) + 8) * 625 / 384));
}

/* Handles as the hardware wants them: texture descriptor index | sampler index << 20 */
static void nvnDeviceGetTextureHandle(CPU *c)         { RET((ARG(1) & 0xFFFFF) | (ARG(2) & 0xFFF) << 20); }
static void nvnDeviceGetTexelFetchHandle(CPU *c)      { RET(ARG(1) & 0xFFFFF); }
static void nvnDeviceGetImageHandle(CPU *c)           { RET(ARG(1) & 0xFFFFF); }
static void nvnDeviceGetSeparateTextureHandle(CPU *c) { RET(ARG(1) & 0xFFFFF); }
static void nvnDeviceGetSeparateSamplerHandle(CPU *c) { RET((ARG(1) & 0xFFF) << 20); }

static void nvnDeviceSetWindowOriginMode(CPU *c) {
    window_origin_mode = (uint32_t)ARG(1);
    fprintf(stderr, "nvn: window origin mode %u (0 lower left, 1 upper left)\n", window_origin_mode);
}
static void nvnDeviceGetWindowOriginMode(CPU *c) { RET(window_origin_mode); }
/* 0: clip-space z runs -w..w (NEAR_IS_MINUS_W, OpenGL's); 1: 0..w (NEAR_IS_ZERO, like Vulkan) */
static void nvnDeviceSetDepthMode(CPU *c) {
    depth_mode = (uint32_t)ARG(1);
    fprintf(stderr, "nvn: depth mode %u (0 near is -w, 1 near is 0)\n", depth_mode);
}
uint32_t nvn_depth_mode(void) { return depth_mode; }
static void nvnDeviceGetDepthMode(CPU *c)        { RET(depth_mode); }
static void nvnDeviceGenerateDebugDomainId(CPU *c) { static int next = 1; RET(next++); }

/* ---- queue ---- */

static void nvnQueueBuilderSetDevice(CPU *c) { ((QueueBuilder *)gptr(ARG(0)))->device = ARG(1); }
static void nvnQueueBuilderSetDefaults(CPU *c) {
    QueueBuilder *b = gptr(ARG(0));
    uint64_t device = b->device;
    *b = (QueueBuilder){ .device = device, .command_size = 0x10000, .compute_size = 0x40000,
                         .control_size = 0x4000, .flush_threshold = 0x8000, .priority = 1 };
}
#define QB ((QueueBuilder *)gptr(ARG(0)))
static void nvnQueueBuilderSetFlags(CPU *c)                 { QB->flags = (uint32_t)ARG(1); }
static void nvnQueueBuilderSetCommandMemorySize(CPU *c)     { QB->command_size = (uint32_t)ARG(1); }
static void nvnQueueBuilderSetComputeMemorySize(CPU *c)     { QB->compute_size = (uint32_t)ARG(1); }
static void nvnQueueBuilderSetControlMemorySize(CPU *c)     { QB->control_size = (uint32_t)ARG(1); }
static void nvnQueueBuilderSetCommandFlushThreshold(CPU *c) { QB->flush_threshold = (uint32_t)ARG(1); }
static void nvnQueueBuilderSetQueuePriority(CPU *c)         { QB->priority = (uint32_t)ARG(1); }
static void nvnQueueBuilderSetQueueMemory(CPU *c)           { QB->memory = ARG(1); QB->memory_size = ARG(2); }
static void nvnQueueBuilderGetDevice(CPU *c)                { RET(QB->device); }
static void nvnQueueBuilderGetFlags(CPU *c)                 { RET(QB->flags); }
static void nvnQueueBuilderGetQueuePriority(CPU *c)         { RET(QB->priority); }
static void nvnQueueBuilderGetMemory(CPU *c)                { RET(QB->memory); }
/* (builder, int *size) -> bool */
static void nvnQueueBuilderGetCommandMemorySize(CPU *c)     { g_st32(ARG(1), QB->command_size); RET(1); }
static void nvnQueueBuilderGetComputeMemorySize(CPU *c)     { g_st32(ARG(1), QB->compute_size); RET(1); }
static void nvnQueueBuilderGetControlMemorySize(CPU *c)     { g_st32(ARG(1), QB->control_size); RET(1); }
static void nvnQueueBuilderGetCommandFlushThreshold(CPU *c) { g_st32(ARG(1), QB->flush_threshold); RET(1); }
/* how much queue memory the queue needs (the game allocates it), and how much it was given */
static void nvnQueueBuilderGetQueueMemorySize(CPU *c)       { RET(QB->memory ? QB->memory_size : 0x10000); }
static void nvnQueueBuilderGetMemorySize(CPU *c)            { RET(0x10000); }

static void nvnQueueInitialize(CPU *c)                  { RET(1); }
static void nvnQueueGetError(CPU *c)                    { RET(0); }   /* NVN_QUEUE_GET_ERROR_RESULT_GPU_NO_ERROR */

/* (queue, int count, const NVNcommandHandle *handles) */
static void nvnQueueSubmitCommands(CPU *c) {
    int n = (int)ARG(1);
    uint64_t handles = ARG(2);
    for (int i = 0; i < n; i++) nvn_execute(g_ld64(handles + 8ull * i));
}

/* ---- sync ---- */

static void nvnSyncInitialize(CPU *c) { *(Sync *)gptr(ARG(0)) = (Sync){ 0 }; RET(1); }
/* (queue, sync, condition, flags): signalled when the GPU has done the work submitted before it */
static void nvnQueueFenceSync(CPU *c) { nvn_sync_fence(gptr(ARG(1))); }
static void nvnQueueWaitSync(CPU *c)  { RET(1); }   /* (one GPU queue: its work is in order anyway) */
/* (sync, timeout ns) -> NVNsyncWaitResult; 0 = already signalled, 2 = timeout expired */
static void nvnSyncWait(CPU *c) { RET(nvn_sync_wait(gptr(ARG(0)), ARG(1)) == 0 ? 0 : 2); }
static void nvnQueueFinish(CPU *c) { (void)c; nvn_queue_finish(); }

/* ---- event: a u32 in a memory pool ---- */

static void nvnEventBuilderSetDefaults(CPU *c) { *(EventBuilder *)gptr(ARG(0)) = (EventBuilder){ 0 }; }
static void nvnEventBuilderSetStorage(CPU *c)  { *(EventBuilder *)gptr(ARG(0)) = (EventBuilder){ ARG(1), ARG(2) }; }
/* (builder, int64_t *offset) -> NVNmemoryPool * */
static void nvnEventBuilderGetStorage(CPU *c) {
    EventBuilder *b = gptr(ARG(0));
    if (ARG(1)) g_st64(ARG(1), b->offset);
    RET(b->pool);
}
static void nvnEventBuilderGetMemoryPool(CPU *c)   { RET(((EventBuilder *)gptr(ARG(0)))->pool); }
static void nvnEventBuilderGetMemoryOffset(CPU *c) { RET(((EventBuilder *)gptr(ARG(0)))->offset); }
static void nvnEventInitialize(CPU *c) {
    EventBuilder *b = gptr(ARG(1));
    *(Event *)gptr(ARG(0)) = (Event){ pool_address(b->pool, b->offset), b->pool, b->offset };
    RET(1);
}
static void nvnEventGetValue(CPU *c)        { RET(g_ld32(((Event *)gptr(ARG(0)))->address)); }
/* (event, NVNeventSignalMode mode, u32 value); mode 1 = add */
static void nvnEventSignal(CPU *c) {
    uint64_t a = ((Event *)gptr(ARG(0)))->address;
    g_st32(a, ARG(1) == 1 ? g_ld32(a) + (uint32_t)ARG(2) : (uint32_t)ARG(2));
}
static void nvnEventGetMemoryPool(CPU *c)   { RET(((Event *)gptr(ARG(0)))->pool); }
static void nvnEventGetMemoryOffset(CPU *c) { RET(((Event *)gptr(ARG(0)))->offset); }

const NvnEntry nvn_device_functions[] = {
    NVN_ENTRY(nvnDeviceBuilderSetDefaults), NVN_ENTRY(nvnDeviceBuilderSetFlags), NVN_ENTRY(nvnDeviceBuilderGetFlags),
    NVN_ENTRY(nvnDeviceInitialize), NVN_ENTRY(nvnDeviceFinalize), NVN_ENTRY(nvnDeviceGetInteger),
    { "nvnDeviceSetDebugLabel", nvn_nothing },
    NVN_ENTRY(nvnDeviceGetCurrentTimestampInNanoseconds), NVN_ENTRY(nvnDeviceGetTimestampInNanoseconds),
    { "nvnDeviceSetIntermediateShaderCache", nvn_nothing },
    NVN_ENTRY(nvnDeviceGetTextureHandle), NVN_ENTRY(nvnDeviceGetTexelFetchHandle), NVN_ENTRY(nvnDeviceGetImageHandle),
    NVN_ENTRY(nvnDeviceGetSeparateTextureHandle), NVN_ENTRY(nvnDeviceGetSeparateSamplerHandle),
    { "nvnDeviceInstallDebugCallback", nvn_nothing },
    NVN_ENTRY(nvnDeviceGenerateDebugDomainId),
    NVN_ENTRY(nvnDeviceSetWindowOriginMode), NVN_ENTRY(nvnDeviceGetWindowOriginMode),
    NVN_ENTRY(nvnDeviceSetDepthMode), NVN_ENTRY(nvnDeviceGetDepthMode),
    { "nvnDeviceRegisterFastClearColor", nvn_true }, { "nvnDeviceRegisterFastClearColori", nvn_true },
    { "nvnDeviceRegisterFastClearColorui", nvn_true }, { "nvnDeviceRegisterFastClearDepth", nvn_true },
    { "nvnDeviceApplyDeferredFinalizes", nvn_nothing }, { "nvnDeviceFinalizeCommandHandle", nvn_nothing },
    { "nvnDeviceWalkDebugDatabase", nvn_nothing }, { "nvnDeviceIsExternalDebuggerAttached", nvn_zero },

    NVN_ENTRY(nvnQueueBuilderSetDevice), NVN_ENTRY(nvnQueueBuilderSetDefaults), NVN_ENTRY(nvnQueueBuilderSetFlags),
    NVN_ENTRY(nvnQueueBuilderSetCommandMemorySize), NVN_ENTRY(nvnQueueBuilderSetComputeMemorySize),
    NVN_ENTRY(nvnQueueBuilderSetControlMemorySize), NVN_ENTRY(nvnQueueBuilderSetCommandFlushThreshold),
    NVN_ENTRY(nvnQueueBuilderSetQueuePriority), NVN_ENTRY(nvnQueueBuilderSetQueueMemory),
    NVN_ENTRY(nvnQueueBuilderGetDevice), NVN_ENTRY(nvnQueueBuilderGetFlags), NVN_ENTRY(nvnQueueBuilderGetQueuePriority),
    NVN_ENTRY(nvnQueueBuilderGetMemory), NVN_ENTRY(nvnQueueBuilderGetCommandMemorySize),
    NVN_ENTRY(nvnQueueBuilderGetComputeMemorySize), NVN_ENTRY(nvnQueueBuilderGetControlMemorySize),
    NVN_ENTRY(nvnQueueBuilderGetCommandFlushThreshold), NVN_ENTRY(nvnQueueBuilderGetQueueMemorySize),
    NVN_ENTRY(nvnQueueBuilderGetMemorySize),
    NVN_ENTRY(nvnQueueInitialize), { "nvnQueueFinalize", nvn_nothing }, { "nvnQueueSetDebugLabel", nvn_nothing },
    NVN_ENTRY(nvnQueueGetError), { "nvnQueueGetTotalCommandMemoryUsed", nvn_zero },
    { "nvnQueueGetTotalControlMemoryUsed", nvn_zero }, { "nvnQueueGetTotalComputeMemoryUsed", nvn_zero },
    { "nvnQueueResetMemoryUsageCounts", nvn_nothing },
    NVN_ENTRY(nvnQueueSubmitCommands), { "nvnQueueFlush", nvn_nothing }, NVN_ENTRY(nvnQueueFinish),
    NVN_ENTRY(nvnQueueFenceSync), NVN_ENTRY(nvnQueueWaitSync),

    NVN_ENTRY(nvnSyncInitialize), { "nvnSyncFinalize", nvn_nothing }, { "nvnSyncSetDebugLabel", nvn_nothing },
    NVN_ENTRY(nvnSyncWait), { "nvnSyncCreateGLSync", nvn_zero }, { "nvnSyncInitializeFromFencedGLSync", nvn_zero },

    NVN_ENTRY(nvnEventBuilderSetDefaults), NVN_ENTRY(nvnEventBuilderSetStorage), NVN_ENTRY(nvnEventBuilderGetStorage),
    NVN_ENTRY(nvnEventBuilderGetMemoryPool), NVN_ENTRY(nvnEventBuilderGetMemoryOffset),
    NVN_ENTRY(nvnEventInitialize), { "nvnEventFinalize", nvn_nothing }, NVN_ENTRY(nvnEventGetValue),
    NVN_ENTRY(nvnEventSignal), NVN_ENTRY(nvnEventGetMemoryPool), NVN_ENTRY(nvnEventGetMemoryOffset),
    { NULL, NULL },
};
