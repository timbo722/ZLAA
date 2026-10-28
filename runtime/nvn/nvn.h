/*
 * nvn.h - shared definitions for the NVN implementation (runtime/nvn/).
 *
 * NVN is the Switch's graphics API. Every NVN object (NVNtexture, NVNqueue, ...) is a block of
 * memory the game allocates with a fixed size, and only NVN functions look inside it. Since we
 * implement those functions, we choose what goes inside: each object below is a C struct that
 * we place directly in the game's memory (checked to fit with _Static_assert). So a pointer the
 * game passes us, e.g. NVNtexture *, becomes `Texture *t = gptr(c->x[0])`.
 *
 * GPU addresses are guest addresses: a memory pool's "GPU address" is just where its storage
 * lives in guest memory, so buffers, textures and shaders can all be read with gptr().
 */
#ifndef NVN_H
#define NVN_H

#include "hle.h"
#include "guest_memory.h"
#include <stdint.h>
#include <string.h>

typedef struct { const char *name; HleFunc fn; } NvnEntry;
#define NVN_ENTRY(fn) { #fn, fn }

/* one table per group of NVN functions, each ending with { NULL, NULL } */
extern const NvnEntry nvn_device_functions[];   /* device.c: device, queue, sync, event */
extern const NvnEntry nvn_memory_functions[];   /* memory.c: memory pools, buffers */
extern const NvnEntry nvn_texture_functions[];  /* texture.c: textures, views, samplers, pools */
extern const NvnEntry nvn_state_functions[];    /* state.c: small state objects, programs, window */
extern const NvnEntry nvn_command_functions[];  /* cmdbuf.c: command buffers */

void nvn_init(void);
void nvn_get_proc_address(CPU *c);
extern int nvn_trace;                           /* LAA_TRACE_NVN: 1 = calls, 2 = also executed commands */
/* for functions that need no work: return nothing, true, or 0 */
void nvn_nothing(CPU *c);
void nvn_true(CPU *c);
void nvn_zero(CPU *c);

/* ---- arguments and results (AArch64 calling convention) ---- */
#define ARG(i)   (c->x[i])
#define ARGF(i)  (c->V[i].f[0])                 /* float arguments arrive in s0, s1, ... */
#define RET(v)   (c->x[0] = (uint64_t)(v))
static inline void ret_float(CPU *c, float f) { memset(&c->V[0], 0, sizeof c->V[0]); c->V[0].f[0] = f; }

/* ---- guest memory from C ---- */
static inline void *gptr(uint64_t addr) { return guest_mem + addr; }
static inline const char *guest_str(uint64_t addr) {
    if (!addr || addr >= guest_mem_size) return NULL;
    const char *s = (const char *)guest_mem + addr;
    return memchr(s, 0, guest_mem_size - addr) ? s : NULL;
}

#define FITS(type, nvn_size) _Static_assert(sizeof(type) <= (nvn_size), #type " must fit in " #nvn_size " bytes")

/* ================================================================ objects ================ */

typedef struct {                    /* NVNqueueBuilder (64 bytes) */
    uint64_t device;
    uint32_t flags, command_size, compute_size, control_size;
    uint64_t memory, memory_size;   /* queue memory the game gives us (unused) */
    uint32_t flush_threshold, priority;
} QueueBuilder;
FITS(QueueBuilder, 64);

typedef struct {                    /* NVNmemoryPoolBuilder (64) */
    uint64_t device;
    uint32_t flags, pad;
    uint64_t storage, size;         /* guest address and size of the pool's memory */
} MemoryPoolBuilder;
FITS(MemoryPoolBuilder, 64);

typedef struct {                    /* NVNmemoryPool (256) */
    uint64_t storage, size;
    uint32_t flags;
} MemoryPool;
FITS(MemoryPool, 256);

typedef struct {                    /* NVNbufferBuilder (64) */
    uint64_t device, pool, offset, size;
} BufferBuilder;
FITS(BufferBuilder, 64);

typedef struct {                    /* NVNbuffer (48) */
    uint64_t address;               /* == pool storage + offset */
    uint64_t pool, offset, size;
    uint64_t debug_id;
} Buffer;
FITS(Buffer, 48);

