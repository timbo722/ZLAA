/*
 * ui_render.cpp - RmlUi's drawing, on our Vulkan device (RmlUi asks; we draw).
 *
 * RmlUi turns the menu's documents into triangles: each box, border and line of text is "geometry"
 * (vertices with a position in pixels, a premultiplied color and a texture coordinate) that it
 * compiles once and draws whenever the screen changes, with a texture (a font's glyph atlas, an
 * image) or none. This file keeps each compiled geometry in a GPU-visible buffer and each texture in
 * a VkImage, and records the draws into a command buffer of its own, inside a render pass on one of
 * the window's images (render_ui_begin/end in render.c). One pipeline does everything: color x
 * texture, premultiplied alpha blending; untextured geometry samples a 1x1 white texture.
 *
 * What RmlUi can do beyond that (clip masks for rounded overflow, layers for filters, blur and
 * box-shadow, shaders for CSS gradients) isn't implemented: the menu's style sheets don't use it.
 */
#include "ui_internal.h"
#include "ui.h"
#include "render/shader/builtin/builtin_shaders.h"
extern "C" {
#include "render/png.h"
}
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
#include <vector>

namespace {

struct Geometry {
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceSize index_offset;
    uint32_t index_count;
};

struct UiTexture {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkDescriptorSet set;
};

#define UI_FRAMES 2                  /* frames recorded while the GPU may still run the one before */

struct PushConstants { float transform[16]; float translation[2]; };

}

class UiRenderer : public Rml::RenderInterface {
public:
    int init();
    void begin_frame(const RenderUiTarget &t, int clear);
    void end_frame(const char *shot);
    void draw_image(VkImageView view, float dim);    /* a window-sized image, darkened (the paused game) */

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;
    Rml::TextureHandle LoadTexture(Rml::Vector2i &dimensions, const Rml::String &source) override;
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture) override;
    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(Rml::Rectanglei region) override;
    void SetTransform(const Rml::Matrix4f *transform) override;

private:
    VkRenderPass pass_clear = VK_NULL_HANDLE, pass_load = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmds[UI_FRAMES], upload_cmd = VK_NULL_HANDLE;
    VkFence fences[UI_FRAMES], upload_fence = VK_NULL_HANDLE;
    VkFramebuffer framebuffers[UI_FRAMES] = {};
    std::vector<Geometry *> dead_geometry[UI_FRAMES];   /* released while a frame may still draw them */
    std::vector<UiTexture *> dead_textures[UI_FRAMES];
    UiTexture *white = nullptr;
    int frame = 0;
    VkCommandBuffer cmd = VK_NULL_HANDLE;              /* the frame being recorded */
    uint32_t width = 0, height = 0;
    VkImage target = VK_NULL_HANDLE;
    bool scissor_on = false;
    Rml::Rectanglei scissor_rect;
    Rml::Matrix4f projection, current;

    void set_scissor();
    void free_dead(int f);
};

static UiRenderer renderer;
Rml::RenderInterface *ui_render_interface(void) { return &renderer; }
int ui_render_init(void) { return renderer.init(); }
void ui_render_begin(const RenderUiTarget *t, int clear) { renderer.begin_frame(*t, clear); }
void ui_render_end(const char *shot) { renderer.end_frame(shot); }
void ui_render_image(VkImageView view, float dim) { renderer.draw_image(view, dim); }

static VkShaderModule make_module(const uint32_t *code, size_t size) {
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = size;
    ci.pCode = code;
    VkShaderModule m;
    VK_CHECK(vkCreateShaderModule(vk.device, &ci, NULL, &m));
    return m;
}

static VkRenderPass make_pass(VkAttachmentLoadOp load) {
    VkAttachmentDescription a = {};
    a.format = render_ui_format();
    a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = load;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_GENERAL;       /* (the window images stay GENERAL: render.c) */
    a.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_GENERAL };
    VkSubpassDescription sub = {};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkRenderPassCreateInfo ci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    ci.attachmentCount = 1;
    ci.pAttachments = &a;
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    VkRenderPass p;
    VK_CHECK(vkCreateRenderPass(vk.device, &ci, NULL, &p));
    return p;
}

