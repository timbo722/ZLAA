/*
 * render.c - presenting frames.
 *
 * Presenting runs on its own thread. The game thread (render_present) records, after the
 * frame's rendering, a scaled copy of the shown texture into one of OUT_SLOTS window-sized
 * images (drawn: scaled, flipped and converted to the window's format; vkCmdBlitImage is slow on
 * the Adreno), adds the FPS box, and hands the slot over. The present thread acquires a swapchain
 * image, copies the slot into it (same size and format: a plain copy) and presents. Acquiring waits for the display, and Android's queueBuffer sleeps ~2 ms a
 * frame on the Thor (a system frame-pacing library): neither holds up the game any more. If the
 * game hands over frames faster than the display takes them, the older waiting one is dropped.
 *
 * The shown texture comes from its VkImage when the GPU rendered it; otherwise from guest
 * memory: converted from the Tegra block-linear layout to rows (layout.c) and uploaded first.
 */
#include "render_internal.h"
#include "png.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int active;
static volatile int resized;
static volatile int window_new;      /* Android gave us a new window (back from the background) */
static int surface_lost;             /* the window is gone: frames are dropped until a new one comes */

pthread_mutex_t render_queue_lock = PTHREAD_MUTEX_INITIALIZER;   /* vk.queue is used by two threads */

static struct {                      /* textures the GPU didn't render: uploaded from guest memory */
    VkBuffer staging;                /* CPU-visible copy of the texture */
    VkDeviceMemory staging_mem;
    void *staging_ptr;
    VkDeviceSize staging_size;
    VkImage image;                   /* the texture, as a Vulkan image */
    VkDeviceMemory image_mem;
    uint32_t width, height;
} frame;

#define OUT_SLOTS 3                  /* one being written, one waiting, one being presented */
static struct {
    VkImage image[OUT_SLOTS];        /* window-sized, in the window's format, layout GENERAL */
    VkDeviceMemory memory[OUT_SLOTS];
    VkImageView view[OUT_SLOTS];     /* (drawn into) */
    uint32_t width, height;
    VkCommandPool pool;              /* the present thread's own (pools are single-threaded) */
    VkCommandBuffer cmd[OUT_SLOTS];  /* the present thread's copy of each slot into the window */
    VkFence copied[OUT_SLOTS];       /* ... is finished */
    VkSemaphore acquired[OUT_SLOTS]; /* the swapchain image for that copy is ready */
    VkSemaphore rendered[8];         /* per swapchain image: our copy into it is done (an image
                                        can't be acquired again until its present used this) */
    int pending, busy;               /* slot waiting to be presented, slot being presented (-1: none) */
    pthread_mutex_t lock;
    pthread_cond_t cond;
} out = { .pending = -1, .busy = -1, .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };

int render_active(void) { return active; }
void render_resized(void) { resized = 1; }
void render_background(int on) {
    if (on) vk_save_pipeline_cache();
    else window_new = 1;
}

static void *present_thread(void *arg);

int render_init(void *sdl_window) {
    if (vk_init(sdl_window) != 0) return -1;
    render_guard_init();
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = vk.queue_family };
    VK_CHECK(vkCreateCommandPool(vk.device, &pci, NULL, &out.pool));
    VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                       .commandPool = out.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                       .commandBufferCount = OUT_SLOTS };
    VK_CHECK(vkAllocateCommandBuffers(vk.device, &ai, out.cmd));
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (int i = 0; i < OUT_SLOTS; i++) {
        VK_CHECK(vkCreateFence(vk.device, &fi, NULL, &out.copied[i]));
        VK_CHECK(vkCreateSemaphore(vk.device, &si, NULL, &out.acquired[i]));
    }
    for (int i = 0; i < 8; i++) VK_CHECK(vkCreateSemaphore(vk.device, &si, NULL, &out.rendered[i]));
    pthread_t t;
    pthread_create(&t, NULL, present_thread, NULL);
    pthread_detach(t);
    active = 1;
    return 0;
}

