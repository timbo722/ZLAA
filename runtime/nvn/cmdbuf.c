/*
 * cmdbuf.c - NVNcommandBuffer: recording commands.
 *
 * The game gives each command buffer "command memory" (in a memory pool) and records commands
 * into it; EndRecording returns a handle it can submit to a queue, as often as it likes, until
 * it reuses that memory. We record our own Cmd structs (nvn.h) into that same memory, so the
 * game's memory management works as designed: when a chunk is full we call the game's memory
 * callback, which hands us a new chunk, and a JUMP record links the two.
 *
 * A command handle is simply the guest address of the list's first Cmd.
 */
#include "nvn.h"
#include <stdio.h>
#include <stdlib.h>

#define JUMP_SIZE ((uint32_t)sizeof(Cmd))   /* always keep room to chain to the next chunk */

static CommandBuffer *cmdbuf(uint64_t guest) { return gptr(guest); }

static void nvnCommandBufferInitialize(CPU *c) {
    *cmdbuf(ARG(0)) = (CommandBuffer){ .self = ARG(0) };
    RET(1);
}

static Cmd *put(uint64_t at, uint32_t op, uint32_t size) {
    Cmd *r = gptr(at);
    memset(r, 0, sizeof *r);
    r->op = op;
    r->size = size;
    return r;
}

/* (cmd, pool, offset, size) */
static void nvnCommandBufferAddCommandMemory(CPU *c) {
    CommandBuffer *cb = cmdbuf(ARG(0));
    uint64_t mem = pool_address(ARG(1), ARG(2));
    if (cb->recording && cb->mem) put(cb->mem + cb->used, CMD_JUMP, JUMP_SIZE)->a[0] = mem;
    cb->mem = mem;
    cb->size = ARG(3);
    cb->used = 0;
}
/* (cmd, void *memory, size): control memory is CPU memory the real driver uses; we don't */
static void nvnCommandBufferAddControlMemory(CPU *c) { cmdbuf(ARG(0))->control_size = ARG(2); }
static void nvnCommandBufferGetCommandMemorySize(CPU *c) { RET(cmdbuf(ARG(0))->size); }
static void nvnCommandBufferGetCommandMemoryUsed(CPU *c) { RET(cmdbuf(ARG(0))->used); }
static void nvnCommandBufferGetCommandMemoryFree(CPU *c) {
    CommandBuffer *cb = cmdbuf(ARG(0));
    RET(cb->size > cb->used + JUMP_SIZE ? cb->size - cb->used - JUMP_SIZE : 0);
}
static void nvnCommandBufferGetControlMemorySize(CPU *c) { RET(cmdbuf(ARG(0))->control_size); }
static void nvnCommandBufferGetControlMemoryFree(CPU *c) { RET(cmdbuf(ARG(0))->control_size); }
static void nvnCommandBufferSetMemoryCallback(CPU *c)     { cmdbuf(ARG(0))->callback = ARG(1); }
static void nvnCommandBufferGetMemoryCallback(CPU *c)     { RET(cmdbuf(ARG(0))->callback); }
static void nvnCommandBufferSetMemoryCallbackData(CPU *c) { cmdbuf(ARG(0))->callback_data = ARG(1); }
static void nvnCommandBufferGetMemoryCallbackData(CPU *c) { RET(cmdbuf(ARG(0))->callback_data); }

/* Make room for a record of `size` bytes. If the chunk is full, call the game's callback:
 * void callback(NVNcommandBuffer *, NVNcommandBufferMemoryEvent (0 = out of command memory),
 *               size_t minimum size, void *callback data). It runs guest code on this CPU, so
 * it clobbers the argument registers: callers copy their arguments out first (see Args). */
static Cmd *reserve(CPU *c, CommandBuffer *cb, uint32_t op, uint32_t size) {
    for (int tries = 0; !cb->mem || cb->used + size + JUMP_SIZE > cb->size; tries++) {
        if (!cb->callback || tries == 2) {
            fprintf(stderr, "nvn: out of command memory (need %u bytes)\n", size);
            exit(1);
        }
        uint64_t saved[31];
        memcpy(saved, c->x, sizeof saved);
        c->x[0] = cb->self;
        c->x[1] = 0;
        c->x[2] = size + JUMP_SIZE;
        c->x[3] = cb->callback_data;
        recomp_call_addr(c, cb->callback);
        memcpy(c->x, saved, sizeof saved);
    }
    Cmd *r = put(cb->mem + cb->used, op, size);
    cb->used += size;
    return r;
}

static void nvnCommandBufferBeginRecording(CPU *c) {
    CommandBuffer *cb = cmdbuf(ARG(0));
    cb->recording = 1;
    reserve(c, cb, CMD_NOP, sizeof(Cmd));            /* makes sure there is memory; the list starts here */
    cb->start = cb->mem + cb->used - sizeof(Cmd);
}
static void nvnCommandBufferEndRecording(CPU *c) {
    CommandBuffer *cb = cmdbuf(ARG(0));
    reserve(c, cb, CMD_END, sizeof(Cmd));
    cb->recording = 0;
    RET(cb->start);
}
static void nvnCommandBufferIsRecording(CPU *c) { RET(cmdbuf(ARG(0))->recording); }

