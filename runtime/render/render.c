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
#include "render_ui.h"
#include "png.h"
#include <math.h>
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

#define OUT_SLOTS 4                  /* one being written, one waiting, one being presented; one the menu draws or keeps */
static struct {
    VkImage image[OUT_SLOTS];        /* window-sized, in the window's format, layout GENERAL */
    VkDeviceMemory memory[OUT_SLOTS];
    VkImageView view[OUT_SLOTS];     /* (drawn into) */
    VkImageView ui_view[OUT_SLOTS];  /* the same as UNORM (the menu's colors are sRGB values already) */
    uint32_t width, height;
    VkCommandPool pool;              /* the present thread's own (pools are single-threaded) */
    VkCommandBuffer cmd[OUT_SLOTS];  /* the present thread's copy of each slot into the window */
    VkFence copied[OUT_SLOTS];       /* ... is finished */
    VkSemaphore acquired[OUT_SLOTS]; /* the swapchain image for that copy is ready */
    VkSemaphore rendered[8];         /* per swapchain image: our copy into it is done (an image
                                        can't be acquired again until its present used this) */
    int pending, busy;               /* slot waiting to be presented, slot being presented (-1: none) */
    int last;                        /* the slot handed over last (-1: none yet) */
    int ui_slot;                     /* the slot the menu is drawing (-1: none) */
    int game_slot;                   /* the slot the game thread is writing (-1: none) */
    int ui_on;                       /* the pause menu shows: the game's frames aren't shown */
    pthread_mutex_t lock;
    pthread_cond_t cond;
} out = { .pending = -1, .busy = -1, .last = -1, .ui_slot = -1, .game_slot = -1, .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };

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

/* ---- the overlays, each a rounded dark pill in a corner of the game's picture: LAA_SHOW_FPS=1, the frame rate
 * (frames counted over half a second; coral when under 55 or a frame took over 20 ms, i.e. missed a 60 Hz refresh)
 * in corner LAA_FPS_CORNER; LAA_SHADER_OSD=1, "Compiling shaders" in the corner beside it for a second after a
 * pipeline compile missed the driver's cache. The CPU draws them into a small texture (one row each; Nunito's
 * letters from osd_font.h, premultiplied colours), uploads a row when it changes, and they are drawn over the
 * frame with blending ---- */
#include "osd_font.h"
#define OSD_TW 256
#define OSD_ROW 64                       /* (a row: OSD_TW x OSD_ROW x 4 = 64 KB, one vkCmdUpdateBuffer) */
enum { OSD_FPS, OSD_SHADERS, OSD_ROWS };
static struct {
    int on, shaders;                     /* LAA_SHOW_FPS, LAA_SHADER_OSD */
    int corner;                          /* LAA_FPS_CORNER: 0 bottom left, 1 bottom right, 2 top left, 3 top right */
    VkBuffer buffer;                     /* the pixels, put there by the command buffer (vkCmdUpdateBuffer):
                                            so the CPU never waits for the GPU to finish with them */
    VkDeviceMemory memory;
    uint32_t pixels[OSD_ROWS][OSD_ROW][OSD_TW];   /* RGBA8 (sRGB), premultiplied */
    VkImage image;
    VkDeviceMemory image_memory;
    VkImageView view;
    int image_ready;                     /* (its layout is GENERAL, with both rows) */
    int dirty[OSD_ROWS], w[OSD_ROWS], h; /* rows to upload; each pill's width, their height */
    int big;                             /* 1080p sizes, else 720p's */
    int shown;                           /* the shader pill is showing */
    double window_start, last, worst, drawn_at;
    int frames, fps, slow;
} osd = { .on = -1 };