/* (Re)create the staging buffer and image for a width x height RGBA8 texture */
static void make_frame_image(uint32_t width, uint32_t height) {
    if (frame.width == width && frame.height == height) return;
    render_flush();                                  /* (nothing may still read the old ones) */
    if (frame.staging) {
        vkDestroyBuffer(vk.device, frame.staging, NULL);
        vkFreeMemory(vk.device, frame.staging_mem, NULL);
        vkDestroyImage(vk.device, frame.image, NULL);
        vkFreeMemory(vk.device, frame.image_mem, NULL);
    }
    frame.width = width;
    frame.height = height;
    frame.staging_size = (VkDeviceSize)width * height * 4;

    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = frame.staging_size,
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
    VK_CHECK(vkCreateBuffer(vk.device, &bi, NULL, &frame.staging));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(vk.device, frame.staging, &req);
    VkMemoryAllocateInfo mi = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
        .memoryTypeIndex = vk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VK_CHECK(vkAllocateMemory(vk.device, &mi, NULL, &frame.staging_mem));
    VK_CHECK(vkBindBufferMemory(vk.device, frame.staging, frame.staging_mem, 0));
    VK_CHECK(vkMapMemory(vk.device, frame.staging_mem, 0, VK_WHOLE_SIZE, 0, &frame.staging_ptr));

    VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_SRGB, .extent = { width, height, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VK_CHECK(vkCreateImage(vk.device, &ii, NULL, &frame.image));
    vkGetImageMemoryRequirements(vk.device, frame.image, &req);
    mi.allocationSize = req.size;
    mi.memoryTypeIndex = vk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(vk.device, &mi, NULL, &frame.image_mem));
    VK_CHECK(vkBindImageMemory(vk.device, frame.image, frame.image_mem, 0));
}

static void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                    VkAccessFlags src_access, VkAccessFlags dst_access) {
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = src_access,
        .dstAccessMask = dst_access, .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                         0, NULL, 0, NULL, 1, &b);
}

/* Copy the guest texture's level 0 into the staging buffer, as rows of RGBA8 */
static int read_guest_texture(const Texture *t) {
    TexShape s;
    tex_shape(&t->d, &s);
    if (s.fmt->bytes != 4 || s.fmt->bw != 1) return -1;          /* only 32-bit color formats for now */
    make_frame_image(s.width, s.height);
    LevelShape l = tex_level(&s, 0);
    uint32_t region[6] = { 0, 0, 0, l.wb, l.hb, 1 };
    if (t->d.flags & TEXTURE_FLAG_LINEAR) {
        for (uint32_t y = 0; y < s.height; y++)
            memcpy((uint8_t *)frame.staging_ptr + (size_t)y * s.width * 4, gptr(t->address + y * t->d.stride), s.width * 4);
    } else {
        tex_copy_level(gptr(t->address), frame.staging_ptr, &s, &l, region, (uint64_t)s.width * 4, 0, 1);
    }
    return 0;
}

int render_read_back(uint64_t texture, uint8_t *out) {
    HostImage *h = active ? render_image_find(texture) : NULL;
    if (!h || !h->gpu_written || !(h->aspects & VK_IMAGE_ASPECT_COLOR_BIT)) return -1;
    render_image_flush_clear(h);
    uint64_t size = (uint64_t)h->width * h->height * 4, offset;
    uint8_t *staging = render_staging(size, &offset);
    VkCommandBuffer cmd = render_cmd();
    VkBufferImageCopy copy = { .bufferOffset = offset, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                               .imageExtent = { h->width, h->height, 1 } };
    vkCmdCopyImageToBuffer(cmd, h->image, VK_IMAGE_LAYOUT_GENERAL, render_staging_buffer(), 1, &copy);
    render_flush();
    memcpy(out, staging, size);
    return 0;
}