/* ---- generic recording ---- */

typedef struct { uint64_t x[8]; float f[4]; } Args;
static Args args_of(CPU *c) {
    Args a;
    memcpy(a.x, c->x, sizeof a.x);
    for (int i = 0; i < 4; i++) a.f[i] = c->V[i].f[0];
    return a;
}

/* Record the call: op, variant, its arguments, and `len` bytes from the guest pointer that is
 * argument `ptr_arg` (zeros if the pointer is NULL). */
static Cmd *record(CPU *c, uint32_t op, uint32_t sub, int ptr_arg, uint32_t len) {
    Args a = args_of(c);
    uint32_t size = (uint32_t)(sizeof(Cmd) + len + 7) & ~7u;
    Cmd *r = reserve(c, cmdbuf(a.x[0]), op, size);
    r->sub = sub;
    memcpy(r->a, a.x + 1, sizeof r->a);
    memcpy(r->f, a.f, sizeof r->f);
    if (len && ptr_arg >= 0 && a.x[ptr_arg]) memcpy(r->extra, gptr(a.x[ptr_arg]), len);
    return r;
}

/* PLAIN: arguments only. COPY: plus `len` bytes from a pointer. ARRAY: plus count * elem bytes. */
#define PLAIN(fn, op, sub)               static void fn(CPU *c) { record(c, op, sub, -1, 0); }
#define COPY(fn, op, sub, arg, len)      static void fn(CPU *c) { record(c, op, sub, arg, len); }
#define ARRAY(fn, op, sub, arg, n, elem) static void fn(CPU *c) { record(c, op, sub, arg, (uint32_t)ARG(n) * (elem)); }

/* Single-binding calls are recorded as the array form with count 1: (cmd, [stage,] index,
 * value...) -> a = {[stage,] index, 1}, extra = value... */
static void record_one(CPU *c, uint32_t op, uint32_t sub, int has_stage, uint32_t value_bytes) {
    Args a = args_of(c);
    int first = has_stage ? 2 : 1;                   /* argument holding the index */
    Cmd *r = record(c, op, sub, -1, value_bytes);
    memset(r->a, 0, sizeof r->a);
    if (has_stage) r->a[0] = a.x[1];
    r->a[first - 1] = a.x[first];
    r->a[first] = 1;
    memcpy(r->extra, &a.x[first + 1], value_bytes);
}

/* ---- state ---- */

/* (cmd, program, stage bits): a copy of the program, since the game may change it later */
static void nvnCommandBufferBindProgram(CPU *c) { record(c, CMD_BIND_PROGRAM, 0, 1, sizeof(Program)); }
COPY(nvnCommandBufferBindBlendState, CMD_BIND_STATE, STATE_BLEND, 1, sizeof(BlendState))
COPY(nvnCommandBufferBindChannelMaskState, CMD_BIND_STATE, STATE_CHANNEL_MASK, 1, 4)
COPY(nvnCommandBufferBindColorState, CMD_BIND_STATE, STATE_COLOR, 1, sizeof(ColorState))
COPY(nvnCommandBufferBindDepthStencilState, CMD_BIND_STATE, STATE_DEPTH_STENCIL, 1, sizeof(DepthStencilState))
COPY(nvnCommandBufferBindMultisampleState, CMD_BIND_STATE, STATE_MULTISAMPLE, 1, sizeof(MultisampleState))
COPY(nvnCommandBufferBindPolygonState, CMD_BIND_STATE, STATE_POLYGON, 1, sizeof(PolygonState))
/* (cmd, int count, const state *) */
ARRAY(nvnCommandBufferBindVertexAttribState, CMD_BIND_VERTEX_ATTRIBS, 0, 2, 1, sizeof(VertexAttribState))
ARRAY(nvnCommandBufferBindVertexStreamState, CMD_BIND_VERTEX_STREAMS, 0, 2, 1, sizeof(VertexStreamState))

