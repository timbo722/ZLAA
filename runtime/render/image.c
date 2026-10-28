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
#include "astc.h"
#include "png.h"
#include <pthread.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

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

static VkFormat srgb_twin(VkFormat f);           /* (below) */

/* ---- the cache: a hash table from storage address to images (several may share one) ---- */

#define BUCKETS 4096
static HostImage *buckets[BUCKETS];
static unsigned bucket_of(uint64_t address) { return (unsigned)((address >> 9) * 0x9E3779B1u >> 20) & (BUCKETS - 1); }

/* Two textures are the same image if they describe the same storage the same way */
static int same_image(const TexDesc *a, const TexDesc *b) {
    return a->format == b->format && a->target == b->target && a->width == b->width && a->height == b->height &&
           a->depth == b->depth && a->levels == b->levels && a->samples == b->samples &&
           (a->flags & TEXTURE_FLAG_LINEAR) == (b->flags & TEXTURE_FLAG_LINEAR) &&   /* (other flags: usage only) */
           a->block_height_log2 == b->block_height_log2;
}

static VkFormat host_format(VkFormat f, int *decode);

static void create(HostImage *h) {
    const TexDesc *d = &h->desc;
    TexShape s;
    tex_shape(d, &s);
    h->format = host_format(render_vk_format(d->format), &h->astc_decode);
    h->aspects = render_aspects(h->format);
    h->width = s.width;
    h->height = s.height;
    h->depth = s.depth;
    h->layers = s.layers;
    h->levels = s.levels;
    int depth_stencil = !(h->aspects & VK_IMAGE_ASPECT_COLOR_BIT);
    int compressed = s.fmt->bw > 1 && !h->astc_decode;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!compressed) usage |= depth_stencil ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    /* Views in another format only where the format has an sRGB/UNORM twin, and with the list
     * of the two: an image that may be viewed in any format loses the Adreno's framebuffer
     * compression (UBWC), which made clears, copies and passes 2-4x slower on the Thor. */
    h->view_format = srgb_twin(h->format);
    VkFormat formats[2] = { h->format, h->view_format };
    VkImageFormatListCreateInfo list = { .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
                                         .viewFormatCount = 2, .pViewFormats = formats };
    VkImageCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = h->view_format ? &list : NULL,
        /* any square 2D image with whole sets of 6 layers: shaders may sample a 2D array as a cube */
        .flags = (h->layers % 6 == 0 && h->width == h->height && d->target != TARGET_3D && d->target != TARGET_1D &&
                  d->target != TARGET_1D_ARRAY ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0)
                 | (h->view_format ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0),
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

static int is_astc(VkFormat f) { return f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK; }

/* The same texels read as sRGB or as UNORM: the one other format a view of f may have */
static VkFormat srgb_twin(VkFormat f) {
    static const VkFormat pairs[][2] = {
        { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB }, { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB },
        { VK_FORMAT_BC1_RGB_UNORM_BLOCK, VK_FORMAT_BC1_RGB_SRGB_BLOCK }, { VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK },
        { VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC2_SRGB_BLOCK }, { VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC3_SRGB_BLOCK },
        { VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK },
    };
    for (size_t i = 0; i < sizeof pairs / sizeof *pairs; i++) {
        if (f == pairs[i][0]) return pairs[i][1];
        if (f == pairs[i][1]) return pairs[i][0];
    }
    if (is_astc(f)) return (f - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) % 2 ? f - 1 : f + 1;   /* (UNORM, SRGB, UNORM, ...) */
    return VK_FORMAT_UNDEFINED;
}