/* ---- screenshots without stopping the game: the GPU copies the texture into one of
 * CAPTURE_SLOTS CPU-visible buffers in order with the frame's work, and capture threads wait
 * for it and write the PNG (scaled down by `scale`). A frame with no free buffer isn't saved.
 * Waiting for the copy right away (render_read_back) held each frame up ~100 ms on the Thor, and
 * a fade-in seen that way isn't the one the game shows. */
#define CAPTURE_SLOTS 12
static struct {
    VkBuffer buffer[CAPTURE_SLOTS];
    VkDeviceMemory memory[CAPTURE_SLOTS];
    void *ptr[CAPTURE_SLOTS];
    VkDeviceSize size;
    int busy[CAPTURE_SLOTS];
    struct { int slot; uint64_t serial; uint32_t width, height, scale; int32_t crop[4]; char path[512]; } job[CAPTURE_SLOTS];
    unsigned head, tail, dropped;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} cap = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };

static void *capture_thread(void *arg) {
    host_thread_init();                          /* (off the game's main core) */
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&cap.lock);
        while (cap.head == cap.tail) pthread_cond_wait(&cap.cond, &cap.lock);
        __typeof__(cap.job[0]) j = cap.job[cap.tail++ % CAPTURE_SLOTS];
        pthread_mutex_unlock(&cap.lock);
        render_wait_done(j.serial);
        const uint8_t *px = cap.ptr[j.slot];
        uint32_t x = 0, y = 0, w = j.width, h = j.height;
        if (j.crop[2] > 0 && j.crop[3] > 0 && (uint32_t)(j.crop[0] + j.crop[2]) <= j.width && (uint32_t)(j.crop[1] + j.crop[3]) <= j.height) {
            x = (uint32_t)j.crop[0]; y = (uint32_t)j.crop[1]; w = (uint32_t)j.crop[2]; h = (uint32_t)j.crop[3];
        }
        uint32_t ow = w / j.scale, oh = h / j.scale;    /* rows upwards: row 0 is the bottom (NVN, as OpenGL) */
        uint8_t *out = malloc((size_t)ow * oh * 4);
        if (out) {
            for (uint32_t oy = 0; oy < oh; oy++) {
                const uint8_t *row = px + ((size_t)(y + h - 1 - oy * j.scale) * j.width + x) * 4;
                for (uint32_t ox = 0; ox < ow; ox++) {
                    memcpy(out + ((size_t)oy * ow + ox) * 4, row + (size_t)ox * j.scale * 4, 4);
                    out[((size_t)oy * ow + ox) * 4 + 3] = 255;
                }
            }
            if (png_write(j.path, out, ow, oh, (int32_t)ow * 4) == 0) fprintf(stderr, "nvn: wrote %s\n", j.path);
            free(out);
        }
        pthread_mutex_lock(&cap.lock);
        cap.busy[j.slot] = 0;
        pthread_mutex_unlock(&cap.lock);
    }
    return NULL;
}