/* buffers: (cmd, [stage,] first, count, const NVNbufferRange * {u64 address, u64 size}) */
ARRAY(nvnCommandBufferBindVertexBuffers, CMD_BIND_VERTEX_BUFFERS, 0, 3, 2, 16)
ARRAY(nvnCommandBufferBindUniformBuffers, CMD_BIND_UNIFORM_BUFFERS, 0, 4, 3, 16)
ARRAY(nvnCommandBufferBindStorageBuffers, CMD_BIND_STORAGE_BUFFERS, 0, 4, 3, 16)
static void nvnCommandBufferBindVertexBuffer(CPU *c)  { record_one(c, CMD_BIND_VERTEX_BUFFERS, 0, 0, 16); }
static void nvnCommandBufferBindUniformBuffer(CPU *c) { record_one(c, CMD_BIND_UNIFORM_BUFFERS, 0, 1, 16); }
static void nvnCommandBufferBindStorageBuffer(CPU *c) { record_one(c, CMD_BIND_STORAGE_BUFFERS, 0, 1, 16); }
/* textures: (cmd, stage, first, count, const handle *) or (cmd, stage, index, handle) */
ARRAY(nvnCommandBufferBindTextures, CMD_BIND_TEXTURES, 0, 4, 3, 8)
ARRAY(nvnCommandBufferBindImages, CMD_BIND_IMAGES, 0, 4, 3, 8)
ARRAY(nvnCommandBufferBindSeparateTextures, CMD_BIND_TEXTURES, 1, 4, 3, 8)
ARRAY(nvnCommandBufferBindSeparateSamplers, CMD_BIND_SEPARATE_SAMPLERS, 0, 4, 3, 8)
static void nvnCommandBufferBindTexture(CPU *c)         { record_one(c, CMD_BIND_TEXTURES, 0, 1, 8); }
static void nvnCommandBufferBindImage(CPU *c)           { record_one(c, CMD_BIND_IMAGES, 0, 1, 8); }
static void nvnCommandBufferBindSeparateTexture(CPU *c) { record_one(c, CMD_BIND_TEXTURES, 1, 1, 8); }
static void nvnCommandBufferBindSeparateSampler(CPU *c) { record_one(c, CMD_BIND_SEPARATE_SAMPLERS, 0, 1, 8); }
/* transform feedback isn't drawn yet; recorded so nothing is lost */
ARRAY(nvnCommandBufferBindTransformFeedbackBuffers, CMD_TRANSFORM_FEEDBACK, 0, 3, 2, 16)
static void nvnCommandBufferBindTransformFeedbackBuffer(CPU *c) { record_one(c, CMD_TRANSFORM_FEEDBACK, 0, 0, 16); }
PLAIN(nvnCommandBufferBeginTransformFeedback, CMD_TRANSFORM_FEEDBACK, 1)
PLAIN(nvnCommandBufferEndTransformFeedback, CMD_TRANSFORM_FEEDBACK, 2)
PLAIN(nvnCommandBufferPauseTransformFeedback, CMD_TRANSFORM_FEEDBACK, 3)
PLAIN(nvnCommandBufferResumeTransformFeedback, CMD_TRANSFORM_FEEDBACK, 4)

/* (cmd, const NVNtexturePool *) - the pool object holds the descriptor table pointer */
COPY(nvnCommandBufferSetTexturePool, CMD_SET_TEXTURE_POOL, 0, 1, sizeof(DescriptorPool))
COPY(nvnCommandBufferSetSamplerPool, CMD_SET_SAMPLER_POOL, 0, 1, sizeof(DescriptorPool))

static ImageRef image_ref(uint64_t texture, uint64_t view, uint64_t region) {
    ImageRef r = { .texture = texture };
    if (view) r.view = *(TextureView *)gptr(view);
    if (region) memcpy(r.region, gptr(region), sizeof r.region);
    return r;
}

/* (cmd, int count, NVNtexture *const *colors, NVNtextureView *const *color views,
 *  NVNtexture *depth, NVNtextureView *depth view) */
static void nvnCommandBufferSetRenderTargets(CPU *c) {
    Args a = args_of(c);
    RenderTargets rt = { .count = a.x[1] < 8 ? (uint32_t)a.x[1] : 8 };
    for (uint32_t i = 0; i < rt.count; i++) {
        uint64_t tex = a.x[2] ? g_ld64(a.x[2] + 8 * i) : 0, view = a.x[3] ? g_ld64(a.x[3] + 8 * i) : 0;
        rt.color[i] = image_ref(tex, view, 0);
    }
    rt.depth = image_ref(a.x[4], a.x[5], 0);
    Cmd *r = record(c, CMD_SET_RENDER_TARGETS, 0, -1, sizeof rt);
    memcpy(r->extra, &rt, sizeof rt);
}

/* (cmd, first, count, const float/int *): 4 values per viewport or scissor, 2 per depth range */
ARRAY(nvnCommandBufferSetViewports, CMD_SET_VIEWPORTS, 0, 3, 2, 16)
ARRAY(nvnCommandBufferSetScissors, CMD_SET_SCISSORS, 0, 3, 2, 16)
ARRAY(nvnCommandBufferSetDepthRanges, CMD_SET_DEPTH_RANGES, 0, 3, 2, 8)
/* single forms -> array form with first 0, count 1 */
static void one_rect(CPU *c, uint32_t op, int floats) {
    Args a = args_of(c);
    Cmd *r = record(c, op, 0, -1, 16);
    for (int i = 0; i < 4; i++) {
        int32_t v = (int32_t)a.x[1 + i];
        float f = (float)v;
        memcpy(r->extra + 4 * i, floats ? (void *)&f : (void *)&v, 4);
    }
    r->a[0] = 0;
    r->a[1] = 1;
}
static void nvnCommandBufferSetViewport(CPU *c) { one_rect(c, CMD_SET_VIEWPORTS, 1); }
static void nvnCommandBufferSetScissor(CPU *c)  { one_rect(c, CMD_SET_SCISSORS, 0); }
static void nvnCommandBufferSetDepthRange(CPU *c) {           /* (cmd, float near, float far) */
    Args a = args_of(c);
    Cmd *r = record(c, CMD_SET_DEPTH_RANGES, 0, -1, 8);
    memcpy(r->extra, a.f, 8);
    r->a[0] = 0;
    r->a[1] = 1;
}

