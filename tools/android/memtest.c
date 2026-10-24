/*
 * memtest.c - can the game's memory and the GPU's memory be the same pages on this device?
 *
 * Desktop drivers let Vulkan use ordinary memory (VK_EXT_external_memory_host). Android's
 * Adreno driver doesn't. This program tries other ways to put GPU-visible memory at a fixed
 * address, where the game's memory pool lives:
 *   1. mremap: let the driver allocate and map memory, then MOVE that mapping to the address.
 *      (Result on the Thor: mremap fails with EINVAL. Kept for the record.)
 *   2. opaque fd: allocate exportable memory, get a file descriptor for it
 *      (VK_KHR_external_memory_fd) and mmap that fd at the address.
 *   3. AHardwareBuffer: allocate a BLOB buffer, take its dma-buf file descriptor (received
 *      through a socket with AHardwareBuffer_sendHandleToUnixSocket, the public NDK way to
 *      get at it) and mmap that at the address; Vulkan imports the same buffer.
 * For each, it measures CPU speed at the address and checks that the CPU and the GPU see
 * each other's writes there. It also tries to move the mapping, since the runtime moves
 * guest memory with mremap (svcMapMemory).
 *
 * Build (NDK) and run on the device:
 *   $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang -O2 memtest.c -lvulkan -lnativewindow -o memtest
 *   adb push memtest /data/local/tmp/ && adb shell chmod +x /data/local/tmp/memtest && adb shell /data/local/tmp/memtest
 * Tests 1 and 2 also build on desktop Linux: gcc -O2 memtest.c -lvulkan -o memtest. `memtest 2` runs
 * only test 2 (on llvmpipe, test 1 moves the driver's own memory away and crashes it).
 */
#define _GNU_SOURCE
#ifdef __ANDROID__
#define VK_USE_PLATFORM_ANDROID_KHR
#include <android/hardware_buffer.h>
#endif
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAILED %s: %d\n", #x, r_); exit(1); } } while (0)
#define SIZE (64u << 20)
#define USAGE (VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | \
               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)

static VkPhysicalDevice gpu;
static VkPhysicalDeviceMemoryProperties mp;
static VkDevice dev;
static VkQueue queue;
static VkCommandPool pool;
static int has_fd_ext;
#ifdef __ANDROID__
static int has_ahb_ext;
#endif
static PFN_vkGetMemoryFdKHR GetMemoryFdKHR;
#ifdef __ANDROID__
static PFN_vkGetAndroidHardwareBufferPropertiesANDROID GetAhbProperties;
#endif
static uint8_t src[SIZE];

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static void print_type(uint32_t t) {
    VkMemoryPropertyFlags fl = mp.memoryTypes[t].propertyFlags;
    printf("memory type %u: flags %#x%s%s%s%s\n", t, fl, fl & 1 ? " DEVICE_LOCAL" : "", fl & 2 ? " HOST_VISIBLE" : "",
           fl & 4 ? " HOST_COHERENT" : "", fl & 8 ? " HOST_CACHED" : "");
}

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

static void speed(uint8_t *p) {
    memset(src, 7, sizeof src);
    double t0 = now();
    memcpy(p, src, SIZE);
    double t1 = now();
    memcpy(src, p, SIZE);
    double t2 = now();
    printf("  CPU write %.0f MB/s, read %.0f MB/s\n", SIZE / (t1 - t0) / 1e6, SIZE / (t2 - t1) / 1e6);
}

/* CPU writes at `cpu`, the GPU fills and copies in `b`, the CPU checks at `cpu`.
 * Only for coherent memory: nothing here flushes or invalidates CPU caches. */
