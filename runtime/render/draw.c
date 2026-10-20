/*
 * draw.c - running the GPU side of NVN commands with Vulkan.
 *
 * nvn/exec.c walks a command list and hands each command that needs the GPU to
 * render_command(). This file keeps the state those commands set (render targets, ...) and
 * records Vulkan commands for them.
 */
#include "render_internal.h"
#include <stdio.h>

static struct {
    RenderTargets rt;                /* as the game set them */
    HostImage *color[8], *depth;     /* their images */
    int rendering;                   /* inside vkCmdBeginRendering */
} st;

void render_end_rendering(void) {
    if (!st.rendering) return;
    vkCmdEndRendering(render_cmd());
    st.rendering = 0;
}

static void set_render_targets(const RenderTargets *rt) {
    render_end_rendering();
    st.rt = *rt;
    for (int i = 0; i < 8; i++) st.color[i] = i < (int)rt->count ? render_image_ready(rt->color[i].texture) : NULL;
    st.depth = render_image_ready(rt->depth.texture);
}

/* a color value as Vulkan wants it: float, int or uint bits (sub: 0 float, 1 int, 2 uint) */
static VkClearColorValue clear_value(const uint8_t *color) {
    VkClearColorValue v;
    memcpy(&v, color, 16);
    return v;
}

static void clear_image(HostImage *h, const VkClearColorValue *color, const VkClearDepthStencilValue *ds,
                        VkImageAspectFlags aspects) {
    if (!h) return;
    render_end_rendering();
    VkCommandBuffer cmd = render_cmd();
    render_image_init_layout(cmd, h);
    VkImageSubresourceRange range = { aspects, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS };
    if (color) vkCmdClearColorImage(cmd, h->image, VK_IMAGE_LAYOUT_GENERAL, color, 1, &range);
    else vkCmdClearDepthStencilImage(cmd, h->image, VK_IMAGE_LAYOUT_GENERAL, ds, 1, &range);
    render_barrier(cmd);
    h->gpu_written = 1;
    h->needs_upload = 0;
}

/* Which level/layers of an image an ImageRef means (its view, if any) */
static VkImageSubresourceLayers subresource(const HostImage *h, const ImageRef *ref) {
    VkImageSubresourceLayers s = { h->aspects & VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_COLOR_BIT : h->aspects,
                                   0, 0, h->layers };
    if (ref->view.set & VIEW_LEVELS) s.mipLevel = ref->view.base_level;
    if (ref->view.set & VIEW_LAYERS) { s.baseArrayLayer = ref->view.min_layer; s.layerCount = ref->view.num_layers ? ref->view.num_layers : 1; }
    if (h->depth > 1) { s.baseArrayLayer = 0; s.layerCount = 1; }
    return s;
}

/* region: x, y, z, width, height, depth; a zero size means "to the edge of the level" */
static void region_box(const HostImage *h, uint32_t level, const int32_t *r, VkOffset3D box[2]) {
    int32_t w = (int32_t)(h->width >> level), hh = (int32_t)(h->height >> level), d = (int32_t)(h->depth >> level);
    if (w < 1) w = 1;
    if (hh < 1) hh = 1;
    if (d < 1) d = 1;
    box[0] = (VkOffset3D){ r[0], r[1], h->depth > 1 ? r[2] : 0 };
    box[1] = (VkOffset3D){ r[0] + (r[3] > 0 ? r[3] : w), r[1] + (r[4] > 0 ? r[4] : hh),
                           h->depth > 1 ? r[2] + (r[5] > 0 ? r[5] : d) : 1 };
}