COPY(nvnCommandBufferSetBlendColor, CMD_SET_VALUE, VALUE_BLEND_COLOR, 1, 16)
PLAIN(nvnCommandBufferSetStencilRef, CMD_SET_VALUE, VALUE_STENCIL_REF)
PLAIN(nvnCommandBufferSetStencilMask, CMD_SET_VALUE, VALUE_STENCIL_MASK)
PLAIN(nvnCommandBufferSetStencilValueMask, CMD_SET_VALUE, VALUE_STENCIL_VALUE_MASK)
PLAIN(nvnCommandBufferSetPrimitiveRestart, CMD_SET_VALUE, VALUE_PRIMITIVE_RESTART)
PLAIN(nvnCommandBufferSetAlphaRef, CMD_SET_VALUE, VALUE_ALPHA_REF)
PLAIN(nvnCommandBufferSetDepthBounds, CMD_SET_VALUE, VALUE_DEPTH_BOUNDS)
PLAIN(nvnCommandBufferSetPolygonOffsetClamp, CMD_SET_VALUE, VALUE_POLYGON_OFFSET_CLAMP)
PLAIN(nvnCommandBufferSetLineWidth, CMD_SET_VALUE, VALUE_LINE_WIDTH)
PLAIN(nvnCommandBufferSetPointSize, CMD_SET_VALUE, VALUE_POINT_SIZE)
PLAIN(nvnCommandBufferSetSampleMask, CMD_SET_VALUE, VALUE_SAMPLE_MASK)
PLAIN(nvnCommandBufferSetRenderEnable, CMD_SET_VALUE, VALUE_RENDER_ENABLE)
PLAIN(nvnCommandBufferSetRenderEnableConditional, CMD_SET_VALUE, VALUE_RENDER_ENABLE_CONDITIONAL)
PLAIN(nvnCommandBufferSetDepthClamp, CMD_SET_VALUE, VALUE_DEPTH_CLAMP)
PLAIN(nvnCommandBufferSetRasterizerDiscard, CMD_SET_VALUE, VALUE_RASTERIZER_DISCARD)
PLAIN(nvnCommandBufferSetConservativeRasterEnable, CMD_SET_VALUE, VALUE_CONSERVATIVE_RASTER_ENABLE)
PLAIN(nvnCommandBufferSetConservativeRasterDilate, CMD_SET_VALUE, VALUE_CONSERVATIVE_RASTER_DILATE)
PLAIN(nvnCommandBufferSetColorReductionEnable, CMD_SET_VALUE, VALUE_COLOR_REDUCTION_ENABLE)
PLAIN(nvnCommandBufferSetColorReductionThresholds, CMD_SET_VALUE, VALUE_COLOR_REDUCTION_THRESHOLDS)
PLAIN(nvnCommandBufferSetPatchSize, CMD_SET_VALUE, VALUE_PATCH_SIZE)
COPY(nvnCommandBufferSetInnerTessellationLevels, CMD_SET_VALUE, VALUE_INNER_TESS_LEVELS, 1, 8)
COPY(nvnCommandBufferSetOuterTessellationLevels, CMD_SET_VALUE, VALUE_OUTER_TESS_LEVELS, 1, 16)
PLAIN(nvnCommandBufferSetCopyRowStride, CMD_SET_VALUE, VALUE_COPY_ROW_STRIDE)
PLAIN(nvnCommandBufferSetCopyImageStride, CMD_SET_VALUE, VALUE_COPY_IMAGE_STRIDE)
PLAIN(nvnCommandBufferSetShaderScratchMemory, CMD_SET_VALUE, VALUE_SHADER_SCRATCH_MEMORY)
ARRAY(nvnCommandBufferSetViewportSwizzles, CMD_SET_VALUE, VALUE_VIEWPORT_SWIZZLES, 3, 2, 16)
PLAIN(nvnCommandBufferSetSubpixelPrecisionBias, CMD_SET_VALUE, VALUE_SUBPIXEL_PRECISION_BIAS)
PLAIN(nvnCommandBufferSetStencilCullCriteria, CMD_SET_VALUE, VALUE_STENCIL_CULL_CRITERIA)
PLAIN(nvnCommandBufferSetTiledCacheAction, CMD_SET_VALUE, VALUE_TILED_CACHE_ACTION)
PLAIN(nvnCommandBufferSetTiledCacheTileSize, CMD_SET_VALUE, VALUE_TILED_CACHE_TILE_SIZE)
PLAIN(nvnCommandBufferSetProgramSubroutines, CMD_SET_VALUE, VALUE_PROGRAM_SUBROUTINES)
PLAIN(nvnCommandBufferBindCoverageModulationTable, CMD_SET_VALUE, VALUE_COVERAGE_MODULATION_TABLE)
PLAIN(nvnCommandBufferDiscardDepthStencil, CMD_SET_VALUE, VALUE_DISCARD_DEPTH_STENCIL)
PLAIN(nvnCommandBufferSaveZCullData, CMD_SET_VALUE, VALUE_SAVE_ZCULL)
PLAIN(nvnCommandBufferRestoreZCullData, CMD_SET_VALUE, VALUE_RESTORE_ZCULL)
PLAIN(nvnCommandBufferResolveDepthBuffer, CMD_SET_VALUE, VALUE_RESOLVE_DEPTH)
PLAIN(nvnCommandBufferResetCounter, CMD_SET_VALUE, VALUE_RESET_COUNTER)