static void roundtrip(uint8_t *cpu, VkBuffer b, const char *where) {
    memset(cpu, 0, 32768);
    for (int i = 8192; i < 12288; i++) cpu[i] = (uint8_t)(i * 7);
    gpu_work(b);
    int fill_ok = 1, copy_ok = 1;
    for (int i = 0; i < 4096; i++) if (cpu[i] != 0xA5) fill_ok = 0;
    for (int i = 0; i < 4096; i++) if (cpu[16384 + i] != (uint8_t)((8192 + i) * 7)) copy_ok = 0;
    printf("  GPU fill seen by the CPU %s: %s\n", where, fill_ok ? "YES" : "no");
    printf("  CPU data copied by the GPU, seen %s: %s\n", where, copy_ok ? "YES" : "no");
}

/* Move the mapping at `target` elsewhere and check it still works there, then move it back. */
static void try_move(uint8_t *target, uint8_t *elsewhere, VkBuffer b) {
    if (mremap(target, SIZE, SIZE, MREMAP_MAYMOVE | MREMAP_FIXED, elsewhere) == MAP_FAILED) { perror("  mremap"); return; }
    printf("  mremap of the mapping: ok\n");
    roundtrip(elsewhere, b, "after the move");
    if (mremap(elsewhere, SIZE, SIZE, MREMAP_MAYMOVE | MREMAP_FIXED, target) == MAP_FAILED) perror("  mremap back");
}

/* Put the reservation's PROT_NONE pages back where a test mapped something. */
static void unmap_fixed(uint8_t *target) {
    mmap(target, SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
}

static VkBuffer make_buffer(VkExternalMemoryHandleTypeFlags handle) {
    VkExternalMemoryBufferCreateInfo ext = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL, handle };
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, handle ? &ext : NULL, 0, SIZE, USAGE,
                               VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    VkBuffer b;
    CHECK(vkCreateBuffer(dev, &bci, NULL, &b));
    return b;
}

/* 1. the driver's own mapping, moved */
static void test_mremap(uint8_t *target) {
    printf("\n== 1. mremap of the driver's mapping\n");
    for (uint32_t t = 0; t < mp.memoryTypeCount; t++) {
        print_type(t);
        VkMemoryPropertyFlags fl = mp.memoryTypes[t].propertyFlags;
        if (!(fl & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) continue;
        VkBuffer b = make_buffer(0);
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
        speed(p);
        void *moved = mremap(p, SIZE, SIZE, MREMAP_MAYMOVE | MREMAP_FIXED, target);
        if (moved == MAP_FAILED) perror("  mremap");
        else {
            printf("  mremap %p -> %p: ok\n", p, moved);
            if (fl & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) roundtrip(target, b, "at the new address");
            /* move it back before freeing, so the driver unmaps what it mapped */
            if (mremap(target, SIZE, SIZE, MREMAP_MAYMOVE | MREMAP_FIXED, p) == MAP_FAILED) perror("  mremap back");
        }
        vkUnmapMemory(dev, mem);
        vkFreeMemory(dev, mem, NULL);
        vkDestroyBuffer(dev, b, NULL);
    }
}

/* 2. exported memory, its fd mapped by us */
static void test_opaque_fd(uint8_t *target, uint8_t *elsewhere) {
    printf("\n== 2. opaque fd export + mmap\n");
    if (!has_fd_ext) { printf("  no VK_KHR_external_memory_fd\n"); return; }
    VkPhysicalDeviceExternalBufferInfo q = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO, NULL, 0, USAGE,
                                             VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
    VkExternalBufferProperties ep = { VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES };
    vkGetPhysicalDeviceExternalBufferProperties(gpu, &q, &ep);
    VkExternalMemoryFeatureFlags ef = ep.externalMemoryProperties.externalMemoryFeatures;
    printf("  features %#x%s%s%s\n", ef, ef & 1 ? " DEDICATED_ONLY" : "", ef & 2 ? " EXPORTABLE" : "", ef & 4 ? " IMPORTABLE" : "");
    if (!(ef & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) return;

    VkBuffer b = make_buffer(VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, b, &req);
    for (uint32_t t = 0; t < mp.memoryTypeCount; t++) {
        if (!(req.memoryTypeBits >> t & 1)) continue;
        print_type(t);
        VkMemoryDedicatedAllocateInfo ded = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, VK_NULL_HANDLE, b };
        VkExportMemoryAllocateInfo ex = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
                                          ef & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT ? &ded : NULL,
                                          VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
        VkMemoryAllocateFlagsInfo af = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, &ex, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0 };
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &af, req.size, t };
        VkDeviceMemory mem;
        VkResult r = vkAllocateMemory(dev, &mai, NULL, &mem);
        if (r != VK_SUCCESS) { printf("  vkAllocateMemory: %d\n", r); continue; }
        CHECK(vkBindBufferMemory(dev, b, mem, 0));
        VkMemoryGetFdInfoKHR gi = { VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, NULL, mem, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
        int fd = -1;
        r = GetMemoryFdKHR(dev, &gi, &fd);
        if (r != VK_SUCCESS) printf("  vkGetMemoryFdKHR: %d\n", r);
        else {
            printf("  fd %d, size by lseek %lld\n", fd, (long long)lseek(fd, 0, SEEK_END));
            if (mmap(target, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED) perror("  mmap(fd)");
            else {
                printf("  mmap(fd) at the fixed address: ok\n");
                speed(target);
                roundtrip(target, b, "at the fixed address");
                try_move(target, elsewhere, b);
                unmap_fixed(target);
            }
            close(fd);
        }
        vkFreeMemory(dev, mem, NULL);
        /* a buffer can be bound only once: make a new one for the next type */
        vkDestroyBuffer(dev, b, NULL);
        b = make_buffer(VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT);
    }
    vkDestroyBuffer(dev, b, NULL);
}

#ifdef __ANDROID__
/* Receive the file descriptors that AHardwareBuffer_sendHandleToUnixSocket sent. */
static int receive_fds(int sock, int *fds, int max) {
    char data[4096];
    char control[CMSG_SPACE(16 * sizeof(int))];
    struct iovec iov = { data, sizeof data };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof control };
    if (recvmsg(sock, &msg, 0) < 0) { perror("  recvmsg"); return 0; }
    int n = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int count = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            for (int i = 0; i < count && n < max; i++) memcpy(&fds[n++], CMSG_DATA(c) + i * sizeof(int), sizeof(int));
        }
    return n;
}