/* CopyTextureToTexture: same size -> vkCmdCopyImage; scaled or different formats -> blit */
static void copy_texture(const ImageRef *src_ref, const ImageRef *dst_ref, uint32_t flags) {
    HostImage *src = render_image_ready(src_ref->texture), *dst = render_image_ready(dst_ref->texture);
    if (!src || !dst) return;
    render_end_rendering();
    VkCommandBuffer cmd = render_cmd();
    render_image_init_layout(cmd, src);
    render_image_init_layout(cmd, dst);
    VkImageSubresourceLayers ss = subresource(src, src_ref), ds = subresource(dst, dst_ref);
    if (ss.layerCount != ds.layerCount) ss.layerCount = ds.layerCount = ss.layerCount < ds.layerCount ? ss.layerCount : ds.layerCount;
    VkOffset3D sb[2], db[2];
    region_box(src, ss.mipLevel, src_ref->region, sb);
    region_box(dst, ds.mipLevel, dst_ref->region, db);
    int same_size = sb[1].x - sb[0].x == db[1].x - db[0].x && sb[1].y - sb[0].y == db[1].y - db[0].y &&
                    sb[1].z - sb[0].z == db[1].z - db[0].z;
    if (same_size && src->format == dst->format) {
        VkImageCopy c = { .srcSubresource = ss, .srcOffset = sb[0], .dstSubresource = ds, .dstOffset = db[0],
                          .extent = { (uint32_t)(sb[1].x - sb[0].x), (uint32_t)(sb[1].y - sb[0].y), (uint32_t)(sb[1].z - sb[0].z) } };
        vkCmdCopyImage(cmd, src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    } else {
        VkImageBlit b = { .srcSubresource = ss, .srcOffsets = { sb[0], sb[1] }, .dstSubresource = ds,
                          .dstOffsets = { db[0], db[1] } };
        vkCmdBlitImage(cmd, src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                       (flags & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    render_barrier(cmd);
    dst->gpu_written = 1;
}

void render_command(const Cmd *r) {
    switch (r->op) {
    case CMD_SET_RENDER_TARGETS:
        set_render_targets((const RenderTargets *)r->extra);
        break;
    case CMD_CLEAR_COLOR: {                        /* a = index, color ptr, mask; extra = color */
        uint32_t index = (uint32_t)r->a[0], mask = (uint32_t)r->a[2] & 0xF;
        if (index >= 8 || !st.color[index] || !mask) break;
        if (mask != 0xF) {
            static int warned;
            if (!warned++) fprintf(stderr, "render: clears of some channels only (mask %#x) are done as full clears\n", mask);
        }
        VkClearColorValue v = clear_value(r->extra);
        clear_image(st.color[index], &v, NULL, VK_IMAGE_ASPECT_COLOR_BIT);
        break;
    }
    case CMD_CLEAR_DEPTH_STENCIL: {                /* a = -, depth mask, stencil, stencil mask; f[0] = depth */
        HostImage *d = st.depth;
        if (!d) break;
        VkImageAspectFlags aspects = 0;
        if (r->a[0] & 1) aspects |= d->aspects & VK_IMAGE_ASPECT_DEPTH_BIT;
        if (r->a[2] & 0xFF) aspects |= d->aspects & VK_IMAGE_ASPECT_STENCIL_BIT;
        VkClearDepthStencilValue v = { r->f[0], (uint32_t)r->a[1] };
        if (aspects) clear_image(d, NULL, &v, aspects);
        break;
    }
    case CMD_CLEAR_TEXTURE: {                      /* extra = { ImageRef, color[4] } */
        const ImageRef *ref = (const ImageRef *)r->extra;
        HostImage *h = render_image_ready(ref->texture);
        if (!h) break;
        VkClearColorValue v = clear_value(r->extra + sizeof(ImageRef));
        if (h->aspects & VK_IMAGE_ASPECT_COLOR_BIT) clear_image(h, &v, NULL, VK_IMAGE_ASPECT_COLOR_BIT);
        break;
    }
    case CMD_COPY_TEXTURE_TO_TEXTURE: {            /* a[6] = flags; extra = { src, dst } */
        const ImageRef *refs = (const ImageRef *)r->extra;
        copy_texture(&refs[0], &refs[1], (uint32_t)r->a[6]);
        break;
    }
    default:
        break;
    }
}

void render_submitted(void) { render_flush(); }