/* ---- clears: (cmd, int index, const float/int/uint *color, int mask); sub = 0 float, 1 int, 2 uint ---- */
COPY(nvnCommandBufferClearColor, CMD_CLEAR_COLOR, 0, 2, 16)
COPY(nvnCommandBufferClearColori, CMD_CLEAR_COLOR, 1, 2, 16)
COPY(nvnCommandBufferClearColorui, CMD_CLEAR_COLOR, 2, 2, 16)
/* (cmd, float depth, bool depth mask, int stencil, int stencil mask) */
PLAIN(nvnCommandBufferClearDepthStencil, CMD_CLEAR_DEPTH_STENCIL, 0)
/* (cmd, address, size, u32 value) */
PLAIN(nvnCommandBufferClearBuffer, CMD_CLEAR_BUFFER, 0)
/* (cmd, int index) */
PLAIN(nvnCommandBufferDiscardColor, CMD_DISCARD_COLOR, 0)
/* (cmd, texture, view, const NVNcopyRegion *, const color *, int mask) */
static void clear_texture(CPU *c, uint32_t sub) {
    Args a = args_of(c);
    struct { ImageRef image; uint32_t color[4]; } ct = { image_ref(a.x[1], a.x[2], a.x[3]), { 0 } };
    if (a.x[4]) memcpy(ct.color, gptr(a.x[4]), 16);
    Cmd *r = record(c, CMD_CLEAR_TEXTURE, sub, -1, sizeof ct);
    memcpy(r->extra, &ct, sizeof ct);
}
static void nvnCommandBufferClearTexture(CPU *c)   { clear_texture(c, 0); }
static void nvnCommandBufferClearTexturei(CPU *c)  { clear_texture(c, 1); }
static void nvnCommandBufferClearTextureui(CPU *c) { clear_texture(c, 2); }

/* ---- draws and dispatches: all arguments are integers in registers ---- */

/* (cmd, mode, first, count) -> instanced form: + base instance 0, 1 instance */
static void nvnCommandBufferDrawArrays(CPU *c) {
    Cmd *r = record(c, CMD_DRAW_ARRAYS, 0, -1, 0);
    r->a[3] = 0;
    r->a[4] = 1;
}
/* (cmd, mode, first, count, base instance, instance count) */
PLAIN(nvnCommandBufferDrawArraysInstanced, CMD_DRAW_ARRAYS, 0)
/* (cmd, mode, index type, count, index buffer address[, base vertex[, base instance, instances]]) */
static void nvnCommandBufferDrawElements(CPU *c) {
    Cmd *r = record(c, CMD_DRAW_ELEMENTS, 0, -1, 0);
    r->a[4] = 0; r->a[5] = 0; r->a[6] = 1;
}
static void nvnCommandBufferDrawElementsBaseVertex(CPU *c) {
    Cmd *r = record(c, CMD_DRAW_ELEMENTS, 0, -1, 0);
    r->a[5] = 0; r->a[6] = 1;
}
PLAIN(nvnCommandBufferDrawElementsInstanced, CMD_DRAW_ELEMENTS, 0)
PLAIN(nvnCommandBufferDrawArraysIndirect, CMD_DRAW_ARRAYS_INDIRECT, 0)
PLAIN(nvnCommandBufferDrawElementsIndirect, CMD_DRAW_ELEMENTS_INDIRECT, 0)
PLAIN(nvnCommandBufferMultiDrawArraysIndirectCount, CMD_MULTI_DRAW_ARRAYS_INDIRECT_COUNT, 0)
PLAIN(nvnCommandBufferMultiDrawElementsIndirectCount, CMD_MULTI_DRAW_ELEMENTS_INDIRECT_COUNT, 0)
PLAIN(nvnCommandBufferDrawTransformFeedback, CMD_TRANSFORM_FEEDBACK, 5)
PLAIN(nvnCommandBufferDispatchCompute, CMD_DISPATCH, 0)
PLAIN(nvnCommandBufferDispatchComputeIndirect, CMD_DISPATCH_INDIRECT, 0)