/* Can this GPU sample (and, for uncompressed formats, render to) images of this format? */
static int format_supported(VkFormat f) {
    static uint8_t known[256];                       /* 0 unknown, 1 yes, 2 no (formats < 256 only) */
    if (f < 256 && known[f]) return known[f] == 1;
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(vk.gpu, f, &p);
    int ok = (p.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
    if (f < 256) known[f] = ok ? 1 : 2;
    if (!ok && is_astc(f)) {
        static int said;
        if (!said++) fprintf(stderr, "render: this GPU can't sample ASTC textures: decoding them on the CPU\n");
    } else if (!ok) {
        fprintf(stderr, "render: this GPU can't use texture format %d\n", (int)f);
    }
    return ok;
}

/* The format of our image for a texture of format f: f itself, or RGBA8 for ASTC we decode. */
static VkFormat host_format(VkFormat f, int *decode) {
    *decode = is_astc(f) && !format_supported(f);
    if (!*decode) return f;
    return (f - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) & 1 ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
}

HostImage *render_image(const Texture *t) {
    if (!t || !t->address || render_vk_format(t->d.format) == VK_FORMAT_UNDEFINED) return NULL;
    VkFormat f = render_vk_format(t->d.format);
    if (!format_supported(f) && !is_astc(f)) return NULL;
    unsigned b = bucket_of(t->address);
    for (HostImage *h = buckets[b]; h; h = h->next)
        if (h->address == t->address && same_image(&h->desc, &t->d)) return h;
    if (render_tracing() || getenv("LAA_TRACE_ALIASES"))
        for (HostImage *o = buckets[b]; o; o = o->next)
            if (o->address == t->address)
                fprintf(stderr, "render: second image at %#llx: fmt %#x/%#x target %u/%u %ux%ux%u/%ux%ux%u levels %u/%u "
                        "flags %#x/%#x bh %d/%d (old/new)\n", (unsigned long long)t->address, o->desc.format, t->d.format,
                        o->desc.target, t->d.target, o->desc.width, o->desc.height, o->desc.depth, t->d.width, t->d.height,
                        t->d.depth, o->desc.levels, t->d.levels, o->desc.flags, t->d.flags,
                        (int)o->desc.block_height_log2, (int)t->d.block_height_log2);
    HostImage *h = calloc(1, sizeof *h);
    h->address = t->address;
    h->desc = t->d;
    create(h);
    h->next = buckets[b];
    buckets[b] = h;
    if (t->d.flags & TEXTURE_FLAG_COMPRESSIBLE) {   /* (its guest memory means nothing: start it at 0 */
        static const VkClearValue zero;              /* rather than copying megabytes of it, cheap */
        render_image_clear(h, &zero, h->aspects);    /* as a pass's loadOp) */
    } else {
        h->needs_upload = 1;
    }
    return h;
}

/* ---- CPU writes ----
 * On the Switch, CPU and GPU share memory, so a game may change a texture by just writing
 * its memory (the font cache does: glyphs are drawn into it as text needs them). Our image is
 * a copy, so we notice changes by hashing the guest memory before an image is used:
 *   - an image that changed recently ("hot") is checked every frame;
 *   - others at most every CHECK_EVERY frames, within a per-frame byte budget;
 *   - an image the GPU writes (a render target) isn't checked while the GPU keeps writing it.
 *     After GPU_QUIET frames without, its guest memory is hashed again as the new baseline:
 *     some images are cleared by the GPU once, then filled by the CPU (glyph caches). */
#define CHECK_EVERY 30
#define CHECK_BUDGET (6u << 20)                      /* bytes per frame, for images that aren't hot (spread out) */
#define HOT_FRAMES 600
#define GPU_QUIET 60
static uint64_t frame_no = 1, budget = CHECK_BUDGET;

void render_images_new_frame(void) { frame_no++; budget = CHECK_BUDGET; }

static uint64_t storage_bytes(const HostImage *h) {
    return h->desc.flags & TEXTURE_FLAG_LINEAR ? (uint64_t)h->desc.stride * h->height : texture_storage_size(&h->desc);
}

/* 4 independent lanes: the CPU works on all four at once (one chain was ~4 GB/s on the Thor, and
 * a frame that rechecked 24 MB of textures missed its vsync) */
static uint64_t guest_hash(const HostImage *h) {
    uint64_t n = storage_bytes(h) / 8, v[4] = { 0x9E3779B97F4A7C15ull, 1, 2, 3 };
    const uint8_t *p = gptr(h->address);
    uint64_t i = 0;
    for (; i + 4 <= n; i += 4) {
        uint64_t w[4];
        memcpy(w, p + i * 8, 32);
        for (int k = 0; k < 4; k++) {
            v[k] = (v[k] ^ w[k]) * 0x100000001B3ull;
            v[k] ^= v[k] >> 29;
        }
    }
    for (; i < n; i++) {
        uint64_t w;
        memcpy(&w, p + i * 8, 8);
        v[0] = (v[0] ^ w) * 0x100000001B3ull;
        v[0] ^= v[0] >> 29;
    }
    return v[0] ^ (v[1] * 3) ^ (v[2] * 5) ^ (v[3] * 7);
}

void render_image_gpu_wrote(HostImage *h) {
    h->gpu_written = 1;
    h->preloaded = 0;
    h->needs_upload = 0;
    h->gpu_frame = frame_no;
    h->hash_valid = 0;
}

static void check_cpu_writes(HostImage *h) {
    if (h->needs_upload || h->checked == frame_no) return;
    if (h->preloaded) {                              /* (first use: did the game write it after making it?) */
        h->preloaded = 0;
        h->checked = frame_no;
        if (guest_hash(h) != h->hash) h->needs_upload = 1;
        return;
    }
    if (h->gpu_written && frame_no - h->gpu_frame < GPU_QUIET) return;
    int hot = h->hot_until > frame_no;
    uint64_t size = storage_bytes(h);
    if (!hot || !h->hash_valid) {
        if (frame_no - h->checked < CHECK_EVERY || size > budget) return;
        budget -= size;
    }
    h->checked = frame_no;
    uint64_t now = guest_hash(h);
    if (!h->hash_valid) {                            /* (after GPU writes: just the new baseline) */
        h->hash = now;
        h->hash_valid = 1;
    } else if (now != h->hash) {
        h->needs_upload = 1;
        h->hot_until = frame_no + HOT_FRAMES;
    }
}

/* Debugging: save a colour image as a PNG (blitted to RGBA8 first, so HDR values clamp to
 * 0..1). Finishes the GPU work recorded so far. */
/* unsigned small floats of B10G11R11: 5-bit exponent, `mbits` mantissa bits */
static float small_float(uint32_t v, int mbits) {
    uint32_t e = v >> mbits & 31, m = v & ((1u << mbits) - 1);
    if (e == 0) return ldexpf((float)m, -14 - mbits);
    if (e == 31) return 65504.0f;
    return ldexpf(1.0f + (float)m / (float)(1u << mbits), (int)e - 15);
}

/* HDR images (B10G11R11): read the raw texels, print the range of values, save x / (1 + x) */
static int debug_dump_hdr(HostImage *h, const char *path) {
    uint64_t n = (uint64_t)h->width * h->height, offset;
    uint32_t *raw = (uint32_t *)render_staging(n * 4, &offset);
    render_end_rendering();
    VkCommandBuffer cmd = render_cmd();
    VkBufferImageCopy copy = { .bufferOffset = offset, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                               .imageExtent = { h->width, h->height, 1 } };
    vkCmdCopyImageToBuffer(cmd, h->image, VK_IMAGE_LAYOUT_GENERAL, render_staging_buffer(), 1, &copy);
    render_flush();
    uint8_t *out = malloc(n * 4);
    if (!out) return -1;
    double sum = 0;
    float lo = 1e30f, hi = 0;
    uint64_t special = 0, first = 0;                 /* inf/NaN texels (exponent 31 in any channel) */
    for (uint64_t i = 0; i < n; i++) {
        if ((raw[i] >> 6 & 31) == 31 || (raw[i] >> 17 & 31) == 31 || (raw[i] >> 27 & 31) == 31)
            if (!special++) first = i;
        float c[3] = { small_float(raw[i] & 0x7FF, 6), small_float(raw[i] >> 11 & 0x7FF, 6), small_float(raw[i] >> 22, 5) };
        float l = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
        sum += l;
        if (l < lo) lo = l;
        if (l > hi) hi = l;
        for (int k = 0; k < 3; k++) out[i * 4 + k] = (uint8_t)(255.0f * c[k] / (1.0f + c[k]) + 0.5f);
        out[i * 4 + 3] = 255;
    }
    fprintf(stderr, "render: %s: luminance min %g average %g max %g\n", path, lo, sum / (double)n, hi);
    if (special)
        fprintf(stderr, "render: %s: %llu inf/NaN texels, the first at (%llu, %llu)\n", path, (unsigned long long)special,
                (unsigned long long)(first % h->width), (unsigned long long)(first / h->width));
    png_write(path, out + (n - h->width) * 4, h->width, h->height, -(int32_t)h->width * 4);
    free(out);
    return 0;
}

/* Depth images: the depth aspect's values, printed as a range and saved as grey from the
 * nearest (black) to the farthest (white) value in the image */
static void debug_dump_depth(HostImage *h, const char *path) {
    int bytes = h->format == VK_FORMAT_D16_UNORM ? 2 : 4;
    int is_float = h->format == VK_FORMAT_D32_SFLOAT || h->format == VK_FORMAT_D32_SFLOAT_S8_UINT;
    uint64_t n = (uint64_t)h->width * h->height, offset;
    uint8_t *raw = render_staging(n * bytes, &offset);
    render_end_rendering();
    VkCommandBuffer cmd = render_cmd();
    render_image_init_layout(cmd, h);
    VkBufferImageCopy copy = { .bufferOffset = offset, .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 },
                               .imageExtent = { h->width, h->height, 1 } };
    vkCmdCopyImageToBuffer(cmd, h->image, VK_IMAGE_LAYOUT_GENERAL, render_staging_buffer(), 1, &copy);
    render_flush();
    float *d = malloc(n * sizeof *d);
    uint8_t *out = malloc(n * 4);
    if (!d || !out) { free(d); free(out); return; }
    float lo = 1e30f, hi = -1e30f;
    double sum = 0;
    for (uint64_t i = 0; i < n; i++) {
        uint32_t v = 0;
        memcpy(&v, raw + i * bytes, bytes);
        if (is_float) memcpy(&d[i], &v, 4);
        else d[i] = bytes == 2 ? v / 65535.0f : (v & 0xFFFFFF) / 16777215.0f;
        if (d[i] < lo) lo = d[i];
        if (d[i] > hi) hi = d[i];
        sum += d[i];
    }
    for (uint64_t i = 0; i < n; i++) {
        uint8_t g = (uint8_t)(hi > lo ? 255.0f * (d[i] - lo) / (hi - lo) : 0);
        out[i * 4] = out[i * 4 + 1] = out[i * 4 + 2] = g;
        out[i * 4 + 3] = 255;
    }
    fprintf(stderr, "render: %s: depth min %.6f average %.6f max %.6f\n", path, lo, sum / (double)n, hi);
    png_write(path, out + (n - h->width) * 4, h->width, h->height, -(int32_t)h->width * 4);
    free(d);
    free(out);
}