/* 3. AHardwareBuffer: Vulkan imports it, we mmap its dma-buf */
static void test_ahb(uint8_t *target, uint8_t *elsewhere) {
    printf("\n== 3. AHardwareBuffer BLOB + its dma-buf mmap\n");
    if (!has_ahb_ext) { printf("  no VK_ANDROID_external_memory_android_hardware_buffer\n"); return; }
    AHardwareBuffer_Desc d = { .width = SIZE, .height = 1, .layers = 1, .format = AHARDWAREBUFFER_FORMAT_BLOB,
        .usage = AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER | AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
                 AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN };
    AHardwareBuffer *ahb;
    int e = AHardwareBuffer_allocate(&d, &ahb);
    if (e) { printf("  AHardwareBuffer_allocate: %d\n", e); return; }

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) { perror("  socketpair"); return; }
    e = AHardwareBuffer_sendHandleToUnixSocket(ahb, sv[0]);
    if (e) { printf("  sendHandleToUnixSocket: %d\n", e); return; }
    int fds[16];
    int nfds = receive_fds(sv[1], fds, 16);
    close(sv[0]);
    close(sv[1]);
    int dmabuf = -1;
    for (int i = 0; i < nfds; i++) {
        long long sz = (long long)lseek(fds[i], 0, SEEK_END);
        printf("  fd[%d] = %d, size %lld\n", i, fds[i], sz);
        if (dmabuf < 0 && sz >= SIZE) dmabuf = fds[i];
    }
    if (dmabuf < 0) { printf("  no fd as big as the buffer\n"); return; }

    VkAndroidHardwareBufferPropertiesANDROID props = { VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID };
    CHECK(GetAhbProperties(dev, ahb, &props));
    printf("  Vulkan: allocation size %llu, memory types %#x\n", (unsigned long long)props.allocationSize, props.memoryTypeBits);
    VkBuffer b = make_buffer(VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, b, &req);
    uint32_t types = props.memoryTypeBits & req.memoryTypeBits;
    if (!types) { printf("  no memory type fits both the buffer and the AHardwareBuffer\n"); return; }
    uint32_t t = (uint32_t)__builtin_ctz(types);
    print_type(t);
    VkImportAndroidHardwareBufferInfoANDROID imp = { VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, NULL, ahb };
    VkMemoryAllocateFlagsInfo af = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, &imp, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0 };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &af, props.allocationSize, t };
    VkDeviceMemory mem;
    VkResult r = vkAllocateMemory(dev, &mai, NULL, &mem);
    if (r != VK_SUCCESS) { printf("  import: vkAllocateMemory %d\n", r); return; }
    CHECK(vkBindBufferMemory(dev, b, mem, 0));
    printf("  imported into Vulkan: ok\n");

    if (mmap(target, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, dmabuf, 0) == MAP_FAILED) perror("  mmap(dma-buf)");
    else {
        printf("  mmap(dma-buf) at the fixed address: ok\n");
        speed(target);
        roundtrip(target, b, "at the fixed address");
        try_move(target, elsewhere, b);
        unmap_fixed(target);
    }
    for (int i = 0; i < nfds; i++) close(fds[i]);
    vkFreeMemory(dev, mem, NULL);
    vkDestroyBuffer(dev, b, NULL);
    AHardwareBuffer_release(ahb);
}
#endif

