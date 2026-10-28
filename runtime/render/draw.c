/*
 * draw.c - running the GPU side of NVN commands with Vulkan.
 *
 * nvn/exec.c walks a command list and hands each command that needs the GPU to
 * render_command(). This file keeps the state those commands set (render targets, ...) and
 * records Vulkan commands for them.
 */
#include "render_internal.h"
#include "astc.h"
#include "png.h"
#include "render/shader/builtin/builtin_shaders.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { uint64_t address, size; } Range;

static struct {
    RenderTargets rt;                /* as the game set them */
    HostImage *color[8], *depth;     /* their images */
    int rendering;                   /* inside vkCmdBeginRendering */
    int pass_draws;                  /* draws since it began (LAA_GPU_TIMING_AT labels) */

    Program program;
    BlendState blend[8];
    ColorState color_state;
    uint32_t channel_mask;
    DepthStencilState depth_stencil;
    MultisampleState multisample;
    PolygonState polygon;
    uint32_t nattribs;
    VertexAttribState attrib[16];
    VertexStreamState stream[16];
    Range vertex_buffer[16];
    Range uniform[6][14];            /* per NVN shader stage: vertex 0, fragment 1, ... */
    Range storage[6][16];
    uint64_t texture[6][32];         /* bound texture handles */
    DescriptorPool texture_pool, sampler_pool;
    float viewport[4], depth_range[2];
    int32_t scissor[4];
    float blend_color[4];
    uint32_t stencil_ref[2], stencil_mask[2], stencil_value_mask[2];
    float line_width, polygon_offset[3];
    uint64_t copy_row_stride, copy_image_stride;     /* bytes; 0 = tightly packed */
    uint32_t groups[3];                              /* the dispatch being set up (compute's c0) */
    uint32_t primitive_restart, restart_index;
} st = {
    .channel_mask = 0xFFFFFFFF, .viewport = { 0, 0, 1, 1 }, .depth_range = { 0, 1 },
    .scissor = { 0, 0, 0x7FFF, 0x7FFF }, .stencil_mask = { 0xFF, 0xFF }, .stencil_value_mask = { 0xFF, 0xFF },
    .line_width = 1.0f,
};

int render_passes;                                   /* begun so far (LAA_GPU_TIMING_AT) */

/* Dynamic rendering has no dependencies between passes of its own (render pass objects had
 * subpass dependencies): without a barrier, the GPU may overlap what comes next with this pass.
 * The Adreno did: a glow pass read its blurred mask while the next pass rendered the mask again
 * (the purple box behind "Press L+R"). NVN orders these with nvnCommandBufferBarrier; we order
 * every pass's writes before all that follows (LAA_NO_PASS_BARRIER=1: as before, to compare). */
void render_end_rendering(void) {
    if (!st.rendering) return;
    static int no_barrier = -1;
    if (no_barrier < 0) no_barrier = getenv("LAA_NO_PASS_BARRIER") != NULL;
    vkCmdEndRendering(render_cmd());
    if (!no_barrier) render_barrier(render_cmd());
    st.rendering = 0;
}

/* ---- descriptor sets: from a pool per submission context, emptied when it is reused ---- */

static VkDescriptorPool descriptor_pools[RENDER_CONTEXTS];

static void forget_sets(int context);

void render_submission_done(int context) {
    if (descriptor_pools[context]) vkResetDescriptorPool(vk.device, descriptor_pools[context], 0);
    forget_sets(context);
}

static VkDescriptorSet new_set(VkDescriptorSetLayout layout) {
    render_cmd();                                    /* (recording first: starting it empties the pool) */
    VkDescriptorPool *pool = &descriptor_pools[render_context()];
    if (!*pool) {
        VkDescriptorPoolSize sizes[3] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4096 * 4 },
                                          { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 4096 * 2 * RENDER_DYNAMIC_CBUFS },
                                          { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096 * 8 } };
        VkDescriptorPoolCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 4096,
                                          .poolSizeCount = 3, .pPoolSizes = sizes };
        VK_CHECK(vkCreateDescriptorPool(vk.device, &ci, NULL, pool));
    }
    VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                       .descriptorPool = *pool, .descriptorSetCount = 1, .pSetLayouts = &layout };
    VkDescriptorSet set;
    if (vkAllocateDescriptorSets(vk.device, &ai, &set) != VK_SUCCESS) {
        render_submit();                             /* pool full: send the work so far, go on in the next context */
        return new_set(layout);
    }
    return set;
}

/* Sets already made in this submission context, by what is in them (two 64-bit hashes of the
 * writes, dynamic offsets left out): most draws use the same pools, shaders and textures as
 * one before them. Emptied with the context's descriptor pool. LAA_NO_SET_CACHE=1: always new. */
#define SET_CACHE 2048
typedef struct { uint64_t h1, h2; VkDescriptorSetLayout layout; VkDescriptorSet set; } CachedSet;
static CachedSet set_cache[RENDER_CONTEXTS][SET_CACHE];
static unsigned set_cache_used[RENDER_CONTEXTS];

static void hash_in(uint64_t *h1, uint64_t *h2, uint64_t v) {
    *h1 = (*h1 ^ v) * 0x100000001B3ull;
    *h2 = (*h2 ^ (v * 0x9E3779B97F4A7C15ull)) * 0xC2B2AE3D27D4EB4Full;
    *h2 ^= *h2 >> 31;
}

static void forget_sets(int context) {               /* (recycler thread: the context isn't in use) */
    memset(set_cache[context], 0, sizeof set_cache[context]);
    set_cache_used[context] = 0;
}

static VkDescriptorSet get_set(VkDescriptorSetLayout layout, VkWriteDescriptorSet *writes, uint32_t nw) {
    static int no_cache = -1;
    if (no_cache < 0) no_cache = getenv("LAA_NO_SET_CACHE") != NULL;
    uint64_t h1 = 0xCBF29CE484222325ull, h2 = 0x84222325CBF29CE4ull;
    hash_in(&h1, &h2, (uint64_t)(uintptr_t)layout);
    for (uint32_t i = 0; i < nw; i++) {
        const VkWriteDescriptorSet *w = &writes[i];
        hash_in(&h1, &h2, (uint64_t)w->dstBinding << 32 | (uint32_t)w->descriptorType);
        if (w->pBufferInfo) {
            hash_in(&h1, &h2, (uint64_t)(uintptr_t)w->pBufferInfo->buffer);
            hash_in(&h1, &h2, w->pBufferInfo->offset ^ w->pBufferInfo->range << 32);
        } else {
            hash_in(&h1, &h2, (uint64_t)(uintptr_t)w->pImageInfo->imageView);
            hash_in(&h1, &h2, (uint64_t)(uintptr_t)w->pImageInfo->sampler);
        }
    }
    render_cmd();
    int c = render_context();
    CachedSet *slot = NULL;
    if (!no_cache)
        for (unsigned k = 0; k < SET_CACHE; k++) {
            CachedSet *e = &set_cache[c][(h1 + k) % SET_CACHE];
            if (!e->set) { slot = e; break; }
            if (e->h1 == h1 && e->h2 == h2 && e->layout == layout) return e->set;
        }
    VkDescriptorSet set = new_set(layout);
    if (render_context() != c) slot = NULL;          /* (the pool was full: a new context, its own table) */
    for (uint32_t i = 0; i < nw; i++) writes[i].dstSet = set;
    vkUpdateDescriptorSets(vk.device, nw, writes, 0, NULL);
    if (slot && set_cache_used[c] < SET_CACHE * 3 / 4) {
        *slot = (CachedSet){ h1, h2, layout, set };
        set_cache_used[c]++;
    }
    return set;
}

