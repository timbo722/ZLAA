/*
 * render_internal.h - shared between the renderer's files (runtime/render/).
 */
#ifndef RENDER_INTERNAL_H
#define RENDER_INTERNAL_H

#include "render.h"
#include "kernel.h"                          /* (host_thread_init) */
#include <pthread.h>
#include "vk.h"
#include "nvn/nvn.h"

/* A Vulkan image standing for a guest texture (image.c) */
typedef struct HostImage {
    struct HostImage *next;          /* hash bucket chain */
    uint64_t address;                /* guest address of the texture's storage */
    TexDesc desc;
    VkImage image;
    VkDeviceMemory memory;
    VkImageView target_view;         /* level 0, layer 0: for rendering into */
    VkFormat format;
    VkImageAspectFlags aspects;
    uint32_t width, height, depth, layers, levels;
    int needs_upload;                /* guest memory has data the image doesn't */
    int layout_ready;                /* moved to VK_IMAGE_LAYOUT_GENERAL */
    int gpu_written;                 /* rendered into: the image is newer than guest memory */
    int astc_decode;                 /* ASTC the GPU can't sample: decoded to RGBA8 on upload */
    VkFormat view_format;            /* the other format views may have (sRGB <-> UNORM twin), or UNDEFINED */
    uint64_t hash;                   /* of its guest memory when last uploaded (CPU writes, image.c) */
    uint64_t checked;                /* frame of the last hash check */
    uint64_t hot_until;              /* changed recently: check every frame until this frame */
    uint64_t gpu_frame;              /* frame of the last GPU write */
    int hash_valid;                  /* `hash` describes guest memory as it is now */
    int preloaded;                   /* uploaded before its first use: that use checks the hash */
    struct ImageView *views;         /* views made for sampling it (image.c) */
    VkImageAspectFlags pending_clear;   /* cleared, but not yet: see render_image_clear */
    VkClearValue clear_value;
} HostImage;

VkFormat render_vk_format(uint32_t nvn_format);
VkImageAspectFlags render_aspects(VkFormat f);
HostImage *render_image(const Texture *t);       /* find or create; NULL for unsupported formats */
HostImage *render_image_find(uint64_t texture);  /* find only */
void render_reupload_all(void);
void render_images_new_frame(void);              /* once per presented frame */
void render_image_gpu_wrote(HostImage *h);       /* the GPU wrote h: it is newer than guest memory */
int render_tracing(void);                        /* LAA_TRACE_PIPELINES, within LAA_TRACE_FROM/_FRAMES */
void render_debug_dump(HostImage *h, const char *path);   /* save a colour image as a PNG (debugging) */
void render_upload(HostImage *h);                /* guest memory -> image, recorded in render_cmd() */
void render_image_init_layout(VkCommandBuffer cmd, HostImage *h);
/* the image for a texture, ready to use: uploaded if the CPU's copy is newer, cleared if a
 * clear is pending; render_image_target: the same, but leaves a pending clear to the render pass */
HostImage *render_image_ready(uint64_t texture);
HostImage *render_image_target(uint64_t texture);
void render_image_clear(HostImage *h, const VkClearValue *value, VkImageAspectFlags aspects);
void render_image_flush_clear(HostImage *h);
/* A view of an image for sampling, as a shader texture of `dim`/`array` wants it, applying
 * an NVN texture view (view->set == 0: the whole texture) */
VkImageView render_image_view(HostImage *h, const TextureView *view, int dim, int array);
/* A 1x1 black image view of the given kind, for textures we can't provide */
VkImageView render_dummy_view(int dim, int array);
VkSampler render_sampler(const SamplerDesc *d);

/* ---- submissions (submit.c) ---- */
/* The command buffer being recorded; recording starts on first use. */
VkCommandBuffer render_cmd(void);
/* Send what was recorded to the GPU without waiting (submit.c's notes). */
void render_submit(void);
extern pthread_mutex_t render_queue_lock;          /* held around everything that uses vk.queue */
void render_count_work(void);                    /* a draw or dispatch was recorded (submit.c) */
uint32_t render_recorded_work(void);             /* how many since the last submission */
void render_collect_compile_stats(void);          /* background pipeline compiles into render_compile_ms (pipeline.c) */
#define RENDER_CONTEXTS 8                        /* submissions the CPU may be ahead of the GPU (submit.c) */
int render_context(void);                        /* the context being recorded (0 .. NCTX-1) */
int render_serial_done(uint64_t serial);
void render_pools_reap(void);                     /* free finalized pools the GPU is done with (buffer.c) */
/* LAA_GPU_TIMING_AT: GPU timestamp after a command, with a label to add its time to */
extern int render_gpu_timing;
extern int render_passes;
void render_timestamp(const char *label);
void render_gpu_timing_frame(uint64_t frame);
/* A full memory barrier: everything before is done and visible to everything after. */
void render_barrier(VkCommandBuffer cmd);
/* Space in the staging buffer (CPU-writable memory the GPU copies from), valid until the
 * next flush. May flush first to make room, so call it before render_cmd(). */