/* What a texture is: the fields NVNtextureBuilder sets. */
typedef struct {
    uint32_t flags, target, width, height, depth, levels, format, samples;
    uint8_t  swizzle[4];
    uint32_t depth_stencil_mode;
    uint64_t stride;                /* row pitch of linear textures */
    uint64_t pool, offset;          /* storage: NVNmemoryPool * and offset in it */
    uint32_t block_height_log2;     /* block-linear block height, or ~0 = NVN's default */
    uint32_t pad;
} TexDesc;

typedef struct {                    /* NVNtextureBuilder (128) */
    uint64_t device;
    TexDesc  d;
    uint64_t packaged_data;
    uint32_t gl_name;
} TextureBuilder;
FITS(TextureBuilder, 128);

typedef struct {                    /* NVNtexture (192) */
    TexDesc  d;
    uint64_t address;               /* guest address of texel storage */
    uint64_t storage_size;
    uint64_t debug_id;
} Texture;
FITS(Texture, 192);

enum {                              /* TextureView.set: which fields override the texture's */
    VIEW_LEVELS = 1, VIEW_LAYERS = 2, VIEW_FORMAT = 4, VIEW_SWIZZLE = 8, VIEW_DS_MODE = 16, VIEW_TARGET = 32,
};
typedef struct {                    /* NVNtextureView (40) */
    uint32_t set;
    uint16_t base_level, num_levels, min_layer, num_layers;
    uint32_t format, target, depth_stencil_mode;
    uint8_t  swizzle[4];
} TextureView;
FITS(TextureView, 40);

typedef struct {
    uint8_t  min_filter, mag_filter, wrap[3], compare_mode, compare_func, reduction;
    float    lod_min, lod_max, lod_bias, max_anisotropy, lod_snap;
    uint32_t border[4];             /* float, int or uint bits depending on which setter */
} SamplerDesc;

typedef struct {                    /* NVNsamplerBuilder (96) */
    uint64_t device;
    SamplerDesc s;
} SamplerBuilder;
FITS(SamplerBuilder, 96);

typedef struct {                    /* NVNsampler (96) */
    SamplerDesc s;
    uint64_t debug_id;
} Sampler;
FITS(Sampler, 96);

/* Texture and sampler pools: descriptor tables the game indexes by ID (texture handles are
 * textureID | samplerID << 20). We keep the entries in host memory. */
typedef struct {
    uint64_t texture;               /* guest NVNtexture *, 0 = empty */
    TextureView view;               /* view.set == 0: the whole texture */
} TexturePoolEntry;

typedef struct {                    /* NVNtexturePool / NVNsamplerPool (32) */
    void    *entries;               /* host array: TexturePoolEntry[] or SamplerDesc[] */
    uint64_t pool, offset;
    uint32_t count;
} DescriptorPool;
FITS(DescriptorPool, 32);

typedef struct {                    /* NVNprogram (192) */
    uint64_t code[6];               /* per stage: GPU address of the compiled shader */
    uint64_t control[6];            /* per stage: guest address of its control section */
    uint32_t stages;                /* bit per stage: STAGE_VERTEX, ... */
} Program;
FITS(Program, 192);
enum { STAGE_VERTEX, STAGE_FRAGMENT, STAGE_GEOMETRY, STAGE_TESS_CONTROL, STAGE_TESS_EVAL, STAGE_COMPUTE };

typedef struct {                    /* NVNwindowBuilder (64) */
    uint64_t device, native_window;
    uint64_t textures;              /* guest array of NVNtexture * */
    uint32_t num_textures, present_interval, num_active;
} WindowBuilder;
FITS(WindowBuilder, 64);

typedef struct {                    /* NVNwindow (384) */
    uint64_t textures[8];
    uint64_t native_window;
    uint32_t num_textures, num_active, present_interval, next;
    int32_t  crop[4];
} Window;
FITS(Window, 384);

typedef struct {                    /* NVNsync (64) */
    uint32_t signaled;
    uint32_t pad;
    uint64_t serial;                 /* with a renderer: the GPU submission that signals it (0: none) */
} Sync;
FITS(Sync, 64);