/* A buffer of zeros, for constant buffers the game didn't bind (and c0, the driver's) */
static VkBuffer zero_buffer(void) {
    static VkBuffer buffer;
    if (buffer) return buffer;
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 0x10000,
                              .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VK_CHECK(vkCreateBuffer(vk.device, &bi, NULL, &buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(vk.device, buffer, &req);
    VkMemoryAllocateInfo mi = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
        .memoryTypeIndex = vk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VkDeviceMemory mem;
    VK_CHECK(vkAllocateMemory(vk.device, &mi, NULL, &mem));
    VK_CHECK(vkBindBufferMemory(vk.device, buffer, mem, 0));
    void *p;
    VK_CHECK(vkMapMemory(vk.device, mem, 0, VK_WHOLE_SIZE, 0, &p));
    memset(p, 0, 0x10000);
    return buffer;
}

static void warn_once(const char *what, uint64_t key) {
    static uint64_t seen[64];
    static int nseen;
    for (int i = 0; i < nseen; i++) if (seen[i] == key) return;
    if (nseen < 64) seen[nseen++] = key;
    fprintf(stderr, "render: %s (%#llx)\n", what, (unsigned long long)key);
}

static void set_render_targets(const RenderTargets *rt) {
    render_end_rendering();
    st.rt = *rt;
    for (int i = 0; i < 8; i++) st.color[i] = i < (int)rt->count ? render_image_target(rt->color[i].texture) : NULL;
    st.depth = render_image_target(rt->depth.texture);
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
    if (render_tracing()) fprintf(stderr, "render: clear %ux%u fmt %d aspects %#x\n", h->width, h->height, h->format, aspects);
    render_end_rendering();                          /* (the next draw starts a pass that clears) */
    VkClearValue v;
    if (color) v.color = *color;
    else v.depthStencil = *ds;
    render_image_clear(h, &v, aspects);
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

static int can_blit(VkFormat f, VkFormatFeatureFlags need) {
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(vk.gpu, f, &p);
    return (p.optimalTilingFeatures & need) == need;
}

/* ---- scaled depth copies by drawing, for GPUs that can't blit depth formats (the Thor's D24S8):
 * a full-screen triangle whose fragment shader writes the source's depth at the matching point ---- */
static VkDescriptorSetLayout dc_set_layout;
static VkPipelineLayout dc_layout;
static VkSampler dc_sampler;
static struct { VkFormat format; VkPipeline pipeline; } dc_pipelines[4];

static VkShaderModule builtin_module(const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code };
    VkShaderModule m;
    VK_CHECK(vkCreateShaderModule(vk.device, &ci, NULL, &m));
    return m;
}

static VkPipeline depth_copy_pipeline(VkFormat format) {
    for (int i = 0; i < 4; i++) if (dc_pipelines[i].pipeline && dc_pipelines[i].format == format) return dc_pipelines[i].pipeline;
    if (!dc_layout) {
        VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
        VkDescriptorSetLayoutCreateInfo li = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b };
        VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &li, NULL, &dc_set_layout));
        VkPushConstantRange pr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
        VkPipelineLayoutCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                          .pSetLayouts = &dc_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr };
        VK_CHECK(vkCreatePipelineLayout(vk.device, &pi, NULL, &dc_layout));
        VkSamplerCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
            .minFilter = VK_FILTER_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
        VK_CHECK(vkCreateSampler(vk.device, &si, NULL, &dc_sampler));
    }
    VkShaderModule vs = builtin_module(spv_fullscreen_vert, sizeof spv_fullscreen_vert),
                   fs = builtin_module(spv_depth_copy_frag, sizeof spv_depth_copy_frag);
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" },
    };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                  .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                             .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                  .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1 };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineDepthStencilStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE, .depthCompareOp = VK_COMPARE_OP_ALWAYS };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                            .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkPipelineRenderingCreateInfo ri = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .depthAttachmentFormat = format,
        .stencilAttachmentFormat = (render_aspects(format) & VK_IMAGE_ASPECT_STENCIL_BIT) ? format : VK_FORMAT_UNDEFINED };
    VkGraphicsPipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp,
        .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb,
        .pDynamicState = &dy, .layout = dc_layout };
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(vk.device, vk.pipeline_cache, 1, &ci, NULL, &p));
    vkDestroyShaderModule(vk.device, vs, NULL);
    vkDestroyShaderModule(vk.device, fs, NULL);
    for (int i = 0; i < 4; i++) if (!dc_pipelines[i].pipeline) { dc_pipelines[i].format = format; dc_pipelines[i].pipeline = p; break; }
    return p;
}