static int has_extension(const VkExtensionProperties *ext, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(ext[i].extensionName, name)) return 1;
    return 0;
}

int main(int argc, char **argv) {
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "memtest", 1, NULL, 0, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL };
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t n = 1;
    vkEnumeratePhysicalDevices(inst, &n, &gpu);
    vkGetPhysicalDeviceMemoryProperties(gpu, &mp);

    uint32_t next = 0;
    vkEnumerateDeviceExtensionProperties(gpu, NULL, &next, NULL);
    VkExtensionProperties *ext = calloc(next, sizeof *ext);
    vkEnumerateDeviceExtensionProperties(gpu, NULL, &next, ext);
    const char *enable[3];
    uint32_t nenable = 0;
    if ((has_fd_ext = has_extension(ext, next, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)))
        enable[nenable++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
#ifdef __ANDROID__
    if ((has_ahb_ext = has_extension(ext, next, VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME) &&
                       has_extension(ext, next, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME))) {
        enable[nenable++] = VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME;
        enable[nenable++] = VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME;
    }
#endif

    float prio = 1;
    VkDeviceQueueCreateInfo q = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio };
    VkPhysicalDeviceVulkan12Features f12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .bufferDeviceAddress = VK_TRUE };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f12, 0, 1, &q, 0, NULL, nenable, enable, NULL };
    CHECK(vkCreateDevice(gpu, &dci, NULL, &dev));
    vkGetDeviceQueue(dev, 0, 0, &queue);
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0 };
    CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));
    GetMemoryFdKHR = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(dev, "vkGetMemoryFdKHR");
#ifdef __ANDROID__
    GetAhbProperties = (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)
        vkGetDeviceProcAddr(dev, "vkGetAndroidHardwareBufferPropertiesANDROID");
#endif

    /* "guest memory": a reserved region, as the runtime has (64 GiB there; 1 GiB here) */
    uint8_t *region = mmap(NULL, 1u << 30, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    uint8_t *target = region + (256u << 20), *elsewhere = region + (512u << 20);

    /* memtest [test numbers]: all by default */
    const char *which = argc > 1 ? argv[1] : "123";
    if (strchr(which, '1')) test_mremap(target);
    if (strchr(which, '2')) test_opaque_fd(target, elsewhere);
#ifdef __ANDROID__
    if (strchr(which, '3')) test_ahb(target, elsewhere);
#endif
    printf("done\n");
    return 0;
}