static double osd_now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static void osd_init(void) {
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sizeof osd.pixels,
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VK_CHECK(vkCreateBuffer(vk.device, &bi, NULL, &osd.buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(vk.device, osd.buffer, &req);
    VkMemoryAllocateInfo mi = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
        .memoryTypeIndex = vk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VK_CHECK(vkAllocateMemory(vk.device, &mi, NULL, &osd.memory));
    VK_CHECK(vkBindBufferMemory(vk.device, osd.buffer, osd.memory, 0));
    VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_SRGB, .extent = { OSD_TW, OSD_ROW * OSD_ROWS, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VK_CHECK(vkCreateImage(vk.device, &ii, NULL, &osd.image));
    vkGetImageMemoryRequirements(vk.device, osd.image, &req);
    mi.allocationSize = req.size;
    mi.memoryTypeIndex = vk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(vk.device, &mi, NULL, &osd.image_memory));
    VK_CHECK(vkBindImageMemory(vk.device, osd.image, osd.image_memory, 0));
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = osd.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_SRGB,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VK_CHECK(vkCreateImageView(vk.device, &vi, NULL, &osd.view));
    osd.window_start = osd.last = osd_now_ms();
    osd.big = -1;
}

/* Painting into a row: premultiplied src over dst */
static void osd_blend(uint32_t *d, const uint8_t c[3], float a) {
    if (a <= 0) return;
    if (a > 1) a = 1;
    uint32_t v = *d, out = 0;
    for (int k = 0; k < 4; k++) {
        float dst = (float)(v >> (8 * k) & 255), src = k < 3 ? c[k] * a : 255 * a;
        out |= (uint32_t)(src + dst * (1 - a) + 0.5f) << (8 * k);
    }
    *d = out;
}

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

/* A pill w x h at the row's top left: dark, a faint gold edge (anti-aliased by the distance from its middle line) */
static void osd_pill(int row, int w, int h) {
    static const uint8_t bg[3] = { 11, 12, 9 }, edge[3] = { 233, 196, 106 };
    float r = h * 0.5f;
    memset(osd.pixels[row], 0, sizeof osd.pixels[row]);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float cx = x + 0.5f, cy = y + 0.5f, dx = cx < r ? r - cx : cx > w - r ? cx - (w - r) : 0, dy = cy - r;
            float d = sqrtf(dx * dx + dy * dy), inside = clamp01(r - d);
            osd_blend(&osd.pixels[row][y][x], bg, inside * 0.78f);
            osd_blend(&osd.pixels[row][y][x], edge, (inside - clamp01(r - 1.3f - d)) * 0.35f);
        }
}

/* Text along a baseline from x; returns its width (draw = 0: measure only) */
static int osd_text(int row, int x, int baseline, const OsdFont *f, const char *t, const uint8_t c[3], int draw) {
    int pen = x;
    for (; *t; t++) {
        if (*t < 32 || *t > 126) continue;
        const OsdGlyph *g = &f->glyphs[*t - 32];
        for (int gy = 0; draw && gy < g->h; gy++)
            for (int gx = 0; gx < g->w; gx++) {
                int px = pen + g->x0 + gx, py = baseline + g->y0 + gy;
                if (px >= 0 && px < OSD_TW && py >= 0 && py < OSD_ROW)
                    osd_blend(&osd.pixels[row][py][px], c, f->bits[g->offset + gy * g->w + gx] / 255.0f);
            }
        pen += g->advance;
    }
    return pen - x;
}

static void osd_draw_fps(void) {
    static const uint8_t text[3] = { 236, 231, 214 }, coral[3] = { 238, 132, 104 }, gold[3] = { 233, 196, 106 };
    const OsdFont *num = &osd_fonts[osd.big ? 2 : 1], *label = &osd_fonts[osd.big ? 1 : 0];
    int pad = osd.big ? 18 : 12, gap = osd.big ? 6 : 4;
    char n[8];
    snprintf(n, sizeof n, "%d", osd.fps);
    int wn = osd_text(OSD_FPS, 0, 0, num, n, text, 0), wl = osd_text(OSD_FPS, 0, 0, label, "FPS", gold, 0);
    osd.w[OSD_FPS] = pad + wn + gap + wl + pad;
    osd_pill(OSD_FPS, osd.w[OSD_FPS], osd.h);
    int base = (osd.h + num->glyphs['0' - 32].h) / 2;
    osd_text(OSD_FPS, pad, base, num, n, osd.slow ? coral : text, 1);
    osd_text(OSD_FPS, pad + wn + gap, base, label, "FPS", gold, 1);
    osd.dirty[OSD_FPS] = 1;
}

static void osd_draw_shaders(double t) {
    static const uint8_t text[3] = { 236, 231, 214 }, gold[3] = { 233, 196, 106 };
    const OsdFont *f = &osd_fonts[osd.big ? 1 : 0];
    int pad = osd.big ? 18 : 12, dot = osd.big ? 5 : 4, gap = osd.big ? 10 : 7;
    int wt = osd_text(OSD_SHADERS, 0, 0, f, "Compiling shaders", text, 0);
    osd.w[OSD_SHADERS] = pad + 2 * dot + gap + wt + pad;
    osd_pill(OSD_SHADERS, osd.w[OSD_SHADERS], osd.h);
    float pulse = 0.45f + 0.55f * (0.5f + 0.5f * sinf((float)(t / 1000.0 * 2 * 3.14159265)));   /* (once a second) */
    float cx = pad + dot, cy = osd.h * 0.5f;
    for (int y = 0; y < osd.h; y++)
        for (int x = pad - 1; x < pad + 2 * dot + 1; x++)
            osd_blend(&osd.pixels[OSD_SHADERS][y][x], gold,
                      clamp01(dot - sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy))) * pulse);
    osd_text(OSD_SHADERS, pad + 2 * dot + gap, (osd.h + f->glyphs['C' - 32].h) / 2, f, "Compiling shaders", text, 1);
    osd.dirty[OSD_SHADERS] = 1;
}