int UiRenderer::init() {
    pass_clear = make_pass(VK_ATTACHMENT_LOAD_OP_CLEAR);
    pass_load = make_pass(VK_ATTACHMENT_LOAD_OP_LOAD);

    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0;
    VK_CHECK(vkCreateSampler(vk.device, &si, NULL, &sampler));

    VkDescriptorSetLayoutBinding b = {};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = 1;
    li.pBindings = &b;
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &li, NULL, &set_layout));
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024 };
    VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = 1024;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    VK_CHECK(vkCreateDescriptorPool(vk.device, &pi, NULL, &descriptor_pool));

    VkPushConstantRange range = { VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstants) };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &range;
    VK_CHECK(vkCreatePipelineLayout(vk.device, &pli, NULL, &layout));

    VkShaderModule vs = make_module(spv_ui_vert, sizeof spv_ui_vert), fs = make_module(spv_ui_frag, sizeof spv_ui_frag);
    VkPipelineShaderStageCreateInfo stages[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
                                                  { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";
    VkVertexInputBindingDescription vb = { 0, sizeof(Rml::Vertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[3] = {
        { 0, 0, VK_FORMAT_R32G32_SFLOAT, (uint32_t)offsetof(Rml::Vertex, position) },
        { 1, 0, VK_FORMAT_R8G8B8A8_UNORM, (uint32_t)offsetof(Rml::Vertex, colour) },
        { 2, 0, VK_FORMAT_R32G32_SFLOAT, (uint32_t)offsetof(Rml::Vertex, tex_coord) },
    };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 3;
    vi.pVertexAttributeDescriptions = va;
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend = {};  /* premultiplied alpha: out = src + dst * (1 - src.a) */
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstColorBlendFactor = blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gi.stageCount = 2;
    gi.pStages = stages;
    gi.pVertexInputState = &vi;
    gi.pInputAssemblyState = &ia;
    gi.pViewportState = &vp;
    gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms;
    gi.pColorBlendState = &cb;
    gi.pDynamicState = &ds;
    gi.layout = layout;
    gi.renderPass = pass_clear;                      /* (compatible with pass_load: only the load op differs) */
    VK_CHECK(vkCreateGraphicsPipelines(vk.device, vk.pipeline_cache, 1, &gi, NULL, &pipeline));
    vkDestroyShaderModule(vk.device, vs, NULL);
    vkDestroyShaderModule(vk.device, fs, NULL);

    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = vk.queue_family;
    VK_CHECK(vkCreateCommandPool(vk.device, &cpi, NULL, &pool));
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = UI_FRAMES;
    VK_CHECK(vkAllocateCommandBuffers(vk.device, &ai, cmds));
    ai.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(vk.device, &ai, &upload_cmd));
    VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VK_CHECK(vkCreateFence(vk.device, &fi, NULL, &upload_fence));
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (int i = 0; i < UI_FRAMES; i++) VK_CHECK(vkCreateFence(vk.device, &fi, NULL, &fences[i]));

    static const Rml::byte white_pixel[4] = { 255, 255, 255, 255 };
    white = (UiTexture *)GenerateTexture(Rml::Span<const Rml::byte>(white_pixel, 4), Rml::Vector2i(1, 1));
    return white ? 0 : -1;
}

void UiRenderer::free_dead(int f) {
    for (Geometry *g : dead_geometry[f]) {
        vkDestroyBuffer(vk.device, g->buffer, NULL);
        vkFreeMemory(vk.device, g->memory, NULL);
        delete g;
    }
    dead_geometry[f].clear();
    for (UiTexture *t : dead_textures[f]) {
        vkFreeDescriptorSets(vk.device, descriptor_pool, 1, &t->set);
        vkDestroyImageView(vk.device, t->view, NULL);
        vkDestroyImage(vk.device, t->image, NULL);
        vkFreeMemory(vk.device, t->memory, NULL);
        delete t;
    }
    dead_textures[f].clear();
}

void UiRenderer::begin_frame(const RenderUiTarget &t, int clear) {
    frame = (frame + 1) % UI_FRAMES;
    VK_CHECK(vkWaitForFences(vk.device, 1, &fences[frame], VK_TRUE, UINT64_MAX));
    VK_CHECK(vkResetFences(vk.device, 1, &fences[frame]));
    free_dead(frame);
    if (framebuffers[frame]) vkDestroyFramebuffer(vk.device, framebuffers[frame], NULL);
    width = t.width;
    height = t.height;
    target = t.image;
    VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fi.renderPass = pass_clear;
    fi.attachmentCount = 1;
    fi.pAttachments = &t.view;
    fi.width = width;
    fi.height = height;
    fi.layers = 1;
    VK_CHECK(vkCreateFramebuffer(vk.device, &fi, NULL, &framebuffers[frame]));

    cmd = cmds[frame];
    VK_CHECK(vkResetCommandBuffer(cmd, 0));
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };   /* (after whatever last used the image) */
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    VkClearValue cv = {};
    cv.color.float32[3] = 1;
    VkRenderPassBeginInfo rp = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rp.renderPass = clear ? pass_clear : pass_load;
    rp.framebuffer = framebuffers[frame];
    rp.renderArea.extent = { width, height };
    rp.clearValueCount = 1;
    rp.pClearValues = &cv;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkViewport v = { 0, 0, (float)width, (float)height, 0, 1 };
    vkCmdSetViewport(cmd, 0, 1, &v);
    /* pixels -> clip space: x 0..width -> -1..1, y 0..height (down) -> -1..1 (Vulkan's y points down too) */
    projection = Rml::Matrix4f::ProjectOrtho(0, (float)width, 0, (float)height, -10000, 10000);
    current = projection;
    scissor_on = false;
    set_scissor();
}

/* shot: also save the frame as a PNG there (LAA_UI_SCRIPT; waits for the GPU) */
void UiRenderer::end_frame(const char *shot) {
    vkCmdEndRenderPass(cmd);
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (shot) {
        VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bi.size = (VkDeviceSize)width * height * 4;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VK_CHECK(vkCreateBuffer(vk.device, &bi, NULL, &buffer));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(vk.device, buffer, &req);
        VkMemoryAllocateInfo mi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mi.allocationSize = req.size;
        mi.memoryTypeIndex = vk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK_CHECK(vkAllocateMemory(vk.device, &mi, NULL, &memory));
        VK_CHECK(vkBindBufferMemory(vk.device, buffer, memory, 0));
        VkBufferImageCopy copy = {};
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageExtent = { width, height, 1 };
        vkCmdCopyImageToBuffer(cmd, target, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &copy);
    }
    VK_CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    pthread_mutex_lock(&render_queue_lock);
    VK_CHECK(vkQueueSubmit(vk.queue, 1, &si, fences[frame]));
    pthread_mutex_unlock(&render_queue_lock);
    cmd = VK_NULL_HANDLE;
    if (shot) {
        VK_CHECK(vkWaitForFences(vk.device, 1, &fences[frame], VK_TRUE, UINT64_MAX));
        uint8_t *px;
        VK_CHECK(vkMapMemory(vk.device, memory, 0, VK_WHOLE_SIZE, 0, (void **)&px));
        int bgra = vk.swapchain_format == VK_FORMAT_B8G8R8A8_SRGB || vk.swapchain_format == VK_FORMAT_B8G8R8A8_UNORM;
        for (size_t i = 0; i < (size_t)width * height; i++) {
            if (bgra) { uint8_t b = px[i * 4]; px[i * 4] = px[i * 4 + 2]; px[i * 4 + 2] = b; }
            px[i * 4 + 3] = 255;
        }
        if (png_write(shot, px, width, height, (int32_t)width * 4) == 0) fprintf(stderr, "ui: wrote %s\n", shot);
        vkUnmapMemory(vk.device, memory);
        vkDestroyBuffer(vk.device, buffer, NULL);
        vkFreeMemory(vk.device, memory, NULL);
    }
}

void UiRenderer::set_scissor() {
    VkRect2D r = { { 0, 0 }, { width, height } };
    if (scissor_on) {
        int x0 = scissor_rect.Left() < 0 ? 0 : scissor_rect.Left(), y0 = scissor_rect.Top() < 0 ? 0 : scissor_rect.Top();
        int x1 = scissor_rect.Right() > (int)width ? (int)width : scissor_rect.Right();
        int y1 = scissor_rect.Bottom() > (int)height ? (int)height : scissor_rect.Bottom();
        r.offset = { x0, y0 };
        r.extent = { (uint32_t)(x1 > x0 ? x1 - x0 : 0), (uint32_t)(y1 > y0 ? y1 - y0 : 0) };
    }
    vkCmdSetScissor(cmd, 0, 1, &r);
}

void UiRenderer::EnableScissorRegion(bool enable) {
    scissor_on = enable;
    if (cmd) set_scissor();
}

void UiRenderer::SetScissorRegion(Rml::Rectanglei region) {
    scissor_rect = region;
    if (cmd) set_scissor();
}

void UiRenderer::SetTransform(const Rml::Matrix4f *transform) {
    current = transform ? projection * *transform : projection;
}

Rml::CompiledGeometryHandle UiRenderer::CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) {
    VkDeviceSize vsize = vertices.size() * sizeof(Rml::Vertex), isize = indices.size() * sizeof(int);
    Geometry *g = new Geometry();
    g->index_offset = (vsize + 15) & ~(VkDeviceSize)15;
    g->index_count = (uint32_t)indices.size();
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = g->index_offset + isize;
    bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    VK_CHECK(vkCreateBuffer(vk.device, &bi, NULL, &g->buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(vk.device, g->buffer, &req);
    VkMemoryAllocateInfo mi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mi.allocationSize = req.size;
    mi.memoryTypeIndex = vk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(vk.device, &mi, NULL, &g->memory));
    VK_CHECK(vkBindBufferMemory(vk.device, g->buffer, g->memory, 0));
    void *p;
    VK_CHECK(vkMapMemory(vk.device, g->memory, 0, VK_WHOLE_SIZE, 0, &p));
    memcpy(p, vertices.data(), vsize);
    memcpy((char *)p + g->index_offset, indices.data(), isize);
    vkUnmapMemory(vk.device, g->memory);
    return (Rml::CompiledGeometryHandle)g;
}

void UiRenderer::RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation, Rml::TextureHandle texture) {
    Geometry *g = (Geometry *)handle;
    UiTexture *t = texture ? (UiTexture *)texture : white;
    if (!cmd || !g->index_count) return;
    PushConstants pc;
    memcpy(pc.transform, current.data(), sizeof pc.transform);
    pc.translation[0] = translation.x;
    pc.translation[1] = translation.y;
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof pc, &pc);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &t->set, 0, NULL);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &g->buffer, &zero);
    vkCmdBindIndexBuffer(cmd, g->buffer, g->index_offset, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, g->index_count, 1, 0, 0, 0);
}