void render_debug_dump(HostImage *h, const char *path) {
    render_image_flush_clear(h);
    if (h->aspects & VK_IMAGE_ASPECT_DEPTH_BIT) { debug_dump_depth(h, path); return; }
    if (!(h->aspects & VK_IMAGE_ASPECT_COLOR_BIT)) return;
    if (h->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32) { debug_dump_hdr(h, path); return; }
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { h->width, h->height, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage tmp;
    VkDeviceMemory mem;
    VK_CHECK(vkCreateImage(vk.device, &ci, NULL, &tmp));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk.device, tmp, &req);
    VkMemoryAllocateInfo mi = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
        .memoryTypeIndex = vk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VK_CHECK(vkAllocateMemory(vk.device, &mi, NULL, &mem));
    VK_CHECK(vkBindImageMemory(vk.device, tmp, mem, 0));
    render_end_rendering();
    VkCommandBuffer cmd = render_cmd();
    render_image_init_layout(cmd, h);
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .image = tmp,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkImageBlit blit = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffsets = { { 0, 0, 0 }, { (int32_t)h->width, (int32_t)h->height, 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets = { { 0, 0, 0 }, { (int32_t)h->width, (int32_t)h->height, 1 } } };
    vkCmdBlitImage(cmd, h->image, VK_IMAGE_LAYOUT_GENERAL, tmp, VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_NEAREST);
    render_barrier(cmd);
    uint64_t size = (uint64_t)h->width * h->height * 4, offset;
    uint8_t *staging = render_staging(size, &offset);
    VkBufferImageCopy copy = { .bufferOffset = offset, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                               .imageExtent = { h->width, h->height, 1 } };
    vkCmdCopyImageToBuffer(cmd, tmp, VK_IMAGE_LAYOUT_GENERAL, render_staging_buffer(), 1, &copy);
    render_flush();
    for (uint64_t i = 3; i < size; i += 4) staging[i] = 255;    /* (opaque, to see the colours) */
    /* NVN's images are upside down (OpenGL convention): rows bottom-up */
    if (png_write(path, staging + size - (uint64_t)h->width * 4, h->width, h->height, -(int32_t)h->width * 4) == 0)
        fprintf(stderr, "render: wrote %s (%ux%u, format %d)\n", path, h->width, h->height, h->format);
    vkDestroyImage(vk.device, tmp, NULL);
    vkFreeMemory(vk.device, mem, NULL);
}

int render_tracing(void) {
    static int on = -1;
    static uint64_t from, frames;
    if (on < 0) {
        on = getenv("LAA_TRACE_PIPELINES") != NULL;
        from = getenv("LAA_TRACE_FROM") ? strtoull(getenv("LAA_TRACE_FROM"), NULL, 0) : 0;
        frames = getenv("LAA_TRACE_FRAMES") ? strtoull(getenv("LAA_TRACE_FRAMES"), NULL, 0) : ~0ull;
    }
    uint64_t f = nvn_frame_count();
    return on && f >= from && f - from < frames;
}

/* Copy the texture's guest memory into its image (all levels and layers), through the staging
 * buffer: block-linear -> rows on the CPU (layout.c), then vkCmdCopyBufferToImage. */
static int uploading_ahead;                          /* (render_preload: for the trace) */

void render_upload(HostImage *h) {
    uint64_t again = h->checked;                     /* (for the trace: uploaded before) */
    h->pending_clear = 0;                            /* (the whole image is replaced: newer than a clear) */
    h->needs_upload = 0;
    h->gpu_written = 0;                              /* (now the same as guest memory) */
    h->hash_valid = 1;
    h->hash = guest_hash(h);
    h->checked = frame_no;
    TexShape s;
    tex_shape(&h->desc, &s);
    uint64_t total = 0, most = 0;                   /* (most: the largest level, for ASTC decoding) */
    for (uint32_t lv = 0; lv < s.levels; lv++) {
        LevelShape l = tex_level(&s, lv);
        uint64_t blocks = (uint64_t)l.wb * s.fmt->bytes * l.hb * l.depth;
        total += (h->astc_decode ? (uint64_t)l.width * l.height * l.depth * 4 : blocks) * s.layers;
        if (blocks > most) most = blocks;
    }
    uint8_t *blocks = h->astc_decode ? malloc(most) : NULL;
    uint64_t offset;
    if (render_tracing())
        fprintf(stderr, "render: upload %#llx %ux%ux%u fmt %#x levels %u layers %u flags %#x: %llu bytes%s\n", (unsigned long long)h->address,
                s.width, s.height, s.depth, h->desc.format, s.levels, s.layers, h->desc.flags, (unsigned long long)total,
                uploading_ahead ? " (ahead)" : again ? " (again)" : "");
    uint8_t *out = render_staging(total, &offset);
    VkBufferImageCopy copies[16];
    uint32_t ncopies = 0;
    uint64_t layer_size = tex_layer_size(&s), at = 0;
    for (uint32_t lv = 0; lv < s.levels && lv < 16; lv++) {
        LevelShape l = tex_level(&s, lv);
        uint64_t row = (uint64_t)l.wb * s.fmt->bytes, slice = row * l.hb;
        uint64_t out_slice = h->astc_decode ? (uint64_t)l.width * l.height * 4 : slice;
        copies[ncopies++] = (VkBufferImageCopy){
            .bufferOffset = offset + at,
            .bufferRowLength = h->astc_decode ? l.width : l.wb * s.fmt->bw,
            .bufferImageHeight = h->astc_decode ? l.height : l.hb * s.fmt->bh,
            .imageSubresource = { h->aspects & VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_COLOR_BIT
                                                                          : VK_IMAGE_ASPECT_DEPTH_BIT, lv, 0, s.layers },
            .imageExtent = { l.width, l.height, l.depth },
        };
        for (uint32_t layer = 0; layer < s.layers; layer++, at += out_slice * l.depth) {
            if (h->astc_decode) {                    /* block-linear -> rows of blocks -> RGBA */
                uint32_t region[6] = { 0, 0, 0, l.wb, l.hb, l.depth };
                tex_copy_level(gptr(h->address + layer * layer_size + l.offset), blocks, &s, &l, region, row, slice, 1);
                for (uint32_t z = 0; z < l.depth; z++)
                    astc_decode(blocks + z * slice, row, l.width, l.height, s.fmt->bw, s.fmt->bh,
                                h->format == VK_FORMAT_R8G8B8A8_SRGB, out + at + z * out_slice, (size_t)l.width * 4);
            } else if (h->desc.flags & TEXTURE_FLAG_LINEAR) {
                for (uint32_t y = 0; y < l.hb; y++) memcpy(out + at + y * row, gptr(h->address + y * h->desc.stride), row);
            } else {
                uint32_t region[6] = { 0, 0, 0, l.wb, l.hb, l.depth };
                tex_copy_level(gptr(h->address + layer * layer_size + l.offset), out + at, &s, &l, region, row, slice, 1);
            }
        }
    }
    free(blocks);
    render_end_rendering();                          /* (copies can't happen while rendering) */
    VkCommandBuffer cmd = render_cmd();
    render_image_init_layout(cmd, h);
    vkCmdCopyBufferToImage(cmd, render_staging_buffer(), h->image, VK_IMAGE_LAYOUT_GENERAL, ncopies, copies);
    render_barrier(cmd);
}

void render_write_texture(uint64_t texture, const uint8_t *rgba, uint32_t width, uint32_t height) {
    HostImage *h = render_image(gptr(texture));
    if (!h || h->width != width || h->height != height) return;
    h->pending_clear = 0;                            /* (the whole image is replaced) */
    uint64_t size = (uint64_t)width * height * 4, offset;
    uint8_t *dst = render_staging(size, &offset);
    memcpy(dst, rgba, size);
    render_end_rendering();
    VkCommandBuffer cmd = render_cmd();
    render_image_init_layout(cmd, h);
    VkBufferImageCopy copy = { .bufferOffset = offset, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                               .imageExtent = { width, height, 1 } };
    vkCmdCopyBufferToImage(cmd, render_staging_buffer(), h->image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    render_barrier(cmd);
    render_image_gpu_wrote(h);
}

/* The CPU wrote texels (nvnTextureWriteTexels) into the texture's guest memory: copy that
 * region (in blocks: x, y, z, width, height, depth) into the images using this storage. */
void render_texels_written(uint64_t texture, uint32_t level, uint32_t layer, const uint32_t region[6]) {
    const Texture *t = gptr(texture);
    for (HostImage *h = buckets[bucket_of(t->address)]; h; h = h->next) {
        if (h->address != t->address || !same_image(&h->desc, &t->d)) continue;
        if (h->needs_upload) continue;               /* not uploaded yet: the full upload will see it */
        if (h->astc_decode) { h->needs_upload = 1; continue; }   /* (decoded: simply upload it all again) */
        TexShape s;
        tex_shape(&h->desc, &s);
        if (level >= s.levels || layer >= s.layers || (h->desc.flags & TEXTURE_FLAG_LINEAR)) continue;
        LevelShape l = tex_level(&s, level);
        if (region[0] >= l.wb || region[1] >= l.hb || region[2] >= l.depth) continue;
        render_image_flush_clear(h);                 /* (a clear before these texels comes first) */
        uint32_t r[6] = { region[0], region[1], region[2], region[3], region[4], region[5] };
        if (r[3] > l.wb - r[0]) r[3] = l.wb - r[0];
        if (r[4] > l.hb - r[1]) r[4] = l.hb - r[1];
        if (r[5] > l.depth - r[2]) r[5] = l.depth - r[2];
        uint64_t row = (uint64_t)r[3] * s.fmt->bytes, slice = row * r[4], offset;
        uint8_t *out = render_staging(slice * r[5], &offset);
        tex_copy_level(gptr(h->address + layer * tex_layer_size(&s) + l.offset), out, &s, &l, r, row, slice, 1);
        uint32_t x = r[0] * s.fmt->bw, y = r[1] * s.fmt->bh;
        VkBufferImageCopy copy = {
            .bufferOffset = offset, .bufferRowLength = r[3] * s.fmt->bw, .bufferImageHeight = r[4] * s.fmt->bh,
            .imageSubresource = { h->aspects & VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_COLOR_BIT
                                                                         : VK_IMAGE_ASPECT_DEPTH_BIT, level, layer, 1 },
            .imageOffset = { (int32_t)x, (int32_t)y, (int32_t)r[2] },
            .imageExtent = { r[3] * s.fmt->bw < l.width - x ? r[3] * s.fmt->bw : l.width - x,
                             r[4] * s.fmt->bh < l.height - y ? r[4] * s.fmt->bh : l.height - y, r[5] },
        };
        render_end_rendering();
        VkCommandBuffer cmd = render_cmd();
        render_image_init_layout(cmd, h);
        vkCmdCopyBufferToImage(cmd, render_staging_buffer(), h->image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        render_barrier(cmd);
    }
}

/* Debugging (LAA_REUPLOAD=1, once per frame): upload every image the GPU hasn't drawn into
 * again from guest memory, to tell missed CPU writes from other bugs. */
void render_reupload_all(void) {
    for (unsigned b = 0; b < BUCKETS; b++)
        for (HostImage *h = buckets[b]; h; h = h->next)
            if (!h->gpu_written) h->needs_upload = 1;
}

HostImage *render_image_find(uint64_t texture) {
    const Texture *t = gptr(texture);
    for (HostImage *h = buckets[bucket_of(t->address)]; h; h = h->next)
        if (h->address == t->address && same_image(&h->desc, &t->d)) return h;
    return NULL;
}

/* ---- preloading ----
 * A game usually fills a texture's memory before making it (the file is read into a memory
 * pool, then its textures are made there), and draws with it later. Uploading at the first draw
 * put all of a new scene's textures into its first frame (~50 MB, 100 ms on the Thor) while the
 * game's fade-in had already started. So made textures are queued and uploaded at presents
 * (during the loading screen), a few ms each; their first use checks the memory didn't change. */
#define PRELOAD_MAX 4096
static Texture preload_queue[PRELOAD_MAX];
static unsigned preload_head, preload_tail;
static pthread_mutex_t preload_lock = PTHREAD_MUTEX_INITIALIZER;
static HostImage *verify_queue[PRELOAD_MAX];        /* uploaded ahead: checked again at later presents */
static unsigned verify_head, verify_tail;            /* (main thread only) */

void render_texture_made(const Texture *t) {      /* (render targets too: their image is made ahead) */
    pthread_mutex_lock(&preload_lock);
    if (preload_head - preload_tail < PRELOAD_MAX) preload_queue[preload_head++ % PRELOAD_MAX] = *t;
    pthread_mutex_unlock(&preload_lock);
}

void render_preload(double ms) {
    static int off = -1;                             /* LAA_NO_PRELOAD=1: upload at first use only */
    if (off < 0) off = getenv("LAA_NO_PRELOAD") != NULL;
    if (off) return;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        Texture t;
        pthread_mutex_lock(&preload_lock);
        int any = preload_head != preload_tail;
        if (any) t = preload_queue[preload_tail++ % PRELOAD_MAX];
        pthread_mutex_unlock(&preload_lock);
        if (!any) break;
        HostImage *h = render_image(&t);
        if (h && h->needs_upload) {
            uploading_ahead = 1;
            render_upload(h);
            uploading_ahead = 0;
            h->preloaded = 1;
            if (verify_head - verify_tail < PRELOAD_MAX) verify_queue[verify_head++ % PRELOAD_MAX] = h;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6 > ms) return;
    }
    /* then the hash check the first use would do, a frame or more after the upload (the loading
     * screen again): did the game write the memory after making the texture? */
    while (verify_head != verify_tail) {
        HostImage *h = verify_queue[verify_tail % PRELOAD_MAX];
        if (h->preloaded && h->checked == frame_no) return;   /* (uploaded this frame: next time) */
        verify_tail++;
        if (h->preloaded) {
            h->preloaded = 0;
            h->checked = frame_no;
            if (guest_hash(h) != h->hash) h->needs_upload = 1;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6 > ms) return;
    }
}

HostImage *render_image_target(uint64_t texture) {
    HostImage *h = texture ? render_image(gptr(texture)) : NULL;
    if (h) check_cpu_writes(h);
    if (h && h->needs_upload) render_upload(h);
    return h;
}

HostImage *render_image_ready(uint64_t texture) {
    HostImage *h = render_image_target(texture);
    if (h) render_image_flush_clear(h);
    return h;
}

/* Clears wait for the image's next use. If that is a render pass covering all of it, the pass
 * starts with loadOp CLEAR, which costs almost nothing on a tiler (the Adreno): a separate clear
 * writes the whole image to memory and the pass then reads it all back in. Any other use clears
 * it first (render_image_ready). Images with one level and layer only, like render targets. */
void render_image_clear(HostImage *h, const VkClearValue *value, VkImageAspectFlags aspects) {
    static int now = -1;                             /* LAA_NO_DEFERRED_CLEAR=1: clear right away */
    if (now < 0) now = getenv("LAA_NO_DEFERRED_CLEAR") != NULL;
    if (h->levels == 1 && h->layers == 1 && h->depth == 1 && !now) {
        if (aspects & VK_IMAGE_ASPECT_COLOR_BIT) h->clear_value.color = value->color;
        if (aspects & VK_IMAGE_ASPECT_DEPTH_BIT) h->clear_value.depthStencil.depth = value->depthStencil.depth;
        if (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) h->clear_value.depthStencil.stencil = value->depthStencil.stencil;
        h->pending_clear |= aspects;
        render_image_gpu_wrote(h);
        return;
    }
    render_image_flush_clear(h);                     /* (an older one first) */
    h->clear_value = *value;
    h->pending_clear = aspects;
    render_image_flush_clear(h);                     /* (every level and layer: now) */
}

void render_image_flush_clear(HostImage *h) {
    if (!h->pending_clear) return;
    render_end_rendering();
    VkCommandBuffer cmd = render_cmd();
    render_image_init_layout(cmd, h);
    VkImageSubresourceRange range = { h->pending_clear, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS };
    if (h->pending_clear & VK_IMAGE_ASPECT_COLOR_BIT)
        vkCmdClearColorImage(cmd, h->image, VK_IMAGE_LAYOUT_GENERAL, &h->clear_value.color, 1, &range);
    else
        vkCmdClearDepthStencilImage(cmd, h->image, VK_IMAGE_LAYOUT_GENERAL, &h->clear_value.depthStencil, 1, &range);
    h->pending_clear = 0;
    render_barrier(cmd);
    render_image_gpu_wrote(h);
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

/* ---- views and samplers for shaders ---- */

typedef struct ImageView {
    struct ImageView *next;
    TextureView view;
    int dim, array;
    VkImageView handle;
} ImageView;

/* NVN swizzle (ZERO, ONE, R, G, B, A) -> Vulkan */
static VkComponentSwizzle swizzle(uint8_t s) {
    static const VkComponentSwizzle m[6] = { VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_R,
                                             VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A };
    return s < 6 ? m[s] : VK_COMPONENT_SWIZZLE_IDENTITY;
}

VkImageView render_image_view(HostImage *h, const TextureView *view, int dim, int array) {
    for (ImageView *v = h->views; v; v = v->next)
        if (v->dim == dim && v->array == array && memcmp(&v->view, view, sizeof *view) == 0) return v->handle;
    int is3d = h->desc.target == TARGET_3D;
    if ((dim == TEXDIM_3D) != is3d) return VK_NULL_HANDLE;             /* the shader wants another kind */
    if (dim == TEXDIM_CUBE && (h->layers < 6 || h->width != h->height)) return VK_NULL_HANDLE;
    uint32_t base_level = view->set & VIEW_LEVELS ? view->base_level : 0;
    uint32_t levels = view->set & VIEW_LEVELS && view->num_levels ? view->num_levels : h->levels - base_level;
    uint32_t base_layer = view->set & VIEW_LAYERS ? view->min_layer : 0;
    uint32_t layers = view->set & VIEW_LAYERS && view->num_layers ? view->num_layers : h->layers - base_layer;
    if (base_level >= h->levels || base_layer >= h->layers) return VK_NULL_HANDLE;
    if (base_level + levels > h->levels) levels = h->levels - base_level;
    if (base_layer + layers > h->layers) layers = h->layers - base_layer;
    VkImageViewType type;
    if (dim == TEXDIM_3D) { type = VK_IMAGE_VIEW_TYPE_3D; layers = 1; }
    else if (dim == TEXDIM_CUBE) { type = array ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE; layers = array ? layers / 6 * 6 : 6; }
    else if (array) type = h->desc.target == TARGET_1D || h->desc.target == TARGET_1D_ARRAY ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    else { type = h->desc.target == TARGET_1D || h->desc.target == TARGET_1D_ARRAY ? VK_IMAGE_VIEW_TYPE_1D : VK_IMAGE_VIEW_TYPE_2D; layers = 1; }
    VkFormat format = h->format;
    if (view->set & VIEW_FORMAT) {
        int decode;
        VkFormat f = render_vk_format(view->format);
        if (f != VK_FORMAT_UNDEFINED) format = h->astc_decode ? host_format(f, &decode) : f;
    }
    if (format != h->format && format != h->view_format) {   /* (not seen in this game so far) */
        static int warned;
        if (!warned++) fprintf(stderr, "render: a view in format %d of an image in format %d: using the image's own format\n",
                               format, h->format);
        format = h->format;
    }
    const uint8_t *sw = view->set & VIEW_SWIZZLE ? view->swizzle : h->desc.swizzle;
    VkImageAspectFlags aspect = h->aspects;
    if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) aspect = VK_IMAGE_ASPECT_DEPTH_BIT;    /* sample depth, not stencil */
    VkImageViewCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = h->image, .viewType = type,
        .format = format,
        /* the identity swizzle as IDENTITY: some drivers (llvmpipe) crash on explicit R/G/B/A
         * swizzles that name channels a format doesn't have */
        .components = { sw[0] == 2 ? VK_COMPONENT_SWIZZLE_IDENTITY : swizzle(sw[0]), sw[1] == 3 ? VK_COMPONENT_SWIZZLE_IDENTITY : swizzle(sw[1]),
                        sw[2] == 4 ? VK_COMPONENT_SWIZZLE_IDENTITY : swizzle(sw[2]), sw[3] == 5 ? VK_COMPONENT_SWIZZLE_IDENTITY : swizzle(sw[3]) },
        .subresourceRange = { aspect, base_level, levels, base_layer, layers } };
    ImageView *v = calloc(1, sizeof *v);
    if (vkCreateImageView(vk.device, &ci, NULL, &v->handle) != VK_SUCCESS) { free(v); return VK_NULL_HANDLE; }
    v->view = *view;
    v->dim = dim;
    v->array = array;
    v->next = h->views;
    h->views = v;
    return v->handle;
}

VkImageView render_dummy_view(int dim, int array) {
    static VkImage image2d, image3d;
    static VkImageView views[4][2];
    int is3d = dim == TEXDIM_3D, d = dim > 3 ? 1 : dim;
    if (views[d][array]) return views[d][array];
    VkImage *image = is3d ? &image3d : &image2d;
    if (!*image) {
        VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .flags = is3d ? 0 : VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
            .imageType = is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = { 1, 1, 1 }, .mipLevels = 1, .arrayLayers = is3d ? 1 : 6, .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT };
        VK_CHECK(vkCreateImage(vk.device, &ci, NULL, image));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(vk.device, *image, &req);
        VkMemoryAllocateInfo mi = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
            .memoryTypeIndex = vk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
        VkDeviceMemory mem;
        VK_CHECK(vkAllocateMemory(vk.device, &mi, NULL, &mem));
        VK_CHECK(vkBindImageMemory(vk.device, *image, mem, 0));
        /* to GENERAL and cleared to black, once */
        render_end_rendering();
        VkCommandBuffer cmd = render_cmd();
        VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = *image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS } };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        VkClearColorValue black = { { 0, 0, 0, 1 } };
        vkCmdClearColorImage(cmd, *image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &b.subresourceRange);
        render_barrier(cmd);
    }
    VkImageViewType type = is3d ? VK_IMAGE_VIEW_TYPE_3D : dim == TEXDIM_CUBE ? (array ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE)
                         : array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    uint32_t layers = is3d ? 1 : dim == TEXDIM_CUBE ? 6 : 1;
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = *image, .viewType = type,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers } };
    VK_CHECK(vkCreateImageView(vk.device, &vi, NULL, &views[d][array]));
    return views[d][array];
}

