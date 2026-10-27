/*
 * buffer.c - GPU access to the game's memory pools.
 *
 * Vertex, index and uniform data live in the game's memory pools, i.e. in guest memory. Each
 * pool gets one VkBuffer covering exactly its guest memory, so draws read the game's data in
 * place, with no copies. A GPU address is then (pool's buffer, offset). Two ways to get there:
 *  - host import (VK_EXT_external_memory_host, desktop): Vulkan uses the guest pages as they
 *    are. We "import" each pool when a draw first needs it.
 *  - fd export (VK_KHR_external_memory_fd, the Thor): Vulkan allocates the memory and gives us
 *    a file descriptor for it. When the game creates a pool, we copy the pool's contents into
 *    that memory and map the fd over the pool's guest pages, so from then on the game's CPU
 *    code and the GPU use the same pages. (On the Thor, the GPU keeps CPU caches coherent and
 *    these pages are as fast for the CPU as ordinary memory: see tools/android/memtest.c.)
 */
#include "render_internal.h"
#include "guest_memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define POOL_USAGE (VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | \
                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | \
                    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | \
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)

typedef struct {
    uint64_t storage, size;
    VkBuffer buffer;
    VkDeviceMemory memory;
} Import;

static Import *imports;
static int nimports, imports_cap;
static int last_import = -1;                         /* where render_guest_buffer last found its pool */

static Import *new_import(void) {
    if (nimports == imports_cap) {
        imports_cap = imports_cap ? imports_cap * 2 : 64;
        imports = realloc(imports, (size_t)imports_cap * sizeof *imports);
    }
    return &imports[nimports];
}

static int import_pool(uint64_t storage, uint64_t size, Import *out) {
    void *host = gptr(storage);
    if ((uint64_t)(uintptr_t)host % vk.import_alignment || size % vk.import_alignment) {
        fprintf(stderr, "render: pool %#llx (+%#llx) isn't aligned for import\n", (unsigned long long)storage,
                (unsigned long long)size);
        return -1;
    }
    VkExternalMemoryBufferCreateInfo ext = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT };
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &ext, .size = size,
                              .usage = POOL_USAGE };
    VK_CHECK(vkCreateBuffer(vk.device, &bi, NULL, &out->buffer));
    VkMemoryHostPointerPropertiesEXT hp = { .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
    VK_CHECK(vkGetMemoryHostPointerPropertiesEXT(vk.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                                 host, &hp));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(vk.device, out->buffer, &req);
    VkImportMemoryHostPointerInfoEXT imp = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = host };
    VkMemoryAllocateFlagsInfo flags = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .pNext = &imp,
                                        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT };
    VkMemoryAllocateInfo mi = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &flags, .allocationSize = size,
        .memoryTypeIndex = vk_memory_type(hp.memoryTypeBits & req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) };
    VK_CHECK(vkAllocateMemory(vk.device, &mi, NULL, &out->memory));
    VK_CHECK(vkBindBufferMemory(vk.device, out->buffer, out->memory, 0));
    out->storage = storage;
    out->size = size;
    return 0;
}

/* Does exported memory need its own allocation per buffer ("dedicated")? Asked once. */
static int export_dedicated(void) {
    static int known, dedicated;
    if (!known) {
        VkPhysicalDeviceExternalBufferInfo q = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
            .usage = POOL_USAGE, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
        VkExternalBufferProperties p = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES };
        vkGetPhysicalDeviceExternalBufferProperties(vk.gpu, &q, &p);
        VkExternalMemoryFeatureFlags f = p.externalMemoryProperties.externalMemoryFeatures;
        if (!(f & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT))
            fprintf(stderr, "render: pool memory can't be exported (features %#x)\n", f);
        dedicated = (f & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0;
        known = 1;
    }
    return dedicated;
}