typedef struct {                    /* NVNeventBuilder (32) */
    uint64_t pool, offset;
} EventBuilder;
FITS(EventBuilder, 32);

typedef struct {                    /* NVNevent (64): a u32 in a memory pool */
    uint64_t address, pool, offset;
} Event;
FITS(Event, 64);

typedef struct {                    /* NVNcommandBuffer (160) */
    uint64_t self;                  /* its own guest address (for the memory callback) */
    uint64_t mem, size, used;       /* current command memory chunk */
    uint64_t start;                 /* first record of the list being recorded */
    uint64_t callback, callback_data;
    uint64_t control_size;
    uint32_t recording;
} CommandBuffer;
FITS(CommandBuffer, 160);

/* Small state objects (bound with nvnCommandBufferBind*State); bitfields keep them small:
 * `uint8_t target : 3` uses 3 bits of a byte. Enum values in comments are NVN's. */
typedef struct {
    uint8_t target : 3, overlap : 2, premultiplied_src : 1, normalized_dst : 1;
    uint8_t src_color, dst_color, src_alpha, dst_alpha;   /* NVNblendFunc: ZERO = 1, ONE = 2, ... */
    uint8_t eq_color, eq_alpha;                           /* NVNblendEquation: ADD = 1, ... */
    uint8_t advanced_mode;
} BlendState;
FITS(BlendState, 8);
typedef struct { uint8_t blend_enable, logic_op, alpha_test, pad; } ColorState;
typedef struct {
    uint8_t  depth_test : 1, depth_write : 1, stencil_test : 1;
    uint8_t  depth_func;            /* NVNdepthFunc: NEVER = 1, LESS = 2, ... ALWAYS = 8 */
    uint8_t  stencil_func[2];       /* front, back */
    uint16_t stencil_ops[2];        /* front, back: fail | depth fail << 4 | depth pass << 8 */
} DepthStencilState;
FITS(DepthStencilState, 8);
typedef struct {
    uint8_t  enable, alpha_to_coverage, alpha_to_coverage_dither, coverage_to_color, coverage_to_color_output;
    uint8_t  coverage_modulation_mode, sample_locations_enable, sample_locations_grid_enable;
    uint32_t samples, raster_samples;
} MultisampleState;
FITS(MultisampleState, 24);
typedef struct { uint8_t cull_face, front_face, polygon_mode, offset_enables; } PolygonState;
typedef struct { uint32_t format : 8, stream : 5, offset : 19; } VertexAttribState;
FITS(VertexAttribState, 4);
typedef struct { uint32_t stride, divisor; } VertexStreamState;

static inline MemoryPool *pool_of(uint64_t guest) { return gptr(guest); }
/* The memory pool containing a GPU (= guest) address: 1 and its storage/size, or 0 */
int nvn_pool_find(uint64_t address, uint64_t *storage, uint64_t *size);
static inline uint64_t pool_address(uint64_t pool, uint64_t offset) {
    return pool ? pool_of(pool)->storage + offset : 0;
}

/* ================================================================ texture layout ========= */

typedef struct {
    uint8_t bytes;                  /* bytes per block (per texel when uncompressed) */
    uint8_t bw, bh;                 /* block size in texels (4x4 for BCn, varies for ASTC) */
    uint8_t kind;                   /* FORMAT_COLOR, ... */
} FormatInfo;
enum { FORMAT_COLOR, FORMAT_DEPTH, FORMAT_STENCIL, FORMAT_DEPTH_STENCIL, FORMAT_COMPRESSED };
const FormatInfo *nvn_format(uint32_t format);

enum {                              /* NVNtextureTarget */
    TARGET_1D, TARGET_2D, TARGET_3D, TARGET_1D_ARRAY, TARGET_2D_ARRAY, TARGET_2D_MS, TARGET_2D_MS_ARRAY,
    TARGET_RECTANGLE, TARGET_CUBE, TARGET_CUBE_ARRAY, TARGET_BUFFER,
};
enum { TEXTURE_FLAG_LINEAR = 0x10 }; /* NVNtextureFlags: pitch-linear storage (rows of `stride` bytes) */
enum { TEXTURE_FLAG_COMPRESSIBLE = 0x8 }; /* GPU-compressed storage: render targets, never filled by the CPU */