int render_capture(uint64_t texture, const char *path, const int32_t crop[4], uint32_t scale) {
    HostImage *h = active ? render_image_find(texture) : NULL;
    if (!h || !h->gpu_written || !(h->aspects & VK_IMAGE_ASPECT_COLOR_BIT)) return -1;
    VkDeviceSize size = (VkDeviceSize)h->width * h->height * 4;
    if (!cap.size) {                                 /* (first use: buffers for this size, two threads) */
        cap.size = size;
        for (int i = 0; i < CAPTURE_SLOTS; i++) {
            VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                                      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
            VK_CHECK(vkCreateBuffer(vk.device, &bi, NULL, &cap.buffer[i]));
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(vk.device, cap.buffer[i], &req);
            VkMemoryAllocateInfo mi = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                .memoryTypeIndex = vk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
            VK_CHECK(vkAllocateMemory(vk.device, &mi, NULL, &cap.memory[i]));
            VK_CHECK(vkBindBufferMemory(vk.device, cap.buffer[i], cap.memory[i], 0));
            VK_CHECK(vkMapMemory(vk.device, cap.memory[i], 0, size, 0, &cap.ptr[i]));
        }
        for (int i = 0; i < 2; i++) {
            pthread_t t;
            pthread_create(&t, NULL, capture_thread, NULL);
            pthread_detach(t);
        }
    }
    if (size > cap.size) return -1;
    pthread_mutex_lock(&cap.lock);
    int slot = -1;
    for (int i = 0; i < CAPTURE_SLOTS && slot < 0; i++)
        if (!cap.busy[i]) slot = i;
    if (slot >= 0) cap.busy[slot] = 1;
    pthread_mutex_unlock(&cap.lock);
    if (slot < 0) {
        if (cap.dropped++ % 60 == 0) fprintf(stderr, "render: screenshots can't keep up: %u frames not saved\n", cap.dropped);
        return 0;
    }
    render_image_flush_clear(h);
    render_end_rendering();
    VkBufferImageCopy copy = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { h->width, h->height, 1 } };
    vkCmdCopyImageToBuffer(render_cmd(), h->image, VK_IMAGE_LAYOUT_GENERAL, cap.buffer[slot], 1, &copy);
    render_barrier(render_cmd());
    pthread_mutex_lock(&cap.lock);
    __typeof__(cap.job[0]) *j = &cap.job[cap.head % CAPTURE_SLOTS];
    *j = (__typeof__(cap.job[0])){ .slot = slot, .serial = render_fence_serial(), .width = h->width, .height = h->height,
                                   .scale = scale ? scale : 1 };
    memcpy(j->crop, crop, sizeof j->crop);
    snprintf(j->path, sizeof j->path, "%s", path);
    cap.head++;
    pthread_cond_signal(&cap.cond);
    pthread_mutex_unlock(&cap.lock);
    return 0;
}

/* ---- LAA_SHOW_FPS=1: frames per second and the slowest frame (ms) of the last half second,
 * drawn in the bottom-left corner: text in a 5x7 pixel font, rasterized here, blitted 2x larger ---- */
#define OSD_CHARS 13
#define OSD_W (OSD_CHARS * 6 + 2)
#define OSD_H 9
static struct {
    int on;
    VkBuffer buffer;                 /* the pixels, put there by the command buffer (vkCmdUpdateBuffer):
                                        so the CPU never waits for the GPU to finish with them */
    VkDeviceMemory memory;
    uint32_t pixels[OSD_W * OSD_H];  /* RGBA8 */
    VkImage image;
    VkDeviceMemory image_memory;
    double window_start, last, worst;
    int frames;
    char text[OSD_CHARS + 1];
} osd = { .on = -1 };

static double osd_now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

/* 5x7 glyphs, one byte per row, bit 4 = leftmost column */
static const uint8_t *osd_glyph(char c) {
    static const uint8_t digits[10][7] = {
        { 14, 17, 19, 21, 25, 17, 14 }, { 4, 12, 4, 4, 4, 4, 14 }, { 14, 17, 1, 2, 4, 8, 31 }, { 31, 2, 4, 2, 1, 17, 14 },
        { 2, 6, 10, 18, 31, 2, 2 }, { 31, 16, 30, 1, 1, 17, 14 }, { 6, 8, 16, 30, 17, 17, 14 }, { 31, 1, 2, 4, 8, 8, 8 },
        { 14, 17, 17, 14, 17, 17, 14 }, { 14, 17, 17, 15, 1, 2, 12 } };
    static const uint8_t F[7] = { 31, 16, 16, 30, 16, 16, 16 }, P[7] = { 30, 17, 17, 30, 16, 16, 16 },
                         S[7] = { 15, 16, 16, 14, 1, 1, 30 }, M[7] = { 17, 27, 21, 21, 17, 17, 17 };
    if (c >= '0' && c <= '9') return digits[c - '0'];
    return c == 'F' ? F : c == 'P' ? P : c == 'S' ? S : c == 'M' ? M : NULL;
}