static int export_pool(uint64_t storage, uint64_t size, Import *out) {
    uint64_t page = (uint64_t)sysconf(_SC_PAGESIZE);
    if (storage % page || size % page) {
        fprintf(stderr, "render: pool %#llx (+%#llx) isn't in whole host pages\n", (unsigned long long)storage,
                (unsigned long long)size);
        return -1;
    }
    VkExternalMemoryBufferCreateInfo ext = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &ext, .size = size,
                              .usage = POOL_USAGE };
    VK_CHECK(vkCreateBuffer(vk.device, &bi, NULL, &out->buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(vk.device, out->buffer, &req);
    /* Prefer a type Vulkan itself calls CPU-visible and coherent; on the Thor every exportable
     * type maps through the fd alike (memtest), so any will do otherwise. */
    uint32_t type = (uint32_t)__builtin_ctz(req.memoryTypeBits);
    VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < vk.memory_props.memoryTypeCount; i++)
        if ((req.memoryTypeBits >> i & 1) && (vk.memory_props.memoryTypes[i].propertyFlags & want) == want) { type = i; break; }
    VkMemoryDedicatedAllocateInfo ded = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .buffer = out->buffer };
    VkExportMemoryAllocateInfo ex = { .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .pNext = export_dedicated() ? &ded : NULL, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
    VkMemoryAllocateFlagsInfo flags = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .pNext = &ex,
                                        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT };
    VkMemoryAllocateInfo mi = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &flags,
                                .allocationSize = req.size, .memoryTypeIndex = type };
    VkResult r = vkAllocateMemory(vk.device, &mi, NULL, &out->memory);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "render: pool %#llx (+%#llx): vkAllocateMemory %s\n", (unsigned long long)storage,
                (unsigned long long)size, vk_result_name(r));
        vkDestroyBuffer(vk.device, out->buffer, NULL);
        return -1;
    }
    VK_CHECK(vkBindBufferMemory(vk.device, out->buffer, out->memory, 0));
    VkMemoryGetFdInfoKHR gi = { .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = out->memory,
                                .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
    int fd = -1;
    VK_CHECK(vkGetMemoryFdKHR(vk.device, &gi, &fd));
    /* Map the new memory outside guest memory, copy the pool's contents in, then put those
     * pages where the pool is. The mapping keeps the memory referenced; the fd isn't needed. */
    void *pages = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (pages == MAP_FAILED) {
        perror("render: mmap of exported pool memory");
    } else {
        memcpy(pages, gptr(storage), size);
        if (gmem_place(storage, size, pages) == 0) {
            out->storage = storage;
            out->size = size;
            return 0;
        }
        munmap(pages, size);
    }
    vkFreeMemory(vk.device, out->memory, NULL);
    vkDestroyBuffer(vk.device, out->buffer, NULL);
    return -1;
}

void render_pool_created(uint64_t storage, uint64_t size) {
    if (!vk.fd_export || !storage || !size) return;
    for (int i = 0; i < nimports; i++)
        if (storage < imports[i].storage + imports[i].size && imports[i].storage < storage + size) {
            fprintf(stderr, "render: pool %#llx (+%#llx) overlaps pool %#llx: not shared with the GPU\n",
                    (unsigned long long)storage, (unsigned long long)size, (unsigned long long)imports[i].storage);
            return;
        }
    if (export_pool(storage, size, new_import()) == 0) nimports++;
}

void render_pool_destroyed(uint64_t storage, uint64_t size) {
    if (!vk.fd_export) return;
    for (int i = 0; i < nimports; i++) {
        Import *im = &imports[i];
        if (im->storage != storage || im->size != size) continue;
        render_flush();                  /* nothing recorded or running may still use it */
        /* The game keeps its memory and what's in it: copy that into ordinary pages first */
        void *pages = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (pages == MAP_FAILED) { perror("render: mmap"); return; }
        memcpy(pages, gptr(storage), size);
        if (gmem_place(storage, size, pages) != 0) { munmap(pages, size); return; }
        vkDestroyBuffer(vk.device, im->buffer, NULL);
        vkFreeMemory(vk.device, im->memory, NULL);
        *im = imports[--nimports];
        last_import = -1;
        return;
    }
}

uint64_t render_guest_device_address(uint64_t address) {
    VkBuffer b;
    VkDeviceSize off;
    if (render_guest_buffer(address, &b, &off) != 0) return 0;
    VkBufferDeviceAddressInfo ai = { .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = b };
    return vkGetBufferDeviceAddress(vk.device, &ai) + off;
}

int render_guest_buffer(uint64_t address, VkBuffer *buffer, VkDeviceSize *offset) {
    if (!(vk.host_import || vk.fd_export) || !address) return -1;
    if (last_import >= 0 && last_import < nimports && address - imports[last_import].storage < imports[last_import].size) {
        *buffer = imports[last_import].buffer;     /* (almost always: no pool search, no lock) */
        *offset = address - imports[last_import].storage;
        return 0;
    }
    uint64_t storage, size;
    if (!nvn_pool_find(address, &storage, &size)) return -1;
    for (int i = 0; i < nimports; i++)
        if (imports[i].storage == storage && imports[i].size == size) {
            last_import = i;
            *buffer = imports[i].buffer;
            *offset = address - storage;
            return 0;
        }
    /* exported pools are made when the game creates them (render_pool_created) */
    if (vk.fd_export || import_pool(storage, size, new_import()) != 0) return -1;
    *buffer = imports[nimports].buffer;
    *offset = address - storage;
    nimports++;
    return 0;
}
