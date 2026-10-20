/*
 * image.c - Vulkan images for the game's textures.
 *
 * Each guest texture the GPU touches gets a VkImage, found by its storage address and
 * description. A texture the game filled from the CPU (most of them: they come from its
 * files) is uploaded from guest memory the first time it is used; one the GPU renders into
 * lives only in its VkImage. All images stay in VK_IMAGE_LAYOUT_GENERAL, which every use
 * allows, so we never track layouts (slower on some GPUs; fine for now).
 */
#include "render_internal.h"
#include <stdio.h>
#include <stdlib.h>

/* NVNformat -> VkFormat (0 = not supported yet) */
static const VkFormat vk_formats[] = {
    [0x01] = VK_FORMAT_R8_UNORM, [0x02] = VK_FORMAT_R8_SNORM, [0x03] = VK_FORMAT_R8_UINT, [0x04] = VK_FORMAT_R8_SINT,
    [0x05] = VK_FORMAT_R16_SFLOAT, [0x06] = VK_FORMAT_R16_UNORM, [0x07] = VK_FORMAT_R16_SNORM,
    [0x08] = VK_FORMAT_R16_UINT, [0x09] = VK_FORMAT_R16_SINT,
    [0x0A] = VK_FORMAT_R32_SFLOAT, [0x0B] = VK_FORMAT_R32_UINT, [0x0C] = VK_FORMAT_R32_SINT,
    [0x0D] = VK_FORMAT_R8G8_UNORM, [0x0E] = VK_FORMAT_R8G8_SNORM, [0x0F] = VK_FORMAT_R8G8_UINT, [0x10] = VK_FORMAT_R8G8_SINT,
    [0x11] = VK_FORMAT_R16G16_SFLOAT, [0x12] = VK_FORMAT_R16G16_UNORM, [0x13] = VK_FORMAT_R16G16_SNORM,
    [0x14] = VK_FORMAT_R16G16_UINT, [0x15] = VK_FORMAT_R16G16_SINT,
    [0x16] = VK_FORMAT_R32G32_SFLOAT, [0x17] = VK_FORMAT_R32G32_UINT, [0x18] = VK_FORMAT_R32G32_SINT,
    [0x25] = VK_FORMAT_R8G8B8A8_UNORM, [0x26] = VK_FORMAT_R8G8B8A8_SNORM, [0x27] = VK_FORMAT_R8G8B8A8_UINT,
    [0x28] = VK_FORMAT_R8G8B8A8_SINT,
    [0x29] = VK_FORMAT_R16G16B16A16_SFLOAT, [0x2A] = VK_FORMAT_R16G16B16A16_UNORM, [0x2B] = VK_FORMAT_R16G16B16A16_SNORM,
    [0x2C] = VK_FORMAT_R16G16B16A16_UINT, [0x2D] = VK_FORMAT_R16G16B16A16_SINT,
    [0x2E] = VK_FORMAT_R32G32B32A32_SFLOAT, [0x2F] = VK_FORMAT_R32G32B32A32_UINT, [0x30] = VK_FORMAT_R32G32B32A32_SINT,
    [0x31] = VK_FORMAT_S8_UINT, [0x32] = VK_FORMAT_D16_UNORM, [0x33] = VK_FORMAT_X8_D24_UNORM_PACK32,
    [0x34] = VK_FORMAT_D32_SFLOAT, [0x35] = VK_FORMAT_D24_UNORM_S8_UINT, [0x36] = VK_FORMAT_D32_SFLOAT_S8_UINT,
    [0x37] = VK_FORMAT_R8G8B8A8_SRGB, [0x38] = VK_FORMAT_R8G8B8A8_SRGB,
    [0x3D] = VK_FORMAT_A2B10G10R10_UNORM_PACK32, [0x3E] = VK_FORMAT_A2B10G10R10_UINT_PACK32,
    [0x3F] = VK_FORMAT_B10G11R11_UFLOAT_PACK32, [0x40] = VK_FORMAT_E5B9G9R9_UFLOAT_PACK32,
    [0x41] = VK_FORMAT_BC1_RGB_UNORM_BLOCK, [0x42] = VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
    [0x43] = VK_FORMAT_BC2_UNORM_BLOCK, [0x44] = VK_FORMAT_BC3_UNORM_BLOCK,
    [0x45] = VK_FORMAT_BC1_RGB_SRGB_BLOCK, [0x46] = VK_FORMAT_BC1_RGBA_SRGB_BLOCK,
    [0x47] = VK_FORMAT_BC2_SRGB_BLOCK, [0x48] = VK_FORMAT_BC3_SRGB_BLOCK,
    [0x49] = VK_FORMAT_BC4_UNORM_BLOCK, [0x4A] = VK_FORMAT_BC4_SNORM_BLOCK,
    [0x4B] = VK_FORMAT_BC5_UNORM_BLOCK, [0x4C] = VK_FORMAT_BC5_SNORM_BLOCK,
    [0x4D] = VK_FORMAT_BC7_UNORM_BLOCK, [0x4E] = VK_FORMAT_BC7_SRGB_BLOCK,
    [0x4F] = VK_FORMAT_BC6H_SFLOAT_BLOCK, [0x50] = VK_FORMAT_BC6H_UFLOAT_BLOCK,
    [0x6D] = VK_FORMAT_R8G8B8A8_UNORM, [0x6E] = VK_FORMAT_R8G8B8A8_SNORM, [0x6F] = VK_FORMAT_R8G8B8A8_UINT,
    [0x70] = VK_FORMAT_R8G8B8A8_SINT,
    [0x79] = VK_FORMAT_ASTC_4x4_UNORM_BLOCK, [0x7A] = VK_FORMAT_ASTC_5x4_UNORM_BLOCK, [0x7B] = VK_FORMAT_ASTC_5x5_UNORM_BLOCK,
    [0x7C] = VK_FORMAT_ASTC_6x5_UNORM_BLOCK, [0x7D] = VK_FORMAT_ASTC_6x6_UNORM_BLOCK, [0x7E] = VK_FORMAT_ASTC_8x5_UNORM_BLOCK,
    [0x7F] = VK_FORMAT_ASTC_8x6_UNORM_BLOCK, [0x80] = VK_FORMAT_ASTC_8x8_UNORM_BLOCK, [0x81] = VK_FORMAT_ASTC_10x5_UNORM_BLOCK,
    [0x82] = VK_FORMAT_ASTC_10x6_UNORM_BLOCK, [0x83] = VK_FORMAT_ASTC_10x8_UNORM_BLOCK,
    [0x84] = VK_FORMAT_ASTC_10x10_UNORM_BLOCK, [0x85] = VK_FORMAT_ASTC_12x10_UNORM_BLOCK,
    [0x86] = VK_FORMAT_ASTC_12x12_UNORM_BLOCK,
    [0x87] = VK_FORMAT_ASTC_4x4_SRGB_BLOCK, [0x88] = VK_FORMAT_ASTC_5x4_SRGB_BLOCK, [0x89] = VK_FORMAT_ASTC_5x5_SRGB_BLOCK,
    [0x8A] = VK_FORMAT_ASTC_6x5_SRGB_BLOCK, [0x8B] = VK_FORMAT_ASTC_6x6_SRGB_BLOCK, [0x8C] = VK_FORMAT_ASTC_8x5_SRGB_BLOCK,
    [0x8D] = VK_FORMAT_ASTC_8x6_SRGB_BLOCK, [0x8E] = VK_FORMAT_ASTC_8x8_SRGB_BLOCK, [0x8F] = VK_FORMAT_ASTC_10x5_SRGB_BLOCK,
    [0x90] = VK_FORMAT_ASTC_10x6_SRGB_BLOCK, [0x91] = VK_FORMAT_ASTC_10x8_SRGB_BLOCK,
    [0x92] = VK_FORMAT_ASTC_10x10_SRGB_BLOCK, [0x93] = VK_FORMAT_ASTC_12x10_SRGB_BLOCK,
    [0x94] = VK_FORMAT_ASTC_12x12_SRGB_BLOCK,
    [0x98] = VK_FORMAT_B8G8R8A8_UNORM, [0x99] = VK_FORMAT_B8G8R8A8_UNORM,
    [0x9A] = VK_FORMAT_B8G8R8A8_SRGB, [0x9B] = VK_FORMAT_B8G8R8A8_SRGB,
};