typedef struct {                    /* a texture's size in texels, for layout */
    const FormatInfo *fmt;
    uint32_t width, height, depth, layers, levels;
    uint32_t block_height_log2, block_depth_log2;   /* of level 0 */
} TexShape;

typedef struct {                    /* one mip level of one layer */
    uint64_t offset, size;          /* bytes, from the start of the layer */
    uint32_t width, height, depth;  /* texels */
    uint32_t wb, hb;                /* blocks (= texels unless compressed) */
    uint32_t bh, bd;                /* log2 of block height in GOBs / block depth */
} LevelShape;

void tex_shape(const TexDesc *d, TexShape *s);
LevelShape tex_level(const TexShape *s, uint32_t level);
uint64_t tex_layer_size(const TexShape *s);
uint64_t texture_storage_size(const TexDesc *d);
uint32_t texture_storage_alignment(const TexDesc *d);
/* copy (part of) a block-linear level to or from linear memory; region in blocks: x, y, z, w, h, d */
void tex_copy_level(uint8_t *tiled, uint8_t *linear, const TexShape *s, const LevelShape *l,
                    const uint32_t region[6], uint64_t row_pitch, uint64_t slice_pitch, int to_linear);

/* ================================================================ command records ======== */

/* Recorded commands live in the game's command memory, one Cmd after another. The X-macro
 * list gives each an op number and a name for tracing. */
#define NVN_COMMANDS(X) \
    X(END) X(JUMP) X(CALL) X(NOP) \
    X(BIND_PROGRAM) X(BIND_STATE) X(BIND_VERTEX_ATTRIBS) X(BIND_VERTEX_STREAMS) \
    X(BIND_VERTEX_BUFFERS) X(BIND_UNIFORM_BUFFERS) X(BIND_STORAGE_BUFFERS) X(BIND_TEXTURES) \
    X(BIND_IMAGES) X(BIND_SEPARATE_SAMPLERS) X(SET_TEXTURE_POOL) X(SET_SAMPLER_POOL) \
    X(SET_RENDER_TARGETS) X(SET_VIEWPORTS) X(SET_SCISSORS) X(SET_DEPTH_RANGES) X(SET_VALUE) \
    X(CLEAR_COLOR) X(CLEAR_DEPTH_STENCIL) X(CLEAR_TEXTURE) X(CLEAR_BUFFER) X(DISCARD_COLOR) \
    X(DRAW_ARRAYS) X(DRAW_ELEMENTS) X(DRAW_ARRAYS_INDIRECT) X(DRAW_ELEMENTS_INDIRECT) \
    X(MULTI_DRAW_ARRAYS_INDIRECT_COUNT) X(MULTI_DRAW_ELEMENTS_INDIRECT_COUNT) \
    X(DISPATCH) X(DISPATCH_INDIRECT) \
    X(COPY_BUFFER_TO_BUFFER) X(COPY_BUFFER_TO_TEXTURE) X(COPY_TEXTURE_TO_BUFFER) \
    X(COPY_TEXTURE_TO_TEXTURE) X(DOWNSAMPLE) X(UPDATE_UNIFORM_BUFFER) \
    X(FENCE_SYNC) X(WAIT_SYNC) X(BARRIER) X(SIGNAL_EVENT) X(WAIT_EVENT) \
    X(REPORT_COUNTER) X(REPORT_VALUE) X(TRANSFORM_FEEDBACK)
#define NVN_OP_ENUM(name) CMD_##name,
enum { NVN_COMMANDS(NVN_OP_ENUM) CMD_COUNT };
extern const char *const nvn_command_names[];

/* Cmd.sub of CMD_BIND_STATE: which state object (its bytes are in Cmd.extra) */
enum { STATE_BLEND, STATE_CHANNEL_MASK, STATE_COLOR, STATE_DEPTH_STENCIL, STATE_MULTISAMPLE, STATE_POLYGON };