/* the depth of src's region sb, scaled into dst's region db (level 0 of both) */
static void depth_copy(HostImage *src, HostImage *dst, const VkOffset3D sb[2], const VkOffset3D db[2]) {
    VkPipeline pipeline = depth_copy_pipeline(dst->format);
    VkDescriptorSet set = new_set(dc_set_layout);
    VkDescriptorImageInfo ii = { dc_sampler, render_image_view(src, &(TextureView){ 0 }, TEXDIM_2D, 0), VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet wr = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0,
                                .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii };
    vkUpdateDescriptorSets(vk.device, 1, &wr, 0, NULL);
    VkCommandBuffer cmd = render_cmd();
    render_image_init_layout(cmd, src);
    render_image_init_layout(cmd, dst);
    render_barrier(cmd);
    VkRenderingAttachmentInfo att = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = dst->target_view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
    int has_stencil = (dst->aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { dst->width, dst->height } },
        .layerCount = 1, .pDepthAttachment = &att, .pStencilAttachment = has_stencil ? &att : NULL };
    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, dc_layout, 0, 1, &set, 0, NULL);
    float dw = (float)(db[1].x - db[0].x), dh = (float)(db[1].y - db[0].y);
    VkViewport v = { (float)db[0].x, (float)db[0].y, dw, dh, 0, 1 };
    VkRect2D sc = { { db[0].x, db[0].y }, { (uint32_t)dw, (uint32_t)dh } };
    vkCmdSetViewport(cmd, 0, 1, &v);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    /* uv = FragCoord * scale + offset: dst pixel x -> src x = sb0 + (x - db0) * sw / dw, over src's size */
    float sx = (float)(sb[1].x - sb[0].x) / dw, sy = (float)(sb[1].y - sb[0].y) / dh;
    float pc[4] = { sx / (float)src->width, sy / (float)src->height,
                    ((float)sb[0].x - (float)db[0].x * sx) / (float)src->width, ((float)sb[0].y - (float)db[0].y * sy) / (float)src->height };
    vkCmdPushConstants(cmd, dc_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

/* CopyTextureToTexture: same size -> vkCmdCopyImage; scaled or different formats -> blit */
static void copy_texture(const ImageRef *src_ref, const ImageRef *dst_ref, uint32_t flags) {
    HostImage *src = render_image_ready(src_ref->texture), *dst = render_image_ready(dst_ref->texture);
    if (!src || !dst) return;
    /* LAA_DUMP_COPIES_AT=frame (+ LAA_DUMP_DIR): save the source of each copy in that frame */
    static int64_t dump_at = -2;
    static int copy_no;
    if (dump_at == -2) dump_at = getenv("LAA_DUMP_COPIES_AT") ? strtoll(getenv("LAA_DUMP_COPIES_AT"), NULL, 0) : -1;
    if (dump_at >= 0 && nvn_frame_count() == (uint64_t)dump_at) {
        char path[512];
        snprintf(path, sizeof path, "%s/copy%02d_fmt%d_%ux%u.png", getenv("LAA_DUMP_DIR") ? getenv("LAA_DUMP_DIR") : ".",
                 copy_no++, src->format, src->width, src->height);
        render_debug_dump(src, path);
    }
    if (render_tracing())
        fprintf(stderr, "render: copy %ux%u fmt %d -> %ux%u fmt %d, flags %#x, regions %d,%d %dx%d -> %d,%d %dx%d, views %#x %#x\n",
                src->width, src->height, src->format, dst->width, dst->height, dst->format, flags, src_ref->region[0],
                src_ref->region[1], src_ref->region[3], src_ref->region[4], dst_ref->region[0], dst_ref->region[1],
                dst_ref->region[3], dst_ref->region[4], src_ref->view.set, dst_ref->view.set);
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
    } else if (!(src->aspects & VK_IMAGE_ASPECT_COLOR_BIT)) {
        /* scaled depth (e.g. a smaller depth buffer for ambient occlusion): a blit of the depth
         * aspect with NEAREST, where the GPU can blit that format */
        if (!(dst->aspects & VK_IMAGE_ASPECT_DEPTH_BIT)) {
            warn_once("scaled copy of a depth image into a non-depth image", dst->format);
            return;
        }
        static int force_draw = -1;                  /* LAA_DEPTH_COPY_DRAW: test the drawn copy where blits work */
        if (force_draw < 0) force_draw = getenv("LAA_DEPTH_COPY_DRAW") != NULL;
        if (!can_blit(src->format, VK_FORMAT_FEATURE_BLIT_SRC_BIT) || !can_blit(dst->format, VK_FORMAT_FEATURE_BLIT_DST_BIT) ||
            force_draw) {
            depth_copy(src, dst, sb, db);
            render_barrier(render_cmd());            /* (depth_copy may have moved on to another submission) */
            render_image_gpu_wrote(dst);
            return;
        }
        ss.aspectMask = ds.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        VkImageBlit b = { .srcSubresource = ss, .srcOffsets = { sb[0], sb[1] }, .dstSubresource = ds,
                          .dstOffsets = { db[0], db[1] } };
        vkCmdBlitImage(cmd, src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_NEAREST);
    } else {
        VkImageBlit b = { .srcSubresource = ss, .srcOffsets = { sb[0], sb[1] }, .dstSubresource = ds,
                          .dstOffsets = { db[0], db[1] } };
        vkCmdBlitImage(cmd, src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                       (flags & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    render_barrier(cmd);
    render_image_gpu_wrote(dst);
}

/* CopyBufferToTexture / CopyTextureToBuffer: between guest memory at `address` (rows
 * st.copy_row_stride apart, 0 = packed) and a region of the image. For array textures the
 * region's z / depth are layers. The buffer is the game's own memory when its pool is imported. */
static void copy_buffer_image(uint64_t address, const ImageRef *ref, int to_buffer) {
    HostImage *h = render_image_ready(ref->texture);
    if (!h || !address) return;
    TexShape s;
    tex_shape(&h->desc, &s);
    VkImageSubresourceLayers sub = subresource(h, ref);
    if (!(h->aspects & VK_IMAGE_ASPECT_COLOR_BIT)) sub.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;   /* (depth only) */
    VkOffset3D box[2];
    region_box(h, sub.mipLevel, ref->region, box);
    if (h->depth == 1 && h->layers > 1 && !(ref->view.set & VIEW_LAYERS)) {
        sub.baseArrayLayer = ref->region[2] > 0 ? (uint32_t)ref->region[2] : 0;
        sub.layerCount = ref->region[5] > 0 ? (uint32_t)ref->region[5] : 1;
    } else if (h->depth == 1) {
        sub.layerCount = 1;
    }
    VkExtent3D ext = { (uint32_t)(box[1].x - box[0].x), (uint32_t)(box[1].y - box[0].y), (uint32_t)(box[1].z - box[0].z) };
    uint32_t wb = (ext.width + s.fmt->bw - 1) / s.fmt->bw, hb = (ext.height + s.fmt->bh - 1) / s.fmt->bh;
    uint64_t row = st.copy_row_stride ? st.copy_row_stride : (uint64_t)wb * s.fmt->bytes;
    uint64_t image = st.copy_image_stride ? st.copy_image_stride : row * hb;
    uint64_t total = image * (ext.depth > sub.layerCount ? ext.depth : sub.layerCount);
    if (row % s.fmt->bytes) { warn_once("copy row stride that isn't whole texels", (int)row); return; }
    VkBufferImageCopy c = { .bufferRowLength = (uint32_t)(row / s.fmt->bytes) * s.fmt->bw,
                            .bufferImageHeight = (uint32_t)(image / row) * s.fmt->bh,
                            .imageSubresource = sub, .imageOffset = box[0], .imageExtent = ext };
    VkBuffer buffer;
    VkDeviceSize offset;
    uint8_t *staged = NULL;
    if (h->astc_decode) {                            /* ASTC we decode: blocks from the buffer -> RGBA */
        if (to_buffer) { warn_once("ASTC texture readback", 0); return; }
        uint64_t out_row = (uint64_t)ext.width * 4, out_image = out_row * ext.height;
        uint32_t images = ext.depth > sub.layerCount ? ext.depth : sub.layerCount;
        staged = render_staging(out_image * images, &offset);
        for (uint32_t i = 0; i < images; i++)
            astc_decode(gptr(address + i * image), row, ext.width, ext.height, s.fmt->bw, s.fmt->bh,
                        h->format == VK_FORMAT_R8G8B8A8_SRGB, staged + i * out_image, out_row);
        buffer = render_staging_buffer();
        c.bufferRowLength = ext.width;
        c.bufferImageHeight = ext.height;
    } else if (render_guest_buffer(address, &buffer, &offset) != 0 || offset % 4 || offset % s.fmt->bytes) {
        if (to_buffer) { warn_once("texture readback into memory that isn't in a pool", 0); return; }
        staged = render_staging(total, &offset);    /* not in an imported pool: copy through staging */
        memcpy(staged, gptr(address), total);
        buffer = render_staging_buffer();
    }
    c.bufferOffset = offset;
    render_end_rendering();
    VkCommandBuffer cmd = render_cmd();
    render_image_init_layout(cmd, h);
    if (to_buffer) vkCmdCopyImageToBuffer(cmd, h->image, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &c);
    else vkCmdCopyBufferToImage(cmd, buffer, h->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    render_barrier(cmd);
    if (!to_buffer) render_image_gpu_wrote(h);
}

/* ---- drawing ---- */

/* Start rendering into the current targets, if not already. 0 on success. */
static int begin_rendering(void) {
    if (st.rendering) return 0;
    VkRenderingAttachmentInfo color[8], depth = { 0 };
    uint32_t ncolor = 0, w = 0x7FFFFFFF, h = 0x7FFFFFFF;
    VkCommandBuffer cmd = render_cmd();
    for (uint32_t i = 0; i < st.rt.count; i++) {
        HostImage *img = st.color[i];
        color[i] = (VkRenderingAttachmentInfo){ .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        if (img) {
            render_image_init_layout(cmd, img);
            color[i].imageView = img->target_view;
            color[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            color[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            color[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            if (img->width < w) w = img->width;
            if (img->height < h) h = img->height;
        }
        ncolor = i + 1;
    }
    if (st.depth) {
        render_image_init_layout(cmd, st.depth);
        depth = (VkRenderingAttachmentInfo){ .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = st.depth->target_view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
        if (st.depth->width < w) w = st.depth->width;
        if (st.depth->height < h) h = st.depth->height;
    }
    if (w == 0x7FFFFFFF) return -1;                      /* nothing to render into */
    /* pending clears (render_image_clear) become loadOp CLEAR where the pass covers the image */
    VkRenderingAttachmentInfo stencil = depth;
    for (uint32_t i = 0; i < ncolor; i++) {
        HostImage *img = st.color[i];
        if (!img || !img->pending_clear) continue;
        if (img->width != w || img->height != h) { render_image_flush_clear(img); continue; }
        color[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color[i].clearValue = img->clear_value;
        img->pending_clear = 0;
    }
    if (st.depth && st.depth->pending_clear) {
        HostImage *img = st.depth;
        if (img->width != w || img->height != h) {
            render_image_flush_clear(img);
        } else {
            if (img->pending_clear & VK_IMAGE_ASPECT_DEPTH_BIT) { depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; depth.clearValue = img->clear_value; }
            if (img->pending_clear & VK_IMAGE_ASPECT_STENCIL_BIT) { stencil.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; stencil.clearValue = img->clear_value; }
            img->pending_clear = 0;
        }
    }
    int has_d = st.depth && (st.depth->aspects & VK_IMAGE_ASPECT_DEPTH_BIT);
    int has_s = st.depth && (st.depth->aspects & VK_IMAGE_ASPECT_STENCIL_BIT);
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { w, h } },
        .layerCount = 1, .colorAttachmentCount = ncolor, .pColorAttachments = color,
        .pDepthAttachment = has_d ? &depth : NULL, .pStencilAttachment = has_s ? &stencil : NULL };
    vkCmdBeginRendering(cmd, &ri);
    st.rendering = 1;
    st.pass_draws = 0;
    render_passes++;
    return 0;
}

/* LAA_DUMP_TEXTURE=storage address (+ LAA_DUMP_DRAWS_AT=frame, LAA_DUMP_DIR): when that texture is
 * sampled in that frame, save our image of it and (1-, 2- and 4-byte formats) its guest memory */
static void dump_texture_if_asked(HostImage *h) {
    static uint64_t want = 1, done_frame = ~0ull;
    static int64_t frame;
    if (want == 1) {
        want = getenv("LAA_DUMP_TEXTURE") ? strtoull(getenv("LAA_DUMP_TEXTURE"), NULL, 0) : 0;
        frame = getenv("LAA_DUMP_DRAWS_AT") ? strtoll(getenv("LAA_DUMP_DRAWS_AT"), NULL, 0) : -1;
    }
    if (!want || h->address != want || nvn_frame_count() != (uint64_t)frame || done_frame == (uint64_t)frame) return;
    done_frame = (uint64_t)frame;
    const char *dir = getenv("LAA_DUMP_DIR") ? getenv("LAA_DUMP_DIR") : ".";
    char path[512];
    snprintf(path, sizeof path, "%s/texture_%llx_gpu.png", dir, (unsigned long long)h->address);
    render_debug_dump(h, path);
    TexShape s;
    tex_shape(&h->desc, &s);
    if (s.fmt->bw != 1 || s.fmt->bytes == 3 || s.fmt->bytes > 4) return;
    LevelShape l = tex_level(&s, 0);
    uint8_t *raw = malloc((size_t)s.width * s.height * s.fmt->bytes), *rgba = malloc((size_t)s.width * s.height * 4);
    uint32_t region[6] = { 0, 0, 0, l.wb, l.hb, 1 };
    if (h->desc.flags & TEXTURE_FLAG_LINEAR)         /* rows `stride` bytes apart */
        for (uint32_t y = 0; y < s.height; y++)
            memcpy(raw + (size_t)y * s.width * s.fmt->bytes, gptr(h->address + y * h->desc.stride), (size_t)s.width * s.fmt->bytes);
    else
        tex_copy_level(gptr(h->address), raw, &s, &l, region, (uint64_t)s.width * s.fmt->bytes, 0, 1);
    for (size_t i = 0; i < (size_t)s.width * s.height; i++)
        for (int c = 0; c < 4; c++)                  /* (1 byte: grey; 2 bytes: red and green) */
            rgba[4 * i + c] = c == 3 ? 255 : s.fmt->bytes == 1 ? raw[i] : c < (int)s.fmt->bytes ? raw[s.fmt->bytes * i + c] : 0;
    snprintf(path, sizeof path, "%s/texture_%llx_guest.png", dir, (unsigned long long)h->address);
    if (png_write(path, rgba, s.width, s.height, (int32_t)s.width * 4) == 0) fprintf(stderr, "render: wrote %s\n", path);
    free(raw);
    free(rgba);
}

/* Bind the constant buffers and textures one shader stage uses */
/* A table of buffers for shaders that reach them through pointers: 16 bytes each, the
 * device address (what LDG uses) and the size */
static void put_buffer_table(uint8_t *at, const Range *r, int n) {
    for (int i = 0; i < n; i++) {
        uint64_t dev = r[i].address ? render_guest_device_address(r[i].address) : 0;
        uint32_t size32 = (uint32_t)r[i].size;
        memcpy(at + 0x10 * i, &dev, 8);
        memcpy(at + 8 + 0x10 * i, &size32, 4);
    }
}

/* The stage's descriptor writes (their set comes later: get_set), and its constant buffers'
 * dynamic offsets: dyn[slot] for c0..c15 */
static int fill_stage(HostShader *sh, int stage, VkWriteDescriptorSet *writes, uint32_t *nw,
                      VkDescriptorBufferInfo *buffers, uint32_t *nb, VkDescriptorImageInfo *images, uint32_t *ni,
                      uint32_t *dyn) {
    uint32_t base = (uint32_t)stage * SHADER_STAGE_BINDINGS;
    for (int slot = 0; slot < 18; slot++) {
        if (!(sh->info.cbuf_mask >> slot & 1)) continue;
        VkDescriptorBufferInfo *bi = &buffers[(*nb)++];
        *bi = (VkDescriptorBufferInfo){ zero_buffer(), 0, 0x10000 };
        uint64_t addr = 0, size = 0;
        if (slot == 0) {                             /* the driver's constants: storage buffer addresses */
            VkBuffer b;
            VkDeviceSize off;
            uint8_t *c0 = render_uniforms(0x800, &b, &off);
            memset(c0, 0, 0x800);
            /* graphics: stage s's storage buffers at 0x110 + 0x400 * s. Compute has its own c0:
             * group counts at 0x200, its uniform buffers at 0x210 (it reads them with LDG: compute
             * has few hardware constant buffers), storage buffers at 0x310. (Layout as LASRecomp found.) */
            if (stage == SHADER_COMPUTE) {
                memcpy(c0 + 0x200, st.groups, 12);
                put_buffer_table(c0 + 0x210, st.uniform[SHADER_COMPUTE], 14);
                put_buffer_table(c0 + 0x310, st.storage[SHADER_COMPUTE], 16);
            } else {
                for (int s = 0; s <= SHADER_FRAGMENT; s++) put_buffer_table(c0 + 0x110 + 0x400 * s, st.storage[s], 16);
            }
            *bi = (VkDescriptorBufferInfo){ b, off, 0x800 };
        }
        if (slot == 1) { addr = sh->constants; size = sh->constants_size; }
        else if (slot >= 3 && slot - 3 < 14) { addr = st.uniform[stage][slot - 3].address; size = st.uniform[stage][slot - 3].size; }
        if (addr && size) {
            VkBuffer b;
            VkDeviceSize off;
            if (render_guest_buffer(addr, &b, &off) == 0 && off % vk.limits.minUniformBufferOffsetAlignment == 0) {
                *bi = (VkDescriptorBufferInfo){ b, off, size < 0x10000 ? (size + 15) & ~15ull : 0x10000 };
            } else {
                warn_once("uniform buffer not usable", addr);
            }
        }
        int dynamic = slot < RENDER_DYNAMIC_CBUFS;
        if (dynamic) {                               /* the offset at bind time, the set without it */
            dyn[slot] = (uint32_t)bi->offset;
            bi->offset = 0;
        }
        writes[(*nw)++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstBinding = base + SHADER_CBUF_BINDING + (uint32_t)slot, .descriptorCount = 1,
            .descriptorType = dynamic ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo = bi };
    }
    for (uint32_t i = 0; i < sh->info.ntextures; i++) {
        const ShaderTexture *tx = &sh->info.textures[i];
        VkDescriptorImageInfo *ii = &images[(*ni)++];
        ii->imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        ii->imageView = VK_NULL_HANDLE;
        ii->sampler = VK_NULL_HANDLE;
        /* the handle: texture id | sampler id << 20, bound at index (offset / 4 - 8) / 2 */
        uint32_t index = (tx->handle_offset / 4 - 8) / 2;
        uint64_t handle = index < 32 ? st.texture[stage][index] : 0;
        uint32_t tex_id = (uint32_t)(handle & 0xFFFFF), smp_id = (uint32_t)(handle >> 20 & 0xFFF);
        if (render_tracing())
            fprintf(stderr, "render:   binding %u: handle %#llx from index %u (offset %#x)\n", i, (unsigned long long)handle, index,
                    tx->handle_offset);
        if (handle && st.texture_pool.entries && tex_id < st.texture_pool.count) {
            const TexturePoolEntry *e = &((TexturePoolEntry *)st.texture_pool.entries)[tex_id];
            HostImage *h = e->texture ? render_image_ready(e->texture) : NULL;
            if (h) dump_texture_if_asked(h);
            if (h) ii->imageView = render_image_view(h, &e->view, tx->dim, tx->array);
            if (h && render_tracing())
                fprintf(stderr, "render:   texture %u: id %u, object %#llx storage %#llx %ux%u nvn format %#x vk %d view set %#x fmt %#x, levels %u layers %u, gpu written %d\n", i, tex_id,
                        (unsigned long long)e->texture, (unsigned long long)h->address,
                        h->width, h->height, h->desc.format, h->format, e->view.set, e->view.format, h->levels, h->layers, h->gpu_written),
                fprintf(stderr, "render:     swizzle %u %u %u %u / view %u %u %u %u, image %p, target %p, sampler %u\n",
                        h->desc.swizzle[0], h->desc.swizzle[1], h->desc.swizzle[2], h->desc.swizzle[3], e->view.swizzle[0],
                        e->view.swizzle[1], e->view.swizzle[2], e->view.swizzle[3], (void *)h, (void *)st.color[0], smp_id);
        }
        if (st.sampler_pool.entries && smp_id < st.sampler_pool.count) {
            /* Maxwell compares depth only in instructions that ask for it (TEX.DC); a sampler with
             * compare on, used by a plain sample, is undefined in Vulkan (the Adreno compares
             * anyway: the tilt-shift blur read 0/1 instead of depth and blurred everything) */
            SamplerDesc d = ((SamplerDesc *)st.sampler_pool.entries)[smp_id];
            if (!tx->shadow) d.compare_mode = 0;
            ii->sampler = render_sampler(&d);
        }
        if (st.sampler_pool.entries && smp_id < st.sampler_pool.count && render_tracing()) {
            const SamplerDesc *d = &((SamplerDesc *)st.sampler_pool.entries)[smp_id];
            float bc[4];
            memcpy(bc, d->border, sizeof bc);
            fprintf(stderr, "render:     sampler %u: filt %u/%u wrap %u %u %u cmp %u/%u red %u lod %g..%g bias %g aniso %g border %g,%g,%g,%g\n",
                    smp_id, d->min_filter, d->mag_filter, d->wrap[0], d->wrap[1], d->wrap[2], d->compare_mode, d->compare_func,
                    d->reduction, d->lod_min, d->lod_max, d->lod_bias, d->max_anisotropy, bc[0], bc[1], bc[2], bc[3]);
        }
        if (!ii->sampler) { SamplerDesc d = { .wrap = { 1, 1, 1 }, .lod_max = 1000 }; ii->sampler = render_sampler(&d); }
        if (!ii->imageView && render_tracing())
            fprintf(stderr, "render:   texture %u: NOT FOUND (handle %#llx: id %u of %u, sampler %u; offset %#x)\n", i,
                    (unsigned long long)handle, tex_id, st.texture_pool.count, smp_id, tx->handle_offset);
        if (!ii->imageView) ii->imageView = render_dummy_view(tx->dim, tx->array);
        writes[(*nw)++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstBinding = base + SHADER_TEXTURE_BINDING + i, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = ii };
    }
    return 0;
}

/* DRAW_ARRAYS: a = mode, first, count, base instance, instances
 * DRAW_ELEMENTS: a = mode, index type, count, index buffer, base vertex, base instance, instances */
/* draw statistics, printed with the frame count (render_stats) */
static struct { uint64_t done, no_shader, failed_shader, other, dispatches; } stats;

/* ---- buffer writes, in order with draws and dispatches ----
 * Buffers live in the game's memory pools, which the GPU uses in place (buffer.c), so a copy
 * is a GPU copy between two imported buffers. Memory outside the pools: finish the GPU's
 * work so far, then do it on the CPU. */
static int pool_range(uint64_t address, uint64_t size, VkBuffer *b, VkDeviceSize *off) {
    uint64_t storage, pool_size;
    return render_guest_buffer(address, b, off) == 0 && nvn_pool_find(address + size - 1, &storage, &pool_size) &&
           address >= storage;
}

static void copy_buffer(uint64_t src, uint64_t dst, uint64_t size) {
    VkBuffer sb, db;
    VkDeviceSize so, dof;
    int overlap = src < dst + size && dst < src + size;
    if (!size) return;
    if (overlap || !pool_range(src, size, &sb, &so) || !pool_range(dst, size, &db, &dof)) {
        render_flush();
        memmove(gptr(dst), gptr(src), size);
        return;
    }
    render_end_rendering();
    VkCommandBuffer cmd = render_cmd();
    VkBufferCopy c = { so, dof, size };
    vkCmdCopyBuffer(cmd, sb, db, 1, &c);
    render_barrier(cmd);
}

static void update_buffer(uint64_t dst, const void *data, uint64_t size) {
    VkBuffer db;
    VkDeviceSize dof;
    if (!size) return;
    if (size > 65536 || size % 4 || dst % 4 || !pool_range(dst, size, &db, &dof)) {
        render_flush();
        memcpy(gptr(dst), data, size);
        return;
    }
    render_end_rendering();
    VkCommandBuffer cmd = render_cmd();
    vkCmdUpdateBuffer(cmd, db, dof, size, data);
    render_barrier(cmd);
}

static void fill_buffer(uint64_t dst, uint64_t size, uint32_t value) {
    VkBuffer db;
    VkDeviceSize dof;
    size &= ~3ull;
    if (!size) return;
    if (dst % 4 || !pool_range(dst, size, &db, &dof)) {
        render_flush();
        for (uint64_t o = 0; o < size; o += 4) memcpy(gptr(dst + o), &value, 4);
        return;
    }
    render_end_rendering();
    VkCommandBuffer cmd = render_cmd();
    vkCmdFillBuffer(cmd, db, dof, size, value);
    render_barrier(cmd);
}

/* ---- compute ---- */

/* DispatchCompute (a = groups x, y, z) / DispatchComputeIndirect (a = address of the counts) */
/* The game records each frame as one command list, so the GPU used to start only after we had
 * translated all of it (~4 ms): the game then waits for that frame on the GPU, and the two
 * together missed the 60 Hz deadline by about a millisecond. Now the work so far is submitted
 * whenever a new render pass is about to start (not inside one: splitting a pass costs the
 * tiler a store and a load) and enough has been recorded: the GPU works while we translate. */
#define EARLY_SUBMIT_WORK 96                         /* (measured best on the Thor; LAA_EARLY_SUBMIT=n) */
static void submit_early(void) {
    static int every = -1;
    if (every < 0) every = getenv("LAA_EARLY_SUBMIT") ? atoi(getenv("LAA_EARLY_SUBMIT")) : EARLY_SUBMIT_WORK;
    if (!st.rendering && every > 0 && render_recorded_work() >= (uint32_t)every) render_submit();
}

static void dispatch(const Cmd *r, int indirect) {
    submit_early();
    const Program *p = &st.program;
    if (!(p->stages >> STAGE_COMPUTE & 1)) { warn_once("dispatch without a compute shader", p->stages); return; }
    HostShader *cs = render_shader(p->code[STAGE_COMPUTE], p->control[STAGE_COMPUTE], SHADER_COMPUTE, 0);
    VkPipeline pipeline = render_compute_pipeline(cs);
    if (!pipeline) { stats.failed_shader++; return; }
    if (indirect) memcpy(st.groups, gptr(r->a[0]), 12);   /* (as the CPU sees them now) */
    else for (int i = 0; i < 3; i++) st.groups[i] = (uint32_t)r->a[i];
    VkWriteDescriptorSet writes[50];
    VkDescriptorBufferInfo buffers[18];
    VkDescriptorImageInfo images[SHADER_MAX_TEXTURES];
    uint32_t nw = 0, nb = 0, ni = 0, dyn[RENDER_DYNAMIC_CBUFS] = { 0 };
    fill_stage(cs, SHADER_COMPUTE, writes, &nw, buffers, &nb, images, &ni, dyn);
    VkDescriptorSet set = get_set(render_compute_set_layout(), writes, nw);
    if (render_tracing())
        fprintf(stderr, "render: dispatch cs %#llx %s %llu x %llu x %llu (local %u x %u x %u)\n", (unsigned long long)cs->code,
                indirect ? "indirect" : "", (unsigned long long)r->a[0], (unsigned long long)r->a[1],
                (unsigned long long)r->a[2], cs->info.local_size[0], cs->info.local_size[1], cs->info.local_size[2]);
    if (render_tracing())
        for (int i = 0; i < 16; i++)
            if (st.storage[SHADER_COMPUTE][i].address)
                fprintf(stderr, "render:   storage %d: %#llx + %#llx\n", i, (unsigned long long)st.storage[SHADER_COMPUTE][i].address,
                        (unsigned long long)st.storage[SHADER_COMPUTE][i].size);
    render_end_rendering();                          /* (no dispatches while rendering) */
    VkCommandBuffer cmd = render_cmd();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, render_compute_layout(), 0, 1, &set, RENDER_DYNAMIC_CBUFS, dyn);
    if (indirect) {
        VkBuffer b;
        VkDeviceSize off;
        if (render_guest_buffer(r->a[0], &b, &off) != 0) { warn_once("indirect dispatch outside a pool", r->a[0]); return; }
        vkCmdDispatchIndirect(cmd, b, off);
    } else {
        vkCmdDispatch(cmd, (uint32_t)r->a[0], (uint32_t)r->a[1], (uint32_t)r->a[2]);
    }
    render_barrier(cmd);                             /* its writes, before whatever comes next */
    stats.dispatches++;
    render_count_work();
}

void render_stats(void) {
    fprintf(stderr, "render: draws %llu done, skipped: %llu without shaders, %llu untranslated shaders, %llu other\n",
            (unsigned long long)stats.done, (unsigned long long)stats.no_shader,
            (unsigned long long)stats.failed_shader, (unsigned long long)stats.other);
    if (stats.dispatches) fprintf(stderr, "render: %llu compute dispatches\n", (unsigned long long)stats.dispatches);
    render_collect_compile_stats();
    if (render_compiles) {                           /* new pipelines: keep them for next time */
        fprintf(stderr, "render: %u new pipelines, %.0f ms creating shaders and pipelines\n", render_compiles, render_compile_ms);
        vk_save_pipeline_cache();
    }
    render_compiles = 0;
    render_compile_ms = 0;
    memset(&stats, 0, sizeof stats);
}

/* LAA_TRACE_SYNC: where a slow command's time went (exec.c prints it): shaders, resources
 * (descriptors, textures), render pass begin, pipeline */
int render_prof_on;
double render_prof[4];
static double prof_clock(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
#define PROF(i, stmt) do { double prof_t0 = render_prof_on ? prof_clock() : 0; stmt; \
                           if (render_prof_on) render_prof[i] += prof_clock() - prof_t0; } while (0)

static unsigned frame_draws;                     /* since the last present (render_take_frame_draws) */
unsigned render_take_frame_draws(void) { unsigned n = frame_draws; frame_draws = 0; return n; }

static void draw(const Cmd *r, int indexed) {
    submit_early();
    frame_draws++;
    const Program *p = &st.program;
    if (!(p->stages >> STAGE_VERTEX & 1) || !(p->stages >> STAGE_FRAGMENT & 1)) {
        warn_once("draw without a vertex and a fragment shader", p->stages);
        stats.no_shader++;
        return;
    }
    HostShader *vs, *fs;
    PROF(0, vs = render_shader(p->code[STAGE_VERTEX], p->control[STAGE_VERTEX], SHADER_VERTEX, 0);
            fs = render_shader(p->code[STAGE_FRAGMENT], p->control[STAGE_FRAGMENT], SHADER_FRAGMENT, 0));
    if (!vs->module || !fs->module) { stats.failed_shader++; return; }
    /* Vulkan wants every input the fragment shader reads to be an output of the vertex shader
     * (the Adreno driver refuses the pipeline otherwise): a vertex shader variant adds them */
    uint32_t missing = fs->info.inputs & ~vs->info.outputs;
    if (missing) {
        PROF(0, vs = render_shader(p->code[STAGE_VERTEX], p->control[STAGE_VERTEX], SHADER_VERTEX, missing));
        if (!vs->module) { stats.failed_shader++; return; }
    }
    stats.other++;                                   /* (until it is done, below) */
    if (r->a[0] == 7 || r->a[0] == 8 || r->a[0] == 9 || r->a[0] == 2) { warn_once("quads/polygons/line loops", r->a[0]); return; }

    if (render_tracing()) {                          /* one line per draw: shaders, targets, storage buffers */
        fprintf(stderr, "render: draw vs %#llx fs %#llx ->", (unsigned long long)vs->code, (unsigned long long)fs->code);
        for (uint32_t i = 0; i < st.rt.count; i++)
            if (st.color[i]) fprintf(stderr, " rt%u %ux%u fmt %d @%#llx", i, st.color[i]->width, st.color[i]->height, st.color[i]->format,
                                     (unsigned long long)st.color[i]->address);
        if (st.depth) fprintf(stderr, " depth %ux%u", st.depth->width, st.depth->height);
        fprintf(stderr, " vp %g,%g %gx%g sc %d,%d %dx%d", st.viewport[0], st.viewport[1], st.viewport[2], st.viewport[3],
                st.scissor[0], st.scissor[1], st.scissor[2], st.scissor[3]);
        fprintf(stderr, " logic %u blend %#x textures vs %u fs %u", st.color_state.logic_op, st.color_state.blend_enable,
                vs->info.ntextures, fs->info.ntextures);
        for (int s = 0; s < 6; s++)
            for (int i = 0; i < 16; i++)
                if (st.storage[s][i].address)
                    fprintf(stderr, " sb%d.%d %#llx+%#llx", s, i, (unsigned long long)st.storage[s][i].address,
                            (unsigned long long)st.storage[s][i].size);
        for (int s = 0; s < 2; s++)                  /* uniform buffers: address, first 4 words as floats */
            for (int i = 0; i < 14; i++)
                if (st.uniform[s][i].address) {
                    float f[4];
                    memcpy(f, gptr(st.uniform[s][i].address), 16);
                    fprintf(stderr, " %cub%d %#llx=%g,%g,%g,%g", s ? 'f' : 'v', i, (unsigned long long)st.uniform[s][i].address,
                            f[0], f[1], f[2], f[3]);
                }
        fprintf(stderr, "\n");
    }

    /* resources first: making a descriptor set may have to submit work (which ends rendering) */
    double prof_t0 = render_prof_on ? prof_clock() : 0;
    VkWriteDescriptorSet writes[2 * 50];
    VkDescriptorBufferInfo buffers[2 * 18];
    VkDescriptorImageInfo images[2 * SHADER_MAX_TEXTURES];
    uint32_t nw = 0, nb = 0, ni = 0, dyn[2 * RENDER_DYNAMIC_CBUFS] = { 0 };   /* (in binding order: vertex, fragment) */
    fill_stage(vs, SHADER_VERTEX, writes, &nw, buffers, &nb, images, &ni, dyn);
    fill_stage(fs, SHADER_FRAGMENT, writes, &nw, buffers, &nb, images, &ni, dyn + RENDER_DYNAMIC_CBUFS);
    VkDescriptorSet set = get_set(render_set_layout(), writes, nw);
    if (render_prof_on) render_prof[1] += prof_clock() - prof_t0;
    int began;
    PROF(2, began = begin_rendering());
    if (began != 0) return;

    PipelineKey k;
    memset(&k, 0, sizeof k);
    k.vs = vs->module;
    k.fs = fs->module;
    k.vs_code = vs->code;
    k.fs_code = fs->code;
    k.vs_inputs = vs->info.inputs;
    k.nattribs = (uint8_t)st.nattribs;
    for (uint32_t i = 0; i < st.nattribs; i++) {
        k.attr[i].format = (uint8_t)st.attrib[i].format;
        k.attr[i].stream = (uint8_t)st.attrib[i].stream;
        k.attr[i].offset = (uint16_t)st.attrib[i].offset;
        k.stream[st.attrib[i].stream].stride = st.stream[st.attrib[i].stream].stride;
        k.stream[st.attrib[i].stream].divisor = st.stream[st.attrib[i].stream].divisor;
    }
    k.topology = (uint8_t)r->a[0];
    k.primitive_restart = (uint8_t)st.primitive_restart;
    k.cull = st.polygon.cull_face;
    k.front_ccw = st.polygon.front_face;
    k.depth_bias = (st.polygon.offset_enables & 4) != 0;
    k.alpha_to_coverage = st.multisample.alpha_to_coverage;
    k.depth_test = st.depth && st.depth_stencil.depth_test;
    k.depth_write = st.depth && st.depth_stencil.depth_write;
    k.depth_func = st.depth_stencil.depth_func;
    k.stencil_test = st.depth && st.depth_stencil.stencil_test && (st.depth->aspects & VK_IMAGE_ASPECT_STENCIL_BIT);
    memcpy(k.stencil_func, st.depth_stencil.stencil_func, 2);
    memcpy(k.stencil_ops, st.depth_stencil.stencil_ops, 4);
    k.blend_enable = st.color_state.blend_enable;
    k.logic_op = st.color_state.logic_op;
    k.channel_mask = st.channel_mask;
    k.ncolor = (uint8_t)st.rt.count;
    for (uint32_t i = 0; i < st.rt.count; i++) {
        k.color_format[i] = st.color[i] ? st.color[i]->format : VK_FORMAT_UNDEFINED;
        k.blend[i] = (BlendKey){ st.blend[i].src_color, st.blend[i].dst_color, st.blend[i].src_alpha,
                                 st.blend[i].dst_alpha, st.blend[i].eq_color, st.blend[i].eq_alpha };
    }
    k.depth_format = st.depth ? st.depth->format : VK_FORMAT_UNDEFINED;
    VkPipeline pipeline;
    PROF(3, pipeline = render_pipeline(&k));
    if (!pipeline) return;

    VkCommandBuffer cmd = render_cmd();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, render_pipeline_layout(), 0, 1, &set, 2 * RENDER_DYNAMIC_CBUFS, dyn);
    VkBuffer vb[16];                                 /* the streams used, bound in runs of neighbours */
    VkDeviceSize voff[16];
    uint32_t used = 0;
    for (uint32_t i = 0; i < st.nattribs; i++) {
        if (!(vs->info.inputs >> i & 1) || !st.attrib[i].format) continue;
        uint32_t s = st.attrib[i].stream;
        if (used >> s & 1) continue;
        if (render_guest_buffer(st.vertex_buffer[s].address, &vb[s], &voff[s]) != 0) {
            warn_once("vertex buffer not in a memory pool", st.vertex_buffer[s].address);
            return;
        }
        used |= 1u << s;
    }
    for (uint32_t s = 0; s < 16; s++) {
        if (!(used >> s & 1)) continue;
        uint32_t n = 1;
        while (s + n < 16 && (used >> (s + n) & 1)) n++;
        vkCmdBindVertexBuffers(cmd, s, n, &vb[s], &voff[s]);
        s += n - 1;
    }
    VkViewport vp = { st.viewport[0], st.viewport[1], st.viewport[2], st.viewport[3], st.depth_range[0], st.depth_range[1] };
    if (vp.width <= 0) vp.width = 1;
    if (vp.height == 0) vp.height = 1;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc = { { st.scissor[0] > 0 ? st.scissor[0] : 0, st.scissor[1] > 0 ? st.scissor[1] : 0 },
                    { (uint32_t)(st.scissor[2] > 0 ? st.scissor[2] : 0), (uint32_t)(st.scissor[3] > 0 ? st.scissor[3] : 0) } };
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdSetBlendConstants(cmd, st.blend_color);
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, st.stencil_ref[0]);
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, st.stencil_ref[1]);
    vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_BIT, st.stencil_value_mask[0]);
    vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_BACK_BIT, st.stencil_value_mask[1]);
    vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_BIT, st.stencil_mask[0]);
    vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_BACK_BIT, st.stencil_mask[1]);
    vkCmdSetDepthBias(cmd, st.polygon_offset[1], st.polygon_offset[2], st.polygon_offset[0]);
    vkCmdSetLineWidth(cmd, 1.0f);
    if (indexed) {
        static const VkIndexType types[3] = { VK_INDEX_TYPE_UINT8_EXT, VK_INDEX_TYPE_UINT16, VK_INDEX_TYPE_UINT32 };
        if (r->a[1] == 0) { warn_once("8-bit indices", 0); return; }
        VkBuffer b;
        VkDeviceSize off;
        if (render_guest_buffer(r->a[3], &b, &off) != 0) { warn_once("index buffer not in a memory pool", r->a[3]); return; }
        vkCmdBindIndexBuffer(cmd, b, off, types[r->a[1] < 3 ? r->a[1] : 2]);
        vkCmdDrawIndexed(cmd, (uint32_t)r->a[2], (uint32_t)r->a[6], 0, (int32_t)r->a[4], (uint32_t)r->a[5]);
    } else {
        vkCmdDraw(cmd, (uint32_t)r->a[2], (uint32_t)r->a[4], (uint32_t)r->a[1], (uint32_t)r->a[3]);
    }
    stats.other--;
    stats.done++;
    render_count_work();
    for (uint32_t i = 0; i < st.rt.count; i++) if (st.color[i]) render_image_gpu_wrote(st.color[i]);
    if (st.depth) render_image_gpu_wrote(st.depth);
    /* LAA_DUMP_DRAWS_AT=frame (+ LAA_DUMP_DIR): after each pass of that frame that isn't scene
     * geometry (no depth buffer, or smaller than full screen; at least 200 wide), save its render target */
    static int64_t dump_draws = -2;
    static int draw_no;
    if (dump_draws == -2) dump_draws = getenv("LAA_DUMP_DRAWS_AT") ? strtoll(getenv("LAA_DUMP_DRAWS_AT"), NULL, 0) : -1;
    if (dump_draws >= 0 && nvn_frame_count() == (uint64_t)dump_draws && st.color[0] && st.color[0]->width >= 200 &&
        (!st.depth || st.color[0]->width < 1920)) {
        char path[512];
        snprintf(path, sizeof path, "%s/draw%03d_fs%llx_%ux%u.png", getenv("LAA_DUMP_DIR") ? getenv("LAA_DUMP_DIR") : ".", draw_no++,
                 (unsigned long long)fs->code, st.color[0]->width, st.color[0]->height);
        render_debug_dump(st.color[0], path);
    }
    static int each = -1;                            /* LAA_FLUSH_EACH_DRAW: find which draw breaks the GPU */
    if (each < 0) each = getenv("LAA_FLUSH_EACH_DRAW") != NULL;
    if (each) { render_flush(); fprintf(stderr, "render: draw done\n"); }
}