void *render_staging(uint64_t size, uint64_t *offset);
VkBuffer render_staging_buffer(void);
/* Uniform data written by the CPU (e.g. our c0), valid until the next flush */
void *render_uniforms(uint32_t size, VkBuffer *buffer, VkDeviceSize *offset);

/* ---- GPU access to guest memory (buffer.c) ---- */
/* The VkBuffer and offset for a GPU (= guest) address inside a memory pool; 0 on success */
int render_guest_buffer(uint64_t address, VkBuffer *buffer, VkDeviceSize *offset);
uint64_t render_guest_device_address(uint64_t address);   /* for shaders' pointers; 0 if none */

/* ---- shaders and pipelines (pipeline.c) ---- */
#include "render/shader/shader.h"
typedef struct HostShader {
    struct HostShader *next;
    uint64_t code;
    int stage;
    uint32_t extra_outputs;          /* vertex: generic outputs added for the fragment shader (shader_translate) */
    VkShaderModule module;           /* 0: translation failed (info.error says why) */
    ShaderInfo info;
    uint64_t constants;              /* guest address and size of its c1 data */
    uint32_t constants_size;
    VkPipeline compute;              /* compute shaders: their pipeline (made on first use) */
} HostShader;
HostShader *render_shader(uint64_t code, uint64_t control, int stage, uint32_t extra_outputs);
/* Constant buffers c0..c15 of each stage are dynamic uniform buffers (their offset is given when
 * the set is bound), so draws that differ only in where their constants are share a descriptor
 * set (draw.c). The Thor allows 32 per set: 16 for each of two stages. */
#define RENDER_DYNAMIC_CBUFS 16
void render_capture_print(uint64_t frame);       /* LAA_DEBUG_VS (pipeline.c) */

typedef struct { uint8_t src_color, dst_color, src_alpha, dst_alpha, eq_color, eq_alpha; } BlendKey;

/* Everything a pipeline depends on. Zero it before filling it in: it is compared bytewise. */
typedef struct {
    VkShaderModule vs, fs;
    uint64_t vs_code, fs_code;       /* (for messages) */
    uint32_t vs_inputs;              /* attribute locations the vertex shader reads */
    uint8_t nattribs;
    struct { uint8_t format, stream; uint16_t offset; } attr[16];
    struct { uint32_t stride, divisor; } stream[16];
    uint8_t topology, primitive_restart, cull, front_ccw, rasterizer_discard, depth_bias, alpha_to_coverage;
    uint8_t depth_test, depth_write, depth_func, stencil_test, stencil_func[2];
    uint16_t stencil_ops[2];
    uint8_t blend_enable, logic_op, ncolor, pad;
    uint32_t channel_mask;
    uint32_t vs_extra;               /* the vertex shader variant's added outputs (render_shader) */
    BlendKey blend[8];
    VkFormat color_format[8], depth_format;
} PipelineKey;
VkPipeline render_pipeline(const PipelineKey *k);
int render_pass_objects(void);                   /* passes with VkRenderPass objects, not dynamic rendering (draw.c) */
VkRenderPass render_compatible_pass(uint32_t ncolor, const VkFormat *color, VkFormat depth);   /* (thread-safe) */
void render_draw_copy(VkCommandBuffer cmd, VkImageView src_view, uint32_t src_w, uint32_t src_h, const VkOffset3D sb[2],
                      VkImageView dst_view, VkFormat dst_format, uint32_t dst_w, uint32_t dst_h, const VkOffset3D db[2],
                      int linear, int flip);       /* (draw.c: a copy by drawing, outside a pass) */
void render_load_pipeline_keys(const char *path);   /* remembered pipelines (pipeline.c), from vk.c */
void render_save_pipeline_keys(const char *path);
VkPipelineLayout render_pipeline_layout(void);
VkDescriptorSetLayout render_set_layout(void);
VkPipelineLayout render_compute_layout(void);    /* compute: its own set (bindings from SHADER_COMPUTE * 64) */
VkDescriptorSetLayout render_compute_set_layout(void);
VkPipeline render_compute_pipeline(HostShader *cs);
VkCompareOp render_compare_op(uint32_t nvn_func);

/* ---- command execution (draw.c) ---- */
void render_end_rendering_at(const char *file, int line);   /* close the current render pass */
#define render_end_rendering() render_end_rendering_at(__FILE__, __LINE__)   /* (the trace says who ended it) */
void render_submission_done(int context);        /* the GPU finished that context's work: reuse it */

#endif