/* Cmd.sub of CMD_SET_VALUE: which nvnCommandBufferSet* call (arguments in a[] / f[] / extra) */
#define NVN_VALUES(X) \
    X(BLEND_COLOR) X(STENCIL_REF) X(STENCIL_MASK) X(STENCIL_VALUE_MASK) X(PRIMITIVE_RESTART) X(ALPHA_REF) \
    X(DEPTH_BOUNDS) X(POLYGON_OFFSET_CLAMP) X(LINE_WIDTH) X(POINT_SIZE) X(SAMPLE_MASK) X(RENDER_ENABLE) \
    X(RENDER_ENABLE_CONDITIONAL) X(DEPTH_CLAMP) X(RASTERIZER_DISCARD) X(CONSERVATIVE_RASTER_ENABLE) \
    X(CONSERVATIVE_RASTER_DILATE) X(COLOR_REDUCTION_ENABLE) X(COLOR_REDUCTION_THRESHOLDS) X(PATCH_SIZE) \
    X(INNER_TESS_LEVELS) X(OUTER_TESS_LEVELS) X(COPY_ROW_STRIDE) X(COPY_IMAGE_STRIDE) X(SHADER_SCRATCH_MEMORY) \
    X(VIEWPORT_SWIZZLES) X(SUBPIXEL_PRECISION_BIAS) X(STENCIL_CULL_CRITERIA) X(TILED_CACHE_ACTION) \
    X(TILED_CACHE_TILE_SIZE) X(PROGRAM_SUBROUTINES) X(COVERAGE_MODULATION_TABLE) X(DISCARD_DEPTH_STENCIL) \
    X(SAVE_ZCULL) X(RESTORE_ZCULL) X(RESOLVE_DEPTH) X(RESET_COUNTER)
#define NVN_VALUE_ENUM(name) VALUE_##name,
enum { NVN_VALUES(NVN_VALUE_ENUM) VALUE_COUNT };

typedef struct {
    uint32_t op;
    uint32_t size;                  /* whole record in bytes, multiple of 8 */
    uint32_t sub;                   /* which variant (e.g. which state for BIND_STATE) */
    uint32_t pad;
    uint64_t a[7];                  /* the call's integer arguments after the command buffer */
    float    f[4];                  /* ...and its float arguments */
    uint8_t  extra[];               /* data copied from pointers the game passed */
} Cmd;

/* An image in a command (render target, copy source/destination): copied when recorded, since
 * the game may reuse its view and region structs right after the call. */
typedef struct {
    uint64_t texture;               /* guest NVNtexture * */
    TextureView view;               /* view.set == 0: no view */
    int32_t  region[6];             /* x, y, z, width, height, depth (copies and clears) */
} ImageRef;

typedef struct {
    uint32_t count;
    ImageRef color[8];
    ImageRef depth;                 /* depth.texture == 0: none */
} RenderTargets;

void nvn_execute(uint64_t handle);  /* exec.c: run a finalized command list */
void nvn_present(const Window *w, uint64_t texture);
uint64_t nvn_frame_count(void);      /* frames presented so far */
uint32_t nvn_depth_mode(void);
int nvn_sync_tracing(void);          /* inside the LAA_TRACE_SYNC window */       /* nvnDeviceSetDepthMode: 0 near is -w (OpenGL), 1 near is 0 */
/* Put RGBA8 pixels (rows of width * 4 bytes) into a texture's level 0, e.g. a movie frame */
void nvn_write_texture(uint64_t texture, const uint8_t *rgba, uint32_t width, uint32_t height);
void nvn_texels_written(uint64_t texture, uint32_t level, uint32_t layer, const uint32_t region[6]);  /* CPU wrote texels */
void nvn_pool_changed(uint64_t storage, uint64_t size, int created);   /* a memory pool was made / is going */
/* NVNsync on the renderer's GPU timeline (exec.c): fence it at the work submitted so far / wait */
void nvn_sync_fence(Sync *s);
int nvn_sync_wait(const Sync *s, uint64_t timeout_ns);   /* 0 when signalled, -1 on timeout */
void nvn_queue_finish(void);
uint64_t nvn_gpu_ticks(void);       /* GPU timestamp counter, in units of 625/384 ns */

#endif