VkFormat render_vk_format(uint32_t nvn_format) {
    return nvn_format < sizeof vk_formats / sizeof *vk_formats ? vk_formats[nvn_format] : VK_FORMAT_UNDEFINED;
}

VkImageAspectFlags render_aspects(VkFormat f) {
    switch (f) {
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_S8_UINT:
        return VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default:
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

/* ---- the cache: a hash table from storage address to images (several may share one) ---- */

#define BUCKETS 4096
static HostImage *buckets[BUCKETS];
static unsigned bucket_of(uint64_t address) { return (unsigned)((address >> 9) * 0x9E3779B1u >> 20) & (BUCKETS - 1); }

/* Two textures are the same image if they describe the same storage the same way */
static int same_image(const TexDesc *a, const TexDesc *b) {
    return a->format == b->format && a->target == b->target && a->width == b->width && a->height == b->height &&
           a->depth == b->depth && a->levels == b->levels && a->samples == b->samples && a->flags == b->flags &&
           a->block_height_log2 == b->block_height_log2;
}

static void create(HostImage *h) {
    const TexDesc *d = &h->desc;
    TexShape s;
    tex_shape(d, &s);
    h->format = render_vk_format(d->format);
    h->aspects = render_aspects(h->format);
    h->width = s.width;
    h->height = s.height;
    h->depth = s.depth;
    h->layers = s.layers;
    h->levels = s.levels;
    int depth_stencil = !(h->aspects & VK_IMAGE_ASPECT_COLOR_BIT);
    int compressed = s.fmt->bw > 1;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!compressed) usage |= depth_stencil ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkImageCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = (h->layers % 6 == 0 && (d->target == TARGET_CUBE || d->target == TARGET_CUBE_ARRAY)
                      ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0) | VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
        .imageType = d->target == TARGET_3D ? VK_IMAGE_TYPE_3D : d->target == TARGET_1D || d->target == TARGET_1D_ARRAY
                     ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D,
        .format = h->format,
        .extent = { h->width, h->height, h->depth },
        .mipLevels = h->levels, .arrayLayers = h->layers,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VK_CHECK(vkCreateImage(vk.device, &ci, NULL, &h->image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk.device, h->image, &req);
    VkMemoryAllocateInfo mi = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
        .memoryTypeIndex = vk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VK_CHECK(vkAllocateMemory(vk.device, &mi, NULL, &h->memory));
    VK_CHECK(vkBindImageMemory(vk.device, h->image, h->memory, 0));
    VkImageViewCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = h->image,
        .viewType = ci.imageType == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D_ARRAY,
        .format = h->format,
        .subresourceRange = { h->aspects, 0, 1, 0, 1 },
    };
    if (ci.imageType == VK_IMAGE_TYPE_1D) vi.viewType = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    if (vi.viewType == VK_IMAGE_VIEW_TYPE_3D) vi.subresourceRange.layerCount = 1;
    /* the attachment view: level 0, layer 0 (a render target view picks others later) */
    vi.viewType = vi.viewType == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D : vi.viewType;
    VK_CHECK(vkCreateImageView(vk.device, &vi, NULL, &h->target_view));
}

HostImage *render_image(const Texture *t) {
    if (!t || !t->address || render_vk_format(t->d.format) == VK_FORMAT_UNDEFINED) return NULL;
    unsigned b = bucket_of(t->address);
    for (HostImage *h = buckets[b]; h; h = h->next)
        if (h->address == t->address && same_image(&h->desc, &t->d)) return h;
    HostImage *h = calloc(1, sizeof *h);
    h->address = t->address;
    h->desc = t->d;
    create(h);
    h->next = buckets[b];
    buckets[b] = h;
    h->needs_upload = 1;
    return h;
}

/* Copy the texture's guest memory into its image (all levels and layers), through the staging
 * buffer: block-linear -> rows on the CPU (layout.c), then vkCmdCopyBufferToImage. */
void render_upload(HostImage *h) {
    h->needs_upload = 0;
    TexShape s;
    tex_shape(&h->desc, &s);
    uint64_t total = 0;
    for (uint32_t lv = 0; lv < s.levels; lv++) {
        LevelShape l = tex_level(&s, lv);
        total += (uint64_t)l.wb * s.fmt->bytes * l.hb * l.depth * s.layers;
    }
    uint64_t offset;
    uint8_t *out = render_staging(total, &offset);
    VkBufferImageCopy copies[16];
    uint32_t ncopies = 0;
    uint64_t layer_size = tex_layer_size(&s), at = 0;
    for (uint32_t lv = 0; lv < s.levels && lv < 16; lv++) {
        LevelShape l = tex_level(&s, lv);
        uint64_t row = (uint64_t)l.wb * s.fmt->bytes, slice = row * l.hb;
        copies[ncopies++] = (VkBufferImageCopy){
            .bufferOffset = offset + at, .bufferRowLength = l.wb * s.fmt->bw, .bufferImageHeight = l.hb * s.fmt->bh,
            .imageSubresource = { h->aspects & VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_COLOR_BIT
                                                                          : VK_IMAGE_ASPECT_DEPTH_BIT, lv, 0, s.layers },
            .imageExtent = { l.width, l.height, l.depth },
        };
        for (uint32_t layer = 0; layer < s.layers; layer++, at += slice * l.depth) {
            if (h->desc.flags & TEXTURE_FLAG_LINEAR) {
                for (uint32_t y = 0; y < l.hb; y++) memcpy(out + at + y * row, gptr(h->address + y * h->desc.stride), row);
            } else {
                uint32_t region[6] = { 0, 0, 0, l.wb, l.hb, l.depth };
                tex_copy_level(gptr(h->address + layer * layer_size + l.offset), out + at, &s, &l, region, row, slice, 1);
            }
        }
    }
    VkCommandBuffer cmd = render_cmd();
    render_image_init_layout(cmd, h);
    vkCmdCopyBufferToImage(cmd, render_staging_buffer(), h->image, VK_IMAGE_LAYOUT_GENERAL, ncopies, copies);
    render_barrier(cmd);
}

HostImage *render_image_find(uint64_t texture) {
    const Texture *t = gptr(texture);
    for (HostImage *h = buckets[bucket_of(t->address)]; h; h = h->next)
        if (h->address == t->address && same_image(&h->desc, &t->d)) return h;
    return NULL;
}

HostImage *render_image_ready(uint64_t texture) {
    HostImage *h = texture ? render_image(gptr(texture)) : NULL;
    if (h && h->needs_upload) render_upload(h);
    return h;
}

/* Images start UNDEFINED; move them to GENERAL once, before their first use. */
void render_image_init_layout(VkCommandBuffer cmd, HostImage *h) {
    if (h->layout_ready) return;
    h->layout_ready = 1;
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = h->image, .subresourceRange = { h->aspects, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS } };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                         0, NULL, 0, NULL, 1, &b);
}