/* ---- copies ---- */

/* (cmd, src address, dst address, size, flags) */
PLAIN(nvnCommandBufferCopyBufferToBuffer, CMD_COPY_BUFFER_TO_BUFFER, 0)
/* (cmd, src address, dst texture, dst view, dst region, flags) */
static void nvnCommandBufferCopyBufferToTexture(CPU *c) {
    Args a = args_of(c);
    ImageRef dst = image_ref(a.x[2], a.x[3], a.x[4]);
    Cmd *r = record(c, CMD_COPY_BUFFER_TO_TEXTURE, 0, -1, sizeof dst);
    memcpy(r->extra, &dst, sizeof dst);
}
/* (cmd, src texture, src view, src region, dst address, flags) */
static void nvnCommandBufferCopyTextureToBuffer(CPU *c) {
    Args a = args_of(c);
    ImageRef src = image_ref(a.x[1], a.x[2], a.x[3]);
    Cmd *r = record(c, CMD_COPY_TEXTURE_TO_BUFFER, 0, -1, sizeof src);
    memcpy(r->extra, &src, sizeof src);
}
/* (cmd, src texture, src view, src region, dst texture, dst view, dst region, flags) */
static void nvnCommandBufferCopyTextureToTexture(CPU *c) {
    Args a = args_of(c);
    ImageRef img[2] = { image_ref(a.x[1], a.x[2], a.x[3]), image_ref(a.x[4], a.x[5], a.x[6]) };
    Cmd *r = record(c, CMD_COPY_TEXTURE_TO_TEXTURE, 0, -1, sizeof img);
    memcpy(r->extra, img, sizeof img);
}
/* Downsample (cmd, src, dst) / DownsampleTextureView (cmd, src, src view, dst, dst view);
 * sub 1 = tiled variants */
static void downsample(CPU *c, int views, uint32_t sub) {
    Args a = args_of(c);
    ImageRef img[2] = { image_ref(a.x[1], views ? a.x[2] : 0, 0), image_ref(a.x[views ? 3 : 2], views ? a.x[4] : 0, 0) };
    Cmd *r = record(c, CMD_DOWNSAMPLE, sub, -1, sizeof img);
    memcpy(r->extra, img, sizeof img);
}
static void nvnCommandBufferDownsample(CPU *c)                 { downsample(c, 0, 0); }
static void nvnCommandBufferDownsampleTextureView(CPU *c)      { downsample(c, 1, 0); }
static void nvnCommandBufferTiledDownsample(CPU *c)            { downsample(c, 0, 1); }
static void nvnCommandBufferTiledDownsampleTextureView(CPU *c) { downsample(c, 1, 1); }
/* (cmd, buffer address, buffer size, offset, size, const void *data) */
static void nvnCommandBufferUpdateUniformBuffer(CPU *c) { record(c, CMD_UPDATE_UNIFORM_BUFFER, 0, 5, (uint32_t)ARG(4)); }

/* ---- synchronization ---- */

/* (cmd, sync, condition, flags) */
PLAIN(nvnCommandBufferFenceSync, CMD_FENCE_SYNC, 0)
PLAIN(nvnCommandBufferWaitSync, CMD_WAIT_SYNC, 0)
/* (cmd, int barrier bits) */
PLAIN(nvnCommandBufferBarrier, CMD_BARRIER, 0)
/* (cmd, event, mode, location, flags, u32 value) / (cmd, event, wait mode, u32 value) */
PLAIN(nvnCommandBufferSignalEvent, CMD_SIGNAL_EVENT, 0)
PLAIN(nvnCommandBufferWaitEvent, CMD_WAIT_EVENT, 0)
/* (cmd, NVNcounterType, address) / (cmd, u32 value, address) */
PLAIN(nvnCommandBufferReportCounter, CMD_REPORT_COUNTER, 0)
PLAIN(nvnCommandBufferReportValue, CMD_REPORT_VALUE, 0)
/* (cmd, int count, const NVNcommandHandle *): run other lists from this one */
ARRAY(nvnCommandBufferCallCommands, CMD_CALL, 0, 2, 1, 8)
ARRAY(nvnCommandBufferCopyCommands, CMD_CALL, 0, 2, 1, 8)