void UiRenderer::ReleaseGeometry(Rml::CompiledGeometryHandle handle) {
    dead_geometry[frame].push_back((Geometry *)handle);
}

/* The paused game's last frame behind the menu: a quad over the whole window, darkened */
void UiRenderer::draw_image(VkImageView view, float dim) {
    UiTexture t = {};
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = descriptor_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &set_layout;
    static VkDescriptorSet sets[UI_FRAMES];           /* (one per frame, rewritten: the view may change) */
    if (!sets[frame]) VK_CHECK(vkAllocateDescriptorSets(vk.device, &ai, &sets[frame]));
    VkDescriptorImageInfo ii = { sampler, view, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = sets[frame];
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(vk.device, 1, &w, 0, NULL);
    t.set = sets[frame];
    static Rml::CompiledGeometryHandle quad[UI_FRAMES];
    if (quad[frame]) ReleaseGeometry(quad[frame]);
    Rml::byte c = (Rml::byte)(255 * (1 - dim));
    Rml::Vertex v[4];
    for (int i = 0; i < 4; i++) {
        v[i].position = Rml::Vector2f(i & 1 ? (float)width : 0, i & 2 ? (float)height : 0);
        v[i].colour = Rml::ColourbPremultiplied(c, c, c, 255);
        v[i].tex_coord = Rml::Vector2f(i & 1 ? 1.f : 0.f, i & 2 ? 1.f : 0.f);
    }
    static const int idx[6] = { 0, 1, 2, 2, 1, 3 };
    quad[frame] = CompileGeometry(Rml::Span<const Rml::Vertex>(v, 4), Rml::Span<const int>(idx, 6));
    RenderGeometry(quad[frame], Rml::Vector2f(0, 0), (Rml::TextureHandle)&t);
}

/* Images named in documents (<img src>, image decorators): PNG files among the menu's own (SDL's loader), as RGBA,
 * premultiplied; "game-logo" is made from the player's game files */
Rml::TextureHandle UiRenderer::LoadTexture(Rml::Vector2i &dimensions, const Rml::String &source) {
    dimensions = Rml::Vector2i(0, 0);
    if (source.find("game-logo") != Rml::String::npos) {   /* (not a file: the game's own logo, game_art.cpp) */
        std::vector<Rml::byte> px;
        int w, h;
        if (!game_logo_rgba(ui_game_romfs(), px, w, h)) return 0;
        dimensions = Rml::Vector2i(w, h);
        return GenerateTexture(Rml::Span<const Rml::byte>(px.data(), px.size()), dimensions);
    }
    Rml::FileInterface *files = Rml::GetFileInterface();
    Rml::FileHandle f = files->Open(source);
    if (!f) return 0;
    std::vector<Rml::byte> data(files->Length(f));
    size_t got = files->Read(data.data(), data.size(), f);
    files->Close(f);
    SDL_Surface *png = got == data.size() ? SDL_LoadPNG_IO(SDL_IOFromConstMem(data.data(), data.size()), true) : NULL;
    SDL_Surface *rgba = png ? SDL_ConvertSurface(png, SDL_PIXELFORMAT_ABGR8888) : NULL;   /* (R, G, B, A bytes) */
    SDL_DestroySurface(png);
    if (!rgba || !SDL_PremultiplySurfaceAlpha(rgba, false)) {
        fprintf(stderr, "ui: can't load %s: %s\n", source.c_str(), SDL_GetError());
        SDL_DestroySurface(rgba);
        return 0;
    }
    std::vector<Rml::byte> px((size_t)rgba->w * rgba->h * 4);
    for (int y = 0; y < rgba->h; y++)
        memcpy(&px[(size_t)y * rgba->w * 4], (const Rml::byte *)rgba->pixels + (size_t)y * rgba->pitch, (size_t)rgba->w * 4);
    dimensions = Rml::Vector2i(rgba->w, rgba->h);
    SDL_DestroySurface(rgba);
    return GenerateTexture(Rml::Span<const Rml::byte>(px.data(), px.size()), dimensions);
}

/* RGBA8, premultiplied (font atlases, generated decorations): staging buffer -> image, waited for
 * (RmlUi makes these between frames' draws, rarely) */
Rml::TextureHandle UiRenderer::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) {
    uint32_t w = (uint32_t)dimensions.x, h = (uint32_t)dimensions.y;
    UiTexture *t = new UiTexture();
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = { w, h, 1 };
    ii.mipLevels = ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VK_CHECK(vkCreateImage(vk.device, &ii, NULL, &t->image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk.device, t->image, &req);
    VkMemoryAllocateInfo mi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mi.allocationSize = req.size;
    mi.memoryTypeIndex = vk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(vk.device, &mi, NULL, &t->memory));
    VK_CHECK(vkBindImageMemory(vk.device, t->image, t->memory, 0));
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = t->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ii.format;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VK_CHECK(vkCreateImageView(vk.device, &vi, NULL, &t->view));

    VkBuffer staging;
    VkDeviceMemory staging_memory;
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = (VkDeviceSize)w * h * 4;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VK_CHECK(vkCreateBuffer(vk.device, &bi, NULL, &staging));
    vkGetBufferMemoryRequirements(vk.device, staging, &req);
    mi.allocationSize = req.size;
    mi.memoryTypeIndex = vk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(vk.device, &mi, NULL, &staging_memory));
    VK_CHECK(vkBindBufferMemory(vk.device, staging, staging_memory, 0));
    void *p;
    VK_CHECK(vkMapMemory(vk.device, staging_memory, 0, VK_WHOLE_SIZE, 0, &p));
    memcpy(p, source.data(), bi.size);
    vkUnmapMemory(vk.device, staging_memory);

    VK_CHECK(vkResetCommandBuffer(upload_cmd, 0));
    VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(upload_cmd, &cbi));
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t->image;
    b.subresourceRange = vi.subresourceRange;
    vkCmdPipelineBarrier(upload_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkBufferImageCopy copy = {};
    copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.imageExtent = { w, h, 1 };
    vkCmdCopyBufferToImage(upload_cmd, staging, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(upload_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VK_CHECK(vkEndCommandBuffer(upload_cmd));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &upload_cmd;
    pthread_mutex_lock(&render_queue_lock);
    VK_CHECK(vkQueueSubmit(vk.queue, 1, &si, upload_fence));
    pthread_mutex_unlock(&render_queue_lock);
    VK_CHECK(vkWaitForFences(vk.device, 1, &upload_fence, VK_TRUE, UINT64_MAX));
    VK_CHECK(vkResetFences(vk.device, 1, &upload_fence));
    vkDestroyBuffer(vk.device, staging, NULL);
    vkFreeMemory(vk.device, staging_memory, NULL);

    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = descriptor_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &set_layout;
    VK_CHECK(vkAllocateDescriptorSets(vk.device, &ai, &t->set));
    VkDescriptorImageInfo di = { sampler, t->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    wr.dstSet = t->set;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &di;
    vkUpdateDescriptorSets(vk.device, 1, &wr, 0, NULL);
    return (Rml::TextureHandle)t;
}

void UiRenderer::ReleaseTexture(Rml::TextureHandle texture) {
    dead_textures[frame].push_back((UiTexture *)texture);
}
