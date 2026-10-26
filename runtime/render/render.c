/*
 * render.c - presenting frames.
 *
 * The presented texture is blitted into the window's swapchain image (vkCmdBlitImage scales
 * and converts RGBA to the window's BGRA). If the GPU rendered it, it comes from its VkImage;
 * otherwise from guest memory: the bytes are converted from the Tegra block-linear layout to
 * rows (layout.c) and uploaded first.
 */
#include "render_internal.h"
#include <stdio.h>
#include <stdlib.h>

static int active;
static volatile int resized;

static struct {
    VkCommandBuffer cmd;
    VkFence done;                    /* the GPU finished this frame's commands */
    VkSemaphore acquired;            /* the swapchain image is ready to draw into */
    VkSemaphore rendered[8];         /* per swapchain image: our copy into it is done (an image
                                        can't be acquired again until its present used this) */
    VkBuffer staging;                /* CPU-visible copy of the texture */
    VkDeviceMemory staging_mem;
    void *staging_ptr;
    VkDeviceSize staging_size;
    VkImage image;                   /* the texture, as a Vulkan image */
    VkDeviceMemory image_mem;
    uint32_t width, height;
} frame;

int render_active(void) { return active; }
void render_resized(void) { resized = 1; }

int render_init(void *sdl_window) {
    if (vk_init(sdl_window) != 0) return -1;
    VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                       .commandPool = vk.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                       .commandBufferCount = 1 };
    VK_CHECK(vkAllocateCommandBuffers(vk.device, &ai, &frame.cmd));
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    VK_CHECK(vkCreateFence(vk.device, &fi, NULL, &frame.done));
    VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VK_CHECK(vkCreateSemaphore(vk.device, &si, NULL, &frame.acquired));
    for (int i = 0; i < 8; i++) VK_CHECK(vkCreateSemaphore(vk.device, &si, NULL, &frame.rendered[i]));
    active = 1;
    return 0;
}

/* (Re)create the staging buffer and image for a width x height RGBA8 texture */
static void make_frame_image(uint32_t width, uint32_t height) {
    if (frame.width == width && frame.height == height) return;
    vkDeviceWaitIdle(vk.device);
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

void render_present(uint64_t texture, const int32_t crop[4]) {
    static int reupload = -1;
    if (reupload < 0) reupload = getenv("LAA_REUPLOAD") != NULL;
    if (reupload) render_reupload_all();
    render_images_new_frame();
    if (!active || !texture) return;
    render_submit();                                 /* the frame's rendering (the blit below comes after it) */
    render_gpu_timing_frame(nvn_frame_count());
    render_capture_print(nvn_frame_count());
    VK_CHECK(vkWaitForFences(vk.device, 1, &frame.done, VK_TRUE, UINT64_MAX));
    if (resized) {
        resized = 0;
        vkDeviceWaitIdle(vk.device);
        vk_create_swapchain(vk.extent.width, vk.extent.height);
    }
    HostImage *h = render_image_find(texture);
    int from_gpu = h && h->gpu_written;
    if (!from_gpu && read_guest_texture(gptr(texture)) != 0) return;
    VkImage src = from_gpu ? h->image : frame.image;
    int32_t width = (int32_t)(from_gpu ? h->width : frame.width), height = (int32_t)(from_gpu ? h->height : frame.height);

    uint32_t index;
    VkResult r = vkAcquireNextImageKHR(vk.device, vk.swapchain, UINT64_MAX, frame.acquired, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { resized = 1; return; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) vk_fail("vkAcquireNextImageKHR", r);
    VK_CHECK(vkResetFences(vk.device, 1, &frame.done));

    VkCommandBuffer cmd = frame.cmd;
    VK_CHECK(vkResetCommandBuffer(cmd, 0));
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    render_barrier(cmd);                             /* after the game's rendering, submitted before */
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
    /* frame image -> window (scaled to fit) */
    VkImage dst = vk.images[index];
    barrier(cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageBlit blit = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffsets = { { 0, 0, 0 }, { width, height, 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        /* upside down: NVN's (OpenGL's) row 0 is the bottom of the screen, Vulkan's the top */
        .dstOffsets = { { 0, (int32_t)vk.extent.height, 0 }, { (int32_t)vk.extent.width, 0, 1 } },
    };
    if (crop && crop[2] > 0 && crop[3] > 0 && crop[0] + crop[2] <= width && crop[1] + crop[3] <= height) {
        /* the window's crop: only this part of the texture is shown (dynamic resolution) */
        blit.srcOffsets[0] = (VkOffset3D){ crop[0], crop[1], 0 };
        blit.srcOffsets[1] = (VkOffset3D){ crop[0] + crop[2], crop[1] + crop[3], 1 };
    }
    vkCmdBlitImage(cmd, src, src_layout, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    barrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    render_barrier(cmd);                             /* the game's next rendering waits for this read */
    VK_CHECK(vkEndCommandBuffer(cmd));

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
                        .pWaitSemaphores = &frame.acquired, .pWaitDstStageMask = &wait_stage,
                        .commandBufferCount = 1, .pCommandBuffers = &cmd,
                        .signalSemaphoreCount = 1, .pSignalSemaphores = &frame.rendered[index] };
    VK_CHECK(vkQueueSubmit(vk.queue, 1, &si, frame.done));
    VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                            .pWaitSemaphores = &frame.rendered[index], .swapchainCount = 1,
                            .pSwapchains = &vk.swapchain, .pImageIndices = &index };
    r = vkQueuePresentKHR(vk.queue, &pi);
    /* (SUBOPTIMAL is fine: Android says it whenever it rotates for us; resizes come as SDL events) */
    if (r == VK_ERROR_OUT_OF_DATE_KHR) resized = 1;
    else if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) vk_fail("vkQueuePresentKHR", r);
}