/* Samplers: one per distinct SamplerDesc */
typedef struct SamplerEntry { struct SamplerEntry *next; SamplerDesc d; VkSampler s; } SamplerEntry;
static SamplerEntry *samplers;

static VkSamplerAddressMode wrap_mode(uint8_t w) {
    switch (w) {
    case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2: case 3: case 4: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 6: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    }
    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;                       /* CLAMP, CLAMP_TO_EDGE */
}

VkSampler render_sampler(const SamplerDesc *d) {
    for (SamplerEntry *e = samplers; e; e = e->next)
        if (memcmp(&e->d, d, sizeof *d) == 0) return e->s;
    /* min filter: NEAREST, LINEAR, then {NEAREST,LINEAR}_MIPMAP_{NEAREST,LINEAR} */
    int minf = d->min_filter;
    VkFilter min = minf == 1 || minf == 3 || minf == 5 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    VkSamplerMipmapMode mip = minf == 4 || minf == 5 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    float max_lod = minf <= 1 ? 0.25f : d->lod_max;                     /* no mipmapping: level 0 only */
    float border[4];
    memcpy(border, d->border, 16);
    VkBorderColor bc = border[3] == 0.0f ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
                     : border[0] > 0.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    VkSamplerCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = d->mag_filter == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST, .minFilter = min, .mipmapMode = mip,
        .addressModeU = wrap_mode(d->wrap[0]), .addressModeV = wrap_mode(d->wrap[1]), .addressModeW = wrap_mode(d->wrap[2]),
        .mipLodBias = d->lod_bias, .compareEnable = d->compare_mode != 0, .compareOp = render_compare_op(d->compare_func),
        .minLod = d->lod_min, .maxLod = max_lod < d->lod_min ? d->lod_min : max_lod, .borderColor = bc };
    SamplerEntry *e = calloc(1, sizeof *e);
    e->d = *d;
    VK_CHECK(vkCreateSampler(vk.device, &ci, NULL, &e->s));
    e->next = samplers;
    samplers = e;
    return e->s;
}