static void osd_init(void) {
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = OSD_W * OSD_H * 4,
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VK_CHECK(vkCreateBuffer(vk.device, &bi, NULL, &osd.buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(vk.device, osd.buffer, &req);
    VkMemoryAllocateInfo mi = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
        .memoryTypeIndex = vk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VK_CHECK(vkAllocateMemory(vk.device, &mi, NULL, &osd.memory));
    VK_CHECK(vkBindBufferMemory(vk.device, osd.buffer, osd.memory, 0));
    VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { OSD_W, OSD_H, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VK_CHECK(vkCreateImage(vk.device, &ii, NULL, &osd.image));
    vkGetImageMemoryRequirements(vk.device, osd.image, &req);
    mi.allocationSize = req.size;
    mi.memoryTypeIndex = vk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(vk.device, &mi, NULL, &osd.image_memory));
    VK_CHECK(vkBindImageMemory(vk.device, osd.image, osd.image_memory, 0));
    osd.window_start = osd.last = osd_now_ms();
    snprintf(osd.text, sizeof osd.text, "   FPS");
}

/* Count this frame; every half second, redraw the text (yellow when below 55 fps or a frame
 * took over 20 ms, i.e. missed a 60 Hz refresh). */
static void osd_update(void) {
    double t = osd_now_ms();
    if (t - osd.last > osd.worst) osd.worst = t - osd.last;
    osd.last = t;
    osd.frames++;
    if (t - osd.window_start < 500 && osd.frames > 1) return;
    double fps = osd.frames * 1000.0 / (t - osd.window_start);
    int slow = fps < 55 || osd.worst > 20;
    snprintf(osd.text, sizeof osd.text, "%2.0f FPS %3.0f MS", fps > 99 ? 99 : fps, osd.worst > 999 ? 999 : osd.worst);
    osd.window_start = t;
    osd.frames = 0;
    osd.worst = 0;
    uint32_t fg = slow ? 0xFF00FFFFu : 0xFFFFFFFFu, bg = 0xC0000000u;    /* ABGR in memory: R, G, B, A bytes */
    for (int i = 0; i < OSD_W * OSD_H; i++) osd.pixels[i] = bg;
    for (int k = 0; osd.text[k]; k++) {
        const uint8_t *g = osd_glyph(osd.text[k]);
        for (int y = 0; g && y < 7; y++)
            for (int x = 0; x < 5; x++)
                if (g[y] >> (4 - x) & 1) osd.pixels[(y + 1) * OSD_W + 1 + k * 6 + x] = fg;
    }
}

static void osd_draw(VkCommandBuffer cmd, VkImage dst) {      /* (dst: an out slot, GENERAL) */
    VkMemoryBarrier before = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                               .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };   /* (last frame's copy read it) */
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, NULL, 0, NULL);
    vkCmdUpdateBuffer(cmd, osd.buffer, 0, sizeof osd.pixels, osd.pixels);
    VkMemoryBarrier after = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                              .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &after, 0, NULL, 0, NULL);
    barrier(cmd, osd.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy copy = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { OSD_W, OSD_H, 1 } };
    vkCmdCopyBufferToImage(cmd, osd.buffer, osd.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier(cmd, osd.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    int32_t scale = out.height >= 1000 ? 2 : 1, x = 4, y = (int32_t)out.height - OSD_H * scale - 4;   /* bottom left */
    VkImageBlit blit = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                         .srcOffsets = { { 0, 0, 0 }, { OSD_W, OSD_H, 1 } },
                         .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                         .dstOffsets = { { x, y, 0 }, { x + OSD_W * scale, y + OSD_H * scale, 1 } } };
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };   /* (after the game's frame blit) */
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    vkCmdBlitImage(cmd, osd.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_NEAREST);
}