const NvnEntry nvn_command_functions[] = {
    NVN_ENTRY(nvnCommandBufferInitialize), { "nvnCommandBufferFinalize", nvn_nothing },
    { "nvnCommandBufferSetDebugLabel", nvn_nothing },
    NVN_ENTRY(nvnCommandBufferAddCommandMemory), NVN_ENTRY(nvnCommandBufferAddControlMemory),
    NVN_ENTRY(nvnCommandBufferGetCommandMemorySize), NVN_ENTRY(nvnCommandBufferGetCommandMemoryUsed),
    NVN_ENTRY(nvnCommandBufferGetCommandMemoryFree), NVN_ENTRY(nvnCommandBufferGetControlMemorySize),
    { "nvnCommandBufferGetControlMemoryUsed", nvn_zero }, NVN_ENTRY(nvnCommandBufferGetControlMemoryFree),
    NVN_ENTRY(nvnCommandBufferSetMemoryCallback), NVN_ENTRY(nvnCommandBufferGetMemoryCallback),
    NVN_ENTRY(nvnCommandBufferSetMemoryCallbackData), NVN_ENTRY(nvnCommandBufferGetMemoryCallbackData),
    { "nvnCommandBufferSetCommandMemoryCallbackEnabled", nvn_nothing },
    NVN_ENTRY(nvnCommandBufferBeginRecording), NVN_ENTRY(nvnCommandBufferEndRecording),
    NVN_ENTRY(nvnCommandBufferIsRecording),

    NVN_ENTRY(nvnCommandBufferBindProgram), NVN_ENTRY(nvnCommandBufferBindBlendState),
    NVN_ENTRY(nvnCommandBufferBindChannelMaskState), NVN_ENTRY(nvnCommandBufferBindColorState),
    NVN_ENTRY(nvnCommandBufferBindDepthStencilState), NVN_ENTRY(nvnCommandBufferBindMultisampleState),
    NVN_ENTRY(nvnCommandBufferBindPolygonState), NVN_ENTRY(nvnCommandBufferBindVertexAttribState),
    NVN_ENTRY(nvnCommandBufferBindVertexStreamState),
    NVN_ENTRY(nvnCommandBufferBindVertexBuffers), NVN_ENTRY(nvnCommandBufferBindUniformBuffers),
    NVN_ENTRY(nvnCommandBufferBindStorageBuffers), NVN_ENTRY(nvnCommandBufferBindVertexBuffer),
    NVN_ENTRY(nvnCommandBufferBindUniformBuffer), NVN_ENTRY(nvnCommandBufferBindStorageBuffer),
    NVN_ENTRY(nvnCommandBufferBindTextures), NVN_ENTRY(nvnCommandBufferBindImages),
    NVN_ENTRY(nvnCommandBufferBindSeparateTextures), NVN_ENTRY(nvnCommandBufferBindSeparateSamplers),
    NVN_ENTRY(nvnCommandBufferBindTexture), NVN_ENTRY(nvnCommandBufferBindImage),
    NVN_ENTRY(nvnCommandBufferBindSeparateTexture), NVN_ENTRY(nvnCommandBufferBindSeparateSampler),
    NVN_ENTRY(nvnCommandBufferBindTransformFeedbackBuffers), NVN_ENTRY(nvnCommandBufferBindTransformFeedbackBuffer),
    NVN_ENTRY(nvnCommandBufferBeginTransformFeedback), NVN_ENTRY(nvnCommandBufferEndTransformFeedback),
    NVN_ENTRY(nvnCommandBufferPauseTransformFeedback), NVN_ENTRY(nvnCommandBufferResumeTransformFeedback),
    NVN_ENTRY(nvnCommandBufferSetTexturePool), NVN_ENTRY(nvnCommandBufferSetSamplerPool),
    NVN_ENTRY(nvnCommandBufferSetRenderTargets),
    NVN_ENTRY(nvnCommandBufferSetViewports), NVN_ENTRY(nvnCommandBufferSetScissors),
    NVN_ENTRY(nvnCommandBufferSetDepthRanges), NVN_ENTRY(nvnCommandBufferSetViewport),
    NVN_ENTRY(nvnCommandBufferSetScissor), NVN_ENTRY(nvnCommandBufferSetDepthRange),
    NVN_ENTRY(nvnCommandBufferSetBlendColor), NVN_ENTRY(nvnCommandBufferSetStencilRef),
    NVN_ENTRY(nvnCommandBufferSetStencilMask), NVN_ENTRY(nvnCommandBufferSetStencilValueMask),
    NVN_ENTRY(nvnCommandBufferSetPrimitiveRestart), NVN_ENTRY(nvnCommandBufferSetAlphaRef),
    NVN_ENTRY(nvnCommandBufferSetDepthBounds), NVN_ENTRY(nvnCommandBufferSetPolygonOffsetClamp),
    NVN_ENTRY(nvnCommandBufferSetLineWidth), NVN_ENTRY(nvnCommandBufferSetPointSize),
    NVN_ENTRY(nvnCommandBufferSetSampleMask), NVN_ENTRY(nvnCommandBufferSetRenderEnable),
    NVN_ENTRY(nvnCommandBufferSetRenderEnableConditional), NVN_ENTRY(nvnCommandBufferSetDepthClamp),
    NVN_ENTRY(nvnCommandBufferSetRasterizerDiscard), NVN_ENTRY(nvnCommandBufferSetConservativeRasterEnable),
    NVN_ENTRY(nvnCommandBufferSetConservativeRasterDilate), NVN_ENTRY(nvnCommandBufferSetColorReductionEnable),
    NVN_ENTRY(nvnCommandBufferSetColorReductionThresholds), NVN_ENTRY(nvnCommandBufferSetPatchSize),
    NVN_ENTRY(nvnCommandBufferSetInnerTessellationLevels), NVN_ENTRY(nvnCommandBufferSetOuterTessellationLevels),
    NVN_ENTRY(nvnCommandBufferSetCopyRowStride), NVN_ENTRY(nvnCommandBufferSetCopyImageStride),
    NVN_ENTRY(nvnCommandBufferSetShaderScratchMemory), NVN_ENTRY(nvnCommandBufferSetViewportSwizzles),
    NVN_ENTRY(nvnCommandBufferSetSubpixelPrecisionBias), NVN_ENTRY(nvnCommandBufferSetStencilCullCriteria),
    NVN_ENTRY(nvnCommandBufferSetTiledCacheAction), NVN_ENTRY(nvnCommandBufferSetTiledCacheTileSize),
    NVN_ENTRY(nvnCommandBufferSetProgramSubroutines), NVN_ENTRY(nvnCommandBufferBindCoverageModulationTable),
    NVN_ENTRY(nvnCommandBufferDiscardDepthStencil), NVN_ENTRY(nvnCommandBufferSaveZCullData),
    NVN_ENTRY(nvnCommandBufferRestoreZCullData), NVN_ENTRY(nvnCommandBufferResolveDepthBuffer),
    NVN_ENTRY(nvnCommandBufferResetCounter),

    NVN_ENTRY(nvnCommandBufferClearColor), NVN_ENTRY(nvnCommandBufferClearColori),
    NVN_ENTRY(nvnCommandBufferClearColorui), NVN_ENTRY(nvnCommandBufferClearDepthStencil),
    NVN_ENTRY(nvnCommandBufferClearBuffer), NVN_ENTRY(nvnCommandBufferDiscardColor),
    NVN_ENTRY(nvnCommandBufferClearTexture), NVN_ENTRY(nvnCommandBufferClearTexturei),
    NVN_ENTRY(nvnCommandBufferClearTextureui),

    NVN_ENTRY(nvnCommandBufferDrawArrays), NVN_ENTRY(nvnCommandBufferDrawArraysInstanced),
    NVN_ENTRY(nvnCommandBufferDrawElements), NVN_ENTRY(nvnCommandBufferDrawElementsBaseVertex),
    NVN_ENTRY(nvnCommandBufferDrawElementsInstanced), NVN_ENTRY(nvnCommandBufferDrawArraysIndirect),
    NVN_ENTRY(nvnCommandBufferDrawElementsIndirect), NVN_ENTRY(nvnCommandBufferMultiDrawArraysIndirectCount),
    NVN_ENTRY(nvnCommandBufferMultiDrawElementsIndirectCount), NVN_ENTRY(nvnCommandBufferDrawTransformFeedback),
    NVN_ENTRY(nvnCommandBufferDispatchCompute), NVN_ENTRY(nvnCommandBufferDispatchComputeIndirect),

    NVN_ENTRY(nvnCommandBufferCopyBufferToBuffer), NVN_ENTRY(nvnCommandBufferCopyBufferToTexture),
    NVN_ENTRY(nvnCommandBufferCopyTextureToBuffer), NVN_ENTRY(nvnCommandBufferCopyTextureToTexture),
    NVN_ENTRY(nvnCommandBufferDownsample), NVN_ENTRY(nvnCommandBufferDownsampleTextureView),
    NVN_ENTRY(nvnCommandBufferTiledDownsample), NVN_ENTRY(nvnCommandBufferTiledDownsampleTextureView),
    NVN_ENTRY(nvnCommandBufferUpdateUniformBuffer),

    NVN_ENTRY(nvnCommandBufferFenceSync), NVN_ENTRY(nvnCommandBufferWaitSync), NVN_ENTRY(nvnCommandBufferBarrier),
    NVN_ENTRY(nvnCommandBufferSignalEvent), NVN_ENTRY(nvnCommandBufferWaitEvent),
    NVN_ENTRY(nvnCommandBufferReportCounter), NVN_ENTRY(nvnCommandBufferReportValue),
    NVN_ENTRY(nvnCommandBufferCallCommands), NVN_ENTRY(nvnCommandBufferCopyCommands),
    { "nvnCommandBufferInsertDebugMarker", nvn_nothing }, { "nvnCommandBufferInsertDebugMarkerStatic", nvn_nothing },
    { "nvnCommandBufferInsertDebugMarkerDynamic", nvn_nothing }, { "nvnCommandBufferPushDebugGroup", nvn_nothing },
    { "nvnCommandBufferPushDebugGroupStatic", nvn_nothing }, { "nvnCommandBufferPushDebugGroupDynamic", nvn_nothing },
    { "nvnCommandBufferPopDebugGroup", nvn_nothing }, { "nvnCommandBufferPopDebugGroupId", nvn_nothing },
    { NULL, NULL },
};