/* array binds: (first, count, values) -> a table */
static void bind_ranges(Range *table, uint32_t max, uint64_t first, uint64_t count, const uint8_t *extra) {
    for (uint64_t i = 0; i < count && first + i < max; i++) memcpy(&table[first + i], extra + 16 * i, 16);
}

static void set_value(const Cmd *r) {
    switch (r->sub) {
    case VALUE_BLEND_COLOR: memcpy(st.blend_color, r->extra, 16); break;
    /* (faces, value): face bit 1 front, 2 back */
    case VALUE_STENCIL_REF:        for (int f = 0; f < 2; f++) if (r->a[0] >> f & 1) st.stencil_ref[f] = (uint32_t)r->a[1]; break;
    case VALUE_STENCIL_MASK:       for (int f = 0; f < 2; f++) if (r->a[0] >> f & 1) st.stencil_mask[f] = (uint32_t)r->a[1]; break;
    case VALUE_STENCIL_VALUE_MASK: for (int f = 0; f < 2; f++) if (r->a[0] >> f & 1) st.stencil_value_mask[f] = (uint32_t)r->a[1]; break;
    case VALUE_PRIMITIVE_RESTART: st.primitive_restart = (uint32_t)r->a[0] & 1; st.restart_index = (uint32_t)r->a[1]; break;
    case VALUE_POLYGON_OFFSET_CLAMP: memcpy(st.polygon_offset, r->f, 12); break;   /* factor, units, clamp */
    case VALUE_LINE_WIDTH: st.line_width = r->f[0]; break;
    case VALUE_COPY_ROW_STRIDE: st.copy_row_stride = r->a[0]; break;
    case VALUE_COPY_IMAGE_STRIDE: st.copy_image_stride = r->a[0]; break;
    default: break;
    }
}