/* ---- the slots ---- */

/* (Re)create the slots at the window's size. Waits until the present thread holds none. */
static void make_out_images(void) {
    pthread_mutex_lock(&out.lock);
    while (out.busy >= 0) pthread_cond_wait(&out.cond, &out.lock);
    out.pending = -1;
    render_flush();
    VK_CHECK(vkWaitForFences(vk.device, OUT_SLOTS, out.copied, VK_TRUE, UINT64_MAX));
    for (int i = 0; i < OUT_SLOTS; i++) {
        if (out.image[i]) {
            vkDestroyImageView(vk.device, out.view[i], NULL);
            vkDestroyImage(vk.device, out.image[i], NULL);
            vkFreeMemory(vk.device, out.memory[i], NULL);
        }
        VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
            .format = vk.swapchain_format, .extent = { vk.extent.width, vk.extent.height, 1 }, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        VK_CHECK(vkCreateImage(vk.device, &ii, NULL, &out.image[i]));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(vk.device, out.image[i], &req);
        VkMemoryAllocateInfo mi = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
            .memoryTypeIndex = vk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
        VK_CHECK(vkAllocateMemory(vk.device, &mi, NULL, &out.memory[i]));
        VK_CHECK(vkBindImageMemory(vk.device, out.image[i], out.memory[i], 0));
        VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = out.image[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = vk.swapchain_format, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        VK_CHECK(vkCreateImageView(vk.device, &vi, NULL, &out.view[i]));
        barrier(render_cmd(), out.image[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    }
    out.width = vk.extent.width;
    out.height = vk.extent.height;
    pthread_mutex_unlock(&out.lock);
}

/* A slot the game thread may write: neither waiting nor being presented, its last copy done */
static int take_slot(void) {
    pthread_mutex_lock(&out.lock);
    int s = 0;
    while (s == out.pending || s == out.busy) s++;
    pthread_mutex_unlock(&out.lock);
    VK_CHECK(vkWaitForFences(vk.device, 1, &out.copied[s], VK_TRUE, UINT64_MAX));
    return s;
}

static void *present_thread(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&out.lock);
        while (out.pending < 0) pthread_cond_wait(&out.cond, &out.lock);
        int s = out.busy = out.pending;
        out.pending = -1;
        uint32_t width = out.width, height = out.height;
        pthread_mutex_unlock(&out.lock);

        if (window_new) {
            window_new = 0;
            pthread_mutex_lock(&render_queue_lock);
            vkDeviceWaitIdle(vk.device);
            surface_lost = vk_recreate_surface() != 0;
            pthread_mutex_unlock(&render_queue_lock);
            fprintf(stderr, "present: %s\n", surface_lost ? "no new window surface" : "new window surface");
            resized = 0;
        }
        if (resized && !surface_lost) {
            resized = 0;
            pthread_mutex_lock(&render_queue_lock);
            vkDeviceWaitIdle(vk.device);
            vk_create_swapchain(vk.extent.width, vk.extent.height);
            pthread_mutex_unlock(&render_queue_lock);
        }
        if (surface_lost) {                          /* (this frame isn't shown) */
            pthread_mutex_lock(&out.lock);
            out.busy = -1;
            pthread_cond_broadcast(&out.cond);
            pthread_mutex_unlock(&out.lock);
            continue;
        }
        uint32_t index;
        struct timespec ta, tb;
        clock_gettime(CLOCK_MONOTONIC, &ta);
        VkResult r = vkAcquireNextImageKHR(vk.device, vk.swapchain, UINT64_MAX, out.acquired[s], VK_NULL_HANDLE, &index);
        clock_gettime(CLOCK_MONOTONIC, &tb);
        double acquire_ms = (tb.tv_sec - ta.tv_sec) * 1e3 + (tb.tv_nsec - ta.tv_nsec) / 1e6;
        /* (Android may hold the window's buffers back: the GPU queue then waits at our copy, and
         * the game's frames behind it. Normally a few ms.) */
        if (acquire_ms > 30) fprintf(stderr, "present: acquiring a window image took %.0f ms\n", acquire_ms);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) resized = 1;
        else if (r == VK_ERROR_SURFACE_LOST_KHR) surface_lost = 1;   /* (Android took the window: the app is hidden) */
        else if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) vk_fail("vkAcquireNextImageKHR", r);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
            VkCommandBuffer cmd = out.cmd[s];
            VK_CHECK(vkResetCommandBuffer(cmd, 0));
            VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
            VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
            render_barrier(cmd);                     /* after the game thread's copy into the slot */
            VkImage dst = vk.images[index];
            barrier(cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
            static int present_blit = -1;            /* (LAA_PRESENT_BLIT: blit here too) */
            if (present_blit < 0) present_blit = getenv("LAA_PRESENT_BLIT") != NULL;
            if (width == vk.extent.width && height == vk.extent.height && !present_blit) {
                VkImageCopy c = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                  .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .extent = { width, height, 1 } };
                vkCmdCopyImage(cmd, out.image[s], VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            } else {                                 /* (the window changed size since this slot was written) */
                VkImageBlit blit = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                     .srcOffsets = { { 0, 0, 0 }, { (int32_t)width, (int32_t)height, 1 } },
                                     .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                     .dstOffsets = { { 0, 0, 0 }, { (int32_t)vk.extent.width, (int32_t)vk.extent.height, 1 } } };
                vkCmdBlitImage(cmd, out.image[s], VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                               VK_FILTER_LINEAR);
            }
            barrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
            VK_CHECK(vkEndCommandBuffer(cmd));
            VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
                                .pWaitSemaphores = &out.acquired[s], .pWaitDstStageMask = &wait_stage,
                                .commandBufferCount = 1, .pCommandBuffers = &cmd,
                                .signalSemaphoreCount = 1, .pSignalSemaphores = &out.rendered[index] };
            VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                                    .pWaitSemaphores = &out.rendered[index], .swapchainCount = 1,
                                    .pSwapchains = &vk.swapchain, .pImageIndices = &index };
            VK_CHECK(vkResetFences(vk.device, 1, &out.copied[s]));
            pthread_mutex_lock(&render_queue_lock);
            VK_CHECK(vkQueueSubmit(vk.queue, 1, &si, out.copied[s]));
            clock_gettime(CLOCK_MONOTONIC, &ta);
            r = vkQueuePresentKHR(vk.queue, &pi);
            clock_gettime(CLOCK_MONOTONIC, &tb);
            pthread_mutex_unlock(&render_queue_lock);
            double present_ms = (tb.tv_sec - ta.tv_sec) * 1e3 + (tb.tv_nsec - ta.tv_nsec) / 1e6;
            if (present_ms > 30) fprintf(stderr, "present: vkQueuePresentKHR took %.0f ms (holding the queue)\n", present_ms);
            /* (SUBOPTIMAL is fine: Android says it whenever it rotates for us; resizes come as SDL events) */
            if (r == VK_ERROR_OUT_OF_DATE_KHR) resized = 1;
            else if (r == VK_ERROR_SURFACE_LOST_KHR) surface_lost = 1;
            else if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) vk_fail("vkQueuePresentKHR", r);
        }
        pthread_mutex_lock(&out.lock);
        out.busy = -1;
        pthread_cond_broadcast(&out.cond);
        pthread_mutex_unlock(&out.lock);
    }
    return NULL;
}

