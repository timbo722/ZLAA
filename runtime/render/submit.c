/*
 * submit.c - recording and submitting GPU work.
 *
 * Simple for now: everything goes into one command buffer, and render_flush() submits it and
 * waits until the GPU has finished. So when an NVN submission returns, its work is done, and
 * every NVN fence can be considered signalled. (Letting the CPU run ahead is for later.)
 */
#include "render_internal.h"
#include <stdio.h>
#include <stdlib.h>

#define STAGING_SIZE (256ull << 20)

static VkCommandBuffer cmd;
static int recording;
static VkFence done;
static VkBuffer staging;
static VkDeviceMemory staging_memory;
static uint8_t *staging_ptr;
static uint64_t staging_used;

static void init(void) {
    VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                       .commandPool = vk.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                       .commandBufferCount = 1 };
    VK_CHECK(vkAllocateCommandBuffers(vk.device, &ai, &cmd));
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VK_CHECK(vkCreateFence(vk.device, &fi, NULL, &done));
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = STAGING_SIZE,
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VK_CHECK(vkCreateBuffer(vk.device, &bi, NULL, &staging));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(vk.device, staging, &req);
    VkMemoryAllocateInfo mi = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
        .memoryTypeIndex = vk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VK_CHECK(vkAllocateMemory(vk.device, &mi, NULL, &staging_memory));
    VK_CHECK(vkBindBufferMemory(vk.device, staging, staging_memory, 0));
    VK_CHECK(vkMapMemory(vk.device, staging_memory, 0, VK_WHOLE_SIZE, 0, (void **)&staging_ptr));
}

VkCommandBuffer render_cmd(void) {
    if (!cmd) init();
    if (!recording) {
        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        VK_CHECK(vkResetCommandBuffer(cmd, 0));
        VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
        recording = 1;
    }
    return cmd;
}

void render_flush(void) {
    if (!recording) { staging_used = 0; return; }
    render_end_rendering();
    VK_CHECK(vkEndCommandBuffer(cmd));
    recording = 0;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    VK_CHECK(vkQueueSubmit(vk.queue, 1, &si, done));
    VK_CHECK(vkWaitForFences(vk.device, 1, &done, VK_TRUE, UINT64_MAX));
    VK_CHECK(vkResetFences(vk.device, 1, &done));
    staging_used = 0;
}

void render_barrier(VkCommandBuffer c) {
    VkMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                          .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                          .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                         1, &b, 0, NULL, 0, NULL);
}

void *render_staging(uint64_t size, uint64_t *offset) {
    if (!cmd) init();
    size = (size + 255) & ~255ull;
    if (size > STAGING_SIZE) { fprintf(stderr, "render: %llu bytes don't fit the staging buffer\n", (unsigned long long)size); abort(); }
    if (staging_used + size > STAGING_SIZE) render_flush();
    *offset = staging_used;
    staging_used += size;
    return staging_ptr + *offset;
}

VkBuffer render_staging_buffer(void) { return staging; }