void render_command(const Cmd *r) {
    switch (r->op) {
    case CMD_BIND_PROGRAM: memcpy(&st.program, r->extra, sizeof st.program); break;
    case CMD_BIND_STATE:
        switch (r->sub) {
        case STATE_BLEND: { BlendState b; memcpy(&b, r->extra, sizeof b); st.blend[b.target] = b; break; }
        case STATE_CHANNEL_MASK: memcpy(&st.channel_mask, r->extra, 4); break;
        case STATE_COLOR: memcpy(&st.color_state, r->extra, sizeof st.color_state); break;
        case STATE_DEPTH_STENCIL: memcpy(&st.depth_stencil, r->extra, sizeof st.depth_stencil); break;
        case STATE_MULTISAMPLE: memcpy(&st.multisample, r->extra, sizeof st.multisample); break;
        case STATE_POLYGON: memcpy(&st.polygon, r->extra, sizeof st.polygon); break;
        }
        break;
    case CMD_BIND_VERTEX_ATTRIBS:                         /* a = count */
        st.nattribs = r->a[0] < 16 ? (uint32_t)r->a[0] : 16;
        memcpy(st.attrib, r->extra, st.nattribs * sizeof *st.attrib);
        break;
    case CMD_BIND_VERTEX_STREAMS: {
        uint32_t n = r->a[0] < 16 ? (uint32_t)r->a[0] : 16;
        memcpy(st.stream, r->extra, n * sizeof *st.stream);
        break;
    }
    case CMD_BIND_VERTEX_BUFFERS: bind_ranges(st.vertex_buffer, 16, r->a[0], r->a[1], r->extra); break;
    case CMD_BIND_UNIFORM_BUFFERS:                        /* a = stage, first, count */
        if (r->a[0] < 6) bind_ranges(st.uniform[r->a[0]], 14, r->a[1], r->a[2], r->extra);
        break;
    case CMD_BIND_STORAGE_BUFFERS:
        if (r->a[0] < 6) bind_ranges(st.storage[r->a[0]], 16, r->a[1], r->a[2], r->extra);
        break;
    case CMD_BIND_TEXTURES:
        if (r->a[0] < 6)
            for (uint64_t i = 0; i < r->a[2] && r->a[1] + i < 32; i++) memcpy(&st.texture[r->a[0]][r->a[1] + i], r->extra + 8 * i, 8);
        break;
    case CMD_SET_TEXTURE_POOL: memcpy(&st.texture_pool, r->extra, sizeof st.texture_pool); break;
    case CMD_SET_SAMPLER_POOL: memcpy(&st.sampler_pool, r->extra, sizeof st.sampler_pool); break;
    case CMD_SET_VIEWPORTS: if (r->a[0] == 0 && r->a[1] >= 1) memcpy(st.viewport, r->extra, 16); break;
    case CMD_SET_SCISSORS: if (r->a[0] == 0 && r->a[1] >= 1) memcpy(st.scissor, r->extra, 16); break;
    case CMD_SET_DEPTH_RANGES: if (r->a[0] == 0 && r->a[1] >= 1) memcpy(st.depth_range, r->extra, 8); break;
    case CMD_SET_VALUE: set_value(r); break;
    case CMD_DRAW_ARRAYS: draw(r, 0); break;
    case CMD_DISPATCH: dispatch(r, 0); break;
    case CMD_COPY_BUFFER_TO_BUFFER: copy_buffer(r->a[0], r->a[1], r->a[2]); break;   /* a = src, dst, size */
    case CMD_UPDATE_UNIFORM_BUFFER: update_buffer(r->a[0] + r->a[2], r->extra, r->a[3]); break;
    case CMD_CLEAR_BUFFER: fill_buffer(r->a[0], r->a[1], (uint32_t)r->a[2]); break;
    case CMD_DISPATCH_INDIRECT: dispatch(r, 1); break;
    case CMD_DRAW_ELEMENTS: draw(r, 1); break;
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
    case CMD_COPY_BUFFER_TO_TEXTURE: {             /* a = src address; extra = dst */
        ImageRef dst;
        memcpy(&dst, r->extra, sizeof dst);
        copy_buffer_image(r->a[0], &dst, 0);
        break;
    }
    case CMD_COPY_TEXTURE_TO_BUFFER: {             /* a = -, -, -, dst address; extra = src */
        ImageRef src;
        memcpy(&src, r->extra, sizeof src);
        copy_buffer_image(r->a[3], &src, 1);
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
    if (render_gpu_timing) {                       /* LAA_GPU_TIMING_AT: label the command just recorded */
        char label[48] = "";
        switch (r->op) {
        case CMD_DRAW_ARRAYS: case CMD_DRAW_ELEMENTS:
            snprintf(label, sizeof label, "draw fs %#llx%s", (unsigned long long)st.program.code[STAGE_FRAGMENT],
                     st.pass_draws++ ? "" : " [new pass]");
            break;
        case CMD_DISPATCH: case CMD_DISPATCH_INDIRECT:
            snprintf(label, sizeof label, "dispatch cs %#llx", (unsigned long long)st.program.code[STAGE_COMPUTE]); break;
        case CMD_COPY_TEXTURE_TO_TEXTURE: {
            const ImageRef *refs = (const ImageRef *)r->extra;
            HostImage *s = render_image_find(refs[0].texture), *d = render_image_find(refs[1].texture);
            snprintf(label, sizeof label, "copy %ux%u fmt %d -> %ux%u fmt %d", s ? s->width : 0, s ? s->height : 0,
                     s ? s->format : 0, d ? d->width : 0, d ? d->height : 0, d ? d->format : 0);
            break;
        }
        case CMD_CLEAR_COLOR: snprintf(label, sizeof label, "clear color"); break;
        case CMD_CLEAR_DEPTH_STENCIL: snprintf(label, sizeof label, "clear depth/stencil"); break;
        case CMD_CLEAR_TEXTURE: snprintf(label, sizeof label, "clear texture"); break;
        case CMD_COPY_BUFFER_TO_TEXTURE: snprintf(label, sizeof label, "buffer -> texture"); break;
        case CMD_COPY_TEXTURE_TO_BUFFER: snprintf(label, sizeof label, "texture -> buffer"); break;
        case CMD_COPY_BUFFER_TO_BUFFER: case CMD_UPDATE_UNIFORM_BUFFER: case CMD_CLEAR_BUFFER:
            snprintf(label, sizeof label, "buffer write"); break;
        }
        if (label[0]) render_timestamp(label);
    }
}

void render_submitted(void) { render_submit(); }

void render_guest_write(uint64_t address, const void *data, uint32_t size) { update_buffer(address, data, size); }
