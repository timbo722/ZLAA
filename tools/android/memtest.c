/*
 * memtest.c - can the game's memory and the GPU's memory be the same pages on this device?
 *
 * Desktop drivers let Vulkan use ordinary memory (VK_EXT_external_memory_host). Android's
 * Adreno driver doesn't. The idea tested here: let the driver allocate GPU-visible memory,
 * map it for the CPU, then MOVE that mapping (mremap) to a fixed address, where the game's
 * memory pool lives. If the GPU and the CPU then see each other's writes there, the renderer
 * can share memory with the game without copies, as on desktop.
 *
 * Build (NDK) and run on the device:
 *   $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang -O2 memtest.c -lvulkan -o memtest
 *   adb push memtest /data/local/tmp/ && adb shell /data/local/tmp/memtest
 */
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/mman.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAILED %s: %d\n", #x, r_); exit(1); } } while (0)
#define SIZE (64u << 20)

static VkDevice dev;
static VkQueue queue;
static VkCommandPool pool;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

/* one command buffer: fill [0, 4 KiB) with 0xA5A5A5A5, copy [8 KiB, 12 KiB) -> [16 KiB, 20 KiB) */
static void gpu_work(VkBuffer b) {
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkCommandBuffer cmd;
    CHECK(vkAllocateCommandBuffers(dev, &ai, &cmd));
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
    CHECK(vkBeginCommandBuffer(cmd, &bi));
    vkCmdFillBuffer(cmd, b, 0, 4096, 0xA5A5A5A5u);
    VkBufferCopy c = { 8192, 16384, 4096 };
    vkCmdCopyBuffer(cmd, b, b, 1, &c);
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cmd, 0, NULL };
    CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(queue));
    vkFreeCommandBuffers(dev, pool, 1, &cmd);
}

int main(void) {
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "memtest", 1, NULL, 0, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL };
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t n = 1;
    VkPhysicalDevice gpu;
    vkEnumeratePhysicalDevices(inst, &n, &gpu);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
    float prio = 1;
    VkDeviceQueueCreateInfo q = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio };
    VkPhysicalDeviceVulkan12Features f12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .bufferDeviceAddress = VK_TRUE };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f12, 0, 1, &q, 0, NULL, 0, NULL, NULL };
    CHECK(vkCreateDevice(gpu, &dci, NULL, &dev));
    vkGetDeviceQueue(dev, 0, 0, &queue);
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0 };
    CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));

    /* "guest memory": a reserved region, as the runtime has (64 GiB there; 1 GiB here) */
    uint8_t *region = mmap(NULL, 1u << 30, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    uint8_t *target = region + (256u << 20);

    for (uint32_t t = 0; t < mp.memoryTypeCount; t++) {
        VkMemoryPropertyFlags fl = mp.memoryTypes[t].propertyFlags;
        printf("memory type %u: flags %#x%s%s%s%s\n", t, fl, fl & 1 ? " DEVICE_LOCAL" : "", fl & 2 ? " HOST_VISIBLE" : "",
               fl & 4 ? " HOST_COHERENT" : "", fl & 8 ? " HOST_CACHED" : "");
        if (!(fl & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) continue;

        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, SIZE,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
        VkBuffer b;
        CHECK(vkCreateBuffer(dev, &bci, NULL, &b));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev, b, &req);
        if (!(req.memoryTypeBits >> t & 1)) { printf("  (not usable for buffers)\n"); vkDestroyBuffer(dev, b, NULL); continue; }
        VkMemoryAllocateFlagsInfo af = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, NULL, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0 };
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &af, req.size, t };
        VkDeviceMemory mem;
        CHECK(vkAllocateMemory(dev, &mai, NULL, &mem));
        CHECK(vkBindBufferMemory(dev, b, mem, 0));
        void *p;
        CHECK(vkMapMemory(dev, mem, 0, SIZE, 0, &p));

        /* CPU write speed into this memory, at the driver's own address */
        static uint8_t src[SIZE];
        memset(src, 7, sizeof src);
        double t0 = now();
        memcpy(p, src, SIZE);
        double t1 = now();
        memcpy(src, p, SIZE);
        double t2 = now();
        printf("  CPU write %.0f MB/s, read %.0f MB/s\n", SIZE / (t1 - t0) / 1e6, SIZE / (t2 - t1) / 1e6);

        /* move the mapping to the fixed "guest" address */
        void *moved = mremap(p, SIZE, SIZE, MREMAP_MAYMOVE | MREMAP_FIXED, target);
        if (moved == MAP_FAILED) { perror("  mremap"); continue; }
        printf("  mremap %p -> %p: ok\n", p, moved);

        /* CPU writes at the new address; the GPU fills and copies; the CPU checks at the new address */
        memset(target, 0, 32768);
        for (int i = 8192; i < 12288; i++) target[i] = (uint8_t)(i * 7);
        if (!(fl & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            VkMappedMemoryRange r = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, mem, 0, VK_WHOLE_SIZE };
            vkFlushMappedMemoryRanges(dev, 1, &r);
        }
        gpu_work(b);
        if (!(fl & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            VkMappedMemoryRange r = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, mem, 0, VK_WHOLE_SIZE };
            vkInvalidateMappedMemoryRanges(dev, 1, &r);
        }
        int fill_ok = 1, copy_ok = 1;
        for (int i = 0; i < 4096; i++) if (target[i] != 0xA5) fill_ok = 0;
        for (int i = 0; i < 4096; i++) if (target[16384 + i] != (uint8_t)((8192 + i) * 7)) copy_ok = 0;
        printf("  GPU fill seen by the CPU at the new address: %s\n", fill_ok ? "YES" : "no");
        printf("  CPU data copied by the GPU, seen at the new address: %s\n", copy_ok ? "YES" : "no");

        /* move it back before freeing, so the driver unmaps what it mapped */
        if (mremap(target, SIZE, SIZE, MREMAP_MAYMOVE | MREMAP_FIXED, p) == MAP_FAILED) perror("  mremap back");
        vkUnmapMemory(dev, mem);
        vkFreeMemory(dev, mem, NULL);
        vkDestroyBuffer(dev, b, NULL);
    }
    printf("done\n");
    return 0;
}