/* Every frame: count it; every half second, the frame rate again; the shader pill while compiles are slow */
static void osd_update(void) {
    double t = osd_now_ms();
    int big = out.height >= 900;
    if (big != osd.big) {
        osd.big = big;
        osd.h = big ? 44 : 30;
        if (osd.on && osd.w[OSD_FPS]) osd_draw_fps();
    }
    if (osd.on) {
        if (t - osd.last > osd.worst) osd.worst = t - osd.last;
        osd.last = t;
        osd.frames++;
        if (t - osd.window_start >= 500) {
            double fps = osd.frames * 1000.0 / (t - osd.window_start);
            int f = fps > 999 ? 999 : (int)(fps + 0.5), slow = fps < 55 || osd.worst > 20;
            if (f != osd.fps || slow != osd.slow || !osd.w[OSD_FPS]) { osd.fps = f; osd.slow = slow; osd_draw_fps(); }
            osd.window_start = t;
            osd.frames = 0;
            osd.worst = 0;
        }
    }
    osd.shown = osd.shaders && t - (double)render_last_slow_compile() < 1000;
    if (osd.shown && t - osd.drawn_at >= 100) { osd.drawn_at = t; osd_draw_shaders(t); }   /* (the dot pulses) */
}

static void osd_draw(VkCommandBuffer cmd, int s) {      /* (over out slot s, after the frame's copy) */
    int any = osd.dirty[OSD_FPS] || osd.dirty[OSD_SHADERS];
    if (any) {
        barrier(cmd, osd.image, osd.image_ready ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT);   /* (last frame's draw and copy read them) */
        for (int r = 0; r < OSD_ROWS; r++) {
            if (!osd.dirty[r] && osd.image_ready) continue;
            vkCmdUpdateBuffer(cmd, osd.buffer, sizeof osd.pixels[0] * r, sizeof osd.pixels[0], osd.pixels[r]);
        }
        VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                               .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        VkBufferImageCopy copy = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                   .imageExtent = { OSD_TW, OSD_ROW * OSD_ROWS, 1 } };
        vkCmdCopyBufferToImage(cmd, osd.buffer, osd.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier(cmd, osd.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        osd.image_ready = 1;
        osd.dirty[OSD_FPS] = osd.dirty[OSD_SHADERS] = 0;
    }
    render_barrier(cmd);                                  /* (after the frame's copy into the slot) */
    int margin = osd.big ? 18 : 12;
    for (int r = 0; r < OSD_ROWS; r++) {
        if ((r == OSD_FPS ? !osd.on : !osd.shown) || !osd.w[r]) continue;   /* (the counter: after its first count) */
        int corner = r == OSD_FPS ? osd.corner : osd.corner ^ 1;   /* (shaders: the other side, same edge) */
        int32_t x = corner & 1 ? (int32_t)out.width - osd.w[r] - margin : margin;
        int32_t y = corner & 2 ? margin : (int32_t)out.height - osd.h - margin;
        VkOffset3D sb[2] = { { 0, r * OSD_ROW, 0 }, { osd.w[r], r * OSD_ROW + osd.h, 1 } };
        VkOffset3D db[2] = { { x, y, 0 }, { x + osd.w[r], y + osd.h, 1 } };
        render_draw_copy(cmd, osd.view, OSD_TW, OSD_ROW * OSD_ROWS, sb, out.view[s], vk.swapchain_format, out.width,
                         out.height, db, 0, 0, 1);
    }
}

/* ---- the slots ---- */

static VkFormat ui_format(void) {
    return vk.swapchain_format == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM
         : vk.swapchain_format == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM : vk.swapchain_format;
}

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
            vkDestroyImageView(vk.device, out.ui_view[i], NULL);
            vkDestroyImage(vk.device, out.image[i], NULL);
            vkFreeMemory(vk.device, out.memory[i], NULL);
        }
        /* (the sRGB format and its UNORM twin, listed: a mutable image without a list loses UBWC) */
        VkFormat formats[2] = { vk.swapchain_format, ui_format() };
        VkImageFormatListCreateInfo fl = { .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
                                           .viewFormatCount = 2, .pViewFormats = formats };
        int twin = formats[1] != formats[0];
        VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = twin ? &fl : NULL,
            .flags = twin ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0, .imageType = VK_IMAGE_TYPE_2D,
            .format = vk.swapchain_format, .extent = { vk.extent.width, vk.extent.height, 1 }, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                     VK_IMAGE_USAGE_SAMPLED_BIT,
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
        vi.format = formats[1];
        VK_CHECK(vkCreateImageView(vk.device, &vi, NULL, &out.ui_view[i]));
        barrier(render_cmd(), out.image[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    }
    out.width = vk.extent.width;
    out.height = vk.extent.height;
    out.last = -1;
    pthread_mutex_unlock(&out.lock);
}

