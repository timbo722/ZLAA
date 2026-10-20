/*
 * render_internal.h - shared between the renderer's files (runtime/render/).
 */
#ifndef RENDER_INTERNAL_H
#define RENDER_INTERNAL_H

#include "render.h"
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
} HostImage;

VkFormat render_vk_format(uint32_t nvn_format);
VkImageAspectFlags render_aspects(VkFormat f);
HostImage *render_image(const Texture *t);       /* find or create; NULL for unsupported formats */
HostImage *render_image_find(uint64_t texture);  /* find only */
void render_upload(HostImage *h);                /* guest memory -> image, recorded in render_cmd() */
void render_image_init_layout(VkCommandBuffer cmd, HostImage *h);
/* the image for a texture, ready to use: uploaded if the CPU's copy is newer */
HostImage *render_image_ready(uint64_t texture);

/* ---- submissions (submit.c) ---- */
/* The command buffer being recorded; recording starts on first use. */
VkCommandBuffer render_cmd(void);
/* Submit what was recorded and wait for the GPU to finish it. */
void render_flush(void);
/* A full memory barrier: everything before is done and visible to everything after. */
void render_barrier(VkCommandBuffer cmd);
/* Space in the staging buffer (CPU-writable memory the GPU copies from), valid until the
 * next flush. May flush first to make room, so call it before render_cmd(). */
void *render_staging(uint64_t size, uint64_t *offset);
VkBuffer render_staging_buffer(void);

/* ---- command execution (draw.c) ---- */
void render_end_rendering(void);                 /* close the current vkCmdBeginRendering */

#endif
