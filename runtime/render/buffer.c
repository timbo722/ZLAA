/*
 * buffer.c - GPU access to the game's memory pools.
 *
 * Vertex, index and uniform data live in the game's memory pools, i.e. in guest memory. With
 * VK_EXT_external_memory_host, Vulkan can use ordinary (host) memory as GPU memory: we
 * "import" each pool once as a VkBuffer covering exactly its guest memory, so draws read the
 * game's data in place, with no copies. A GPU address is then (pool's buffer, offset).
 */
#include "render_internal.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    uint64_t storage, size;
    VkBuffer buffer;
    VkDeviceMemory memory;
} Import;

static Import *imports;
static int nimports, imports_cap;

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
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT };
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

uint64_t render_guest_device_address(uint64_t address) {
    VkBuffer b;
    VkDeviceSize off;
    if (render_guest_buffer(address, &b, &off) != 0) return 0;
    VkBufferDeviceAddressInfo ai = { .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = b };
    return vkGetBufferDeviceAddress(vk.device, &ai) + off;
}

int render_guest_buffer(uint64_t address, VkBuffer *buffer, VkDeviceSize *offset) {
    if (!vk.host_import || !address) return -1;
    uint64_t storage, size;
    if (!nvn_pool_find(address, &storage, &size)) return -1;
    for (int i = 0; i < nimports; i++)
        if (imports[i].storage == storage && imports[i].size == size) {
            *buffer = imports[i].buffer;
            *offset = address - storage;
            return 0;
        }
    if (nimports == imports_cap) {
        imports_cap = imports_cap ? imports_cap * 2 : 64;
        imports = realloc(imports, (size_t)imports_cap * sizeof *imports);
    }
    if (import_pool(storage, size, &imports[nimports]) != 0) return -1;
    *buffer = imports[nimports].buffer;
    *offset = address - storage;
    nimports++;
    return 0;
}