/* A slot the game thread may write: neither waiting nor being presented, its last copy done */
static int take_slot(void) {
    pthread_mutex_lock(&out.lock);
    int s = 0;
    while (s == out.pending || s == out.busy || s == out.ui_slot) s++;
    out.game_slot = s;
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
    static int settings_gen = -1;                    /* the overlays' settings: the menu can change them */
    if (settings_gen != kernel_settings_gen) {
        settings_gen = kernel_settings_gen;
        osd.on = getenv("LAA_SHOW_FPS") && atoi(getenv("LAA_SHOW_FPS"));
        osd.shaders = getenv("LAA_SHADER_OSD") && atoi(getenv("LAA_SHADER_OSD"));
        osd.corner = getenv("LAA_FPS_CORNER") ? atoi(getenv("LAA_FPS_CORNER")) & 3 : 0;
        if ((osd.on || osd.shaders) && !osd.buffer) osd_init();
        osd.w[OSD_FPS] = 0;                          /* (drawn again at the next count, from now) */
        osd.window_start = osd.last = osd_now_ms();
        osd.frames = 0;
        osd.worst = 0;
    }
    if (osd.on || osd.shaders) osd_update();
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
                         vk.swapchain_format, out.width, out.height, db, 1, 1, 0);
    } else {
        vkCmdBlitImage(cmd, src, src_layout, out.image[s], VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_LINEAR);
    }
    if ((osd.on && osd.w[OSD_FPS]) || osd.shown) osd_draw(cmd, s);
    render_barrier(cmd);
    render_submit();
    if (!from_gpu) render_flush();                   /* (the staging buffer is used again next frame) */

    pthread_mutex_lock(&out.lock);                   /* hand it over (a frame still waiting is dropped) */
    if (!out.ui_on) out.pending = s;                 /* (under the pause menu: kept as its background) */
    out.last = s;
    out.game_slot = -1;
    pthread_cond_broadcast(&out.cond);
    pthread_mutex_unlock(&out.lock);
}

/* ---- the menu (runtime/ui) draws whole frames itself: before the game starts, or while it is paused ---- */

VkFormat render_ui_format(void) { return ui_format(); }

int render_ui_begin(RenderUiTarget *t) {
    if (!active) return -1;
    if (out.width != vk.extent.width || out.height != vk.extent.height) {
        make_out_images();
        render_flush();                              /* (their layout changes, recorded there, come first) */
    }
    pthread_mutex_lock(&out.lock);                   /* one frame at a time: the display sets the pace */
    while (out.pending >= 0) pthread_cond_wait(&out.cond, &out.lock);
    int s = 0;
    while (s == out.busy || s == out.last || s == out.game_slot) s++;   /* (the game's last frame stays: the paused menu shows it) */
    t->last = out.last;
    out.ui_slot = s;
    pthread_mutex_unlock(&out.lock);
    VK_CHECK(vkWaitForFences(vk.device, 1, &out.copied[s], VK_TRUE, UINT64_MAX));
    t->slot = s;
    t->image = out.image[s];
    t->view = out.ui_view[s];
    t->last_view = t->last >= 0 ? out.ui_view[t->last] : VK_NULL_HANDLE;
    t->width = out.width;
    t->height = out.height;
    return 0;
}

void render_ui_set(int on) { out.ui_on = on; }

void render_ui_end(const RenderUiTarget *t) {
    pthread_mutex_lock(&out.lock);
    out.ui_slot = -1;
    out.pending = t->slot;
    pthread_cond_broadcast(&out.cond);
    pthread_mutex_unlock(&out.lock);
}