void render_present(uint64_t texture, const int32_t crop[4]) {
    static int reupload = -1;
    if (reupload < 0) reupload = getenv("LAA_REUPLOAD") != NULL;
    if (reupload) render_reupload_all();
    render_images_new_frame();
    render_pools_reap();
    if (!active || !texture) return;
    HostImage *h = render_image_find(texture);
    if (h) render_image_flush_clear(h);              /* (cleared but never drawn into) */
    render_submit();                                 /* the frame's rendering (the copy below comes after it) */
    render_timestamps_update();
    render_gpu_timing_frame(nvn_frame_count());
    render_capture_print(nvn_frame_count());
    render_guard_check(nvn_frame_count());
    if (osd.on < 0 && (osd.on = getenv("LAA_SHOW_FPS") && atoi(getenv("LAA_SHOW_FPS")))) osd_init();
    if (osd.on) osd_update();
    int from_gpu = h && h->gpu_written;
    if (!from_gpu && read_guest_texture(gptr(texture)) != 0) return;
    VkImage src = from_gpu ? h->image : frame.image;
    int32_t width = (int32_t)(from_gpu ? h->width : frame.width), height = (int32_t)(from_gpu ? h->height : frame.height);
    if (out.width != vk.extent.width || out.height != vk.extent.height) make_out_images();
    int s = take_slot();

    VkCommandBuffer cmd = render_cmd();
    VkImageLayout src_layout = VK_IMAGE_LAYOUT_GENERAL;
    if (!from_gpu) {                                 /* staging buffer -> frame image */
        barrier(cmd, frame.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy copy = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                   .imageExtent = { frame.width, frame.height, 1 } };
        vkCmdCopyBufferToImage(cmd, frame.staging, frame.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier(cmd, frame.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        src_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }
    /* frame image -> slot (scaled to fit) */
    VkImageBlit blit = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffsets = { { 0, 0, 0 }, { width, height, 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        /* upside down: NVN's (OpenGL's) row 0 is the bottom of the screen, Vulkan's the top */
        .dstOffsets = { { 0, (int32_t)out.height, 0 }, { (int32_t)out.width, 0, 1 } },
    };
    if (crop && crop[2] > 0 && crop[3] > 0 && crop[0] + crop[2] <= width && crop[1] + crop[3] <= height) {
        /* the window's crop: only this part of the texture is shown (dynamic resolution) */
        blit.srcOffsets[0] = (VkOffset3D){ crop[0], crop[1], 0 };
        blit.srcOffsets[1] = (VkOffset3D){ crop[0] + crop[2], crop[1] + crop[3], 1 };
    }
    static int present_blit = -1;                    /* LAA_PRESENT_BLIT=1: the frame -> slot by vkCmdBlitImage, as before */
    if (present_blit < 0) present_blit = getenv("LAA_PRESENT_BLIT") != NULL;
    if (from_gpu && !present_blit) {                 /* (drawn: blits are slow on the Adreno) */
        VkOffset3D sb[2] = { blit.srcOffsets[0], blit.srcOffsets[1] };
        VkOffset3D db[2] = { { 0, 0, 0 }, { (int32_t)out.width, (int32_t)out.height, 1 } };
        render_image_init_layout(cmd, h);
        render_barrier(cmd);
        render_draw_copy(cmd, render_image_view(h, &(TextureView){ 0 }, TEXDIM_2D, 0), h->width, h->height, sb, out.view[s],
                         vk.swapchain_format, out.width, out.height, db, 1, 1);
    } else {
        vkCmdBlitImage(cmd, src, src_layout, out.image[s], VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_LINEAR);
    }
    if (osd.on) osd_draw(cmd, out.image[s]);
    render_barrier(cmd);
    render_submit();
    if (!from_gpu) render_flush();                   /* (the staging buffer is used again next frame) */

    pthread_mutex_lock(&out.lock);                   /* hand it over (a frame still waiting is dropped) */
    out.pending = s;
    pthread_cond_broadcast(&out.cond);
    pthread_mutex_unlock(&out.lock);
}
