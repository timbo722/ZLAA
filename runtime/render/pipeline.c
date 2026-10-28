/*
 * pipeline.c - translated shaders, and Vulkan pipelines for the game's draw state.
 *
 * A Vulkan pipeline bakes in most of what NVN sets with separate state objects (shaders,
 * vertex layout, blending, depth test, ...). We collect that state into a PipelineKey and keep
 * one pipeline per distinct key. Viewports, scissors, blend color, stencil reference and
 * masks, depth bias and line width stay "dynamic" (set per draw) so they don't multiply the
 * number of pipelines.
 */
#include "render_internal.h"
#include "render/shader/shader.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>

/* ================================================================ shaders */

#define SHADER_BUCKETS 1024
static HostShader *shader_buckets[SHADER_BUCKETS];

/* LAA_DEBUG_VS=address (+ LAA_DUMP_DRAWS_AT=frame): that vertex shader also stores its position and
 * attribute 1 per vertex in a buffer, printed at that frame (render_capture_print) */
static VkBuffer capture_buffer;
static float *capture_ptr;

static void make_capture_buffer(void) {
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 256 * 32,
                              .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT };
    VK_CHECK(vkCreateBuffer(vk.device, &bi, NULL, &capture_buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(vk.device, capture_buffer, &req);
    VkMemoryAllocateFlagsInfo flags = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT };
    VkMemoryAllocateInfo mi = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &flags, .allocationSize = req.size,
        .memoryTypeIndex = vk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VkDeviceMemory mem;
    VK_CHECK(vkAllocateMemory(vk.device, &mi, NULL, &mem));
    VK_CHECK(vkBindBufferMemory(vk.device, capture_buffer, mem, 0));
    VK_CHECK(vkMapMemory(vk.device, mem, 0, VK_WHOLE_SIZE, 0, (void **)&capture_ptr));
    for (int i = 0; i < 256 * 8; i++) capture_ptr[i] = -9;
    VkBufferDeviceAddressInfo ai = { .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = capture_buffer };
    shader_capture_address = vkGetBufferDeviceAddress(vk.device, &ai);
}

void render_capture_print(uint64_t frame) {
    static int64_t at = -2;
    if (at == -2) at = getenv("LAA_DUMP_DRAWS_AT") ? strtoll(getenv("LAA_DUMP_DRAWS_AT"), NULL, 0) : -1;
    if (!capture_ptr || frame != (uint64_t)at) return;
    render_flush();
    for (int s = 0; s < 64; s++) {
        const float *f = capture_ptr + 8 * s;
        if (f[0] == -9) continue;
        fprintf(stderr, "render: vs capture instance %d vertex %d: pos %g %g %g %g  attr1 %g %g %g %g\n", s / 4, s % 4,
                f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
    }
}

/* time spent creating shaders and pipelines (render_stats prints it: each one stalls a frame) */
double render_compile_ms;
unsigned render_compiles;
static double compile_clock(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

HostShader *render_shader(uint64_t code, uint64_t control, int stage, uint32_t extra_outputs) {
    unsigned b = (unsigned)((code >> 6) * 0x9E3779B1u >> 22) & (SHADER_BUCKETS - 1);
    for (HostShader *s = shader_buckets[b]; s; s = s->next)
        if (s->code == code && s->stage == stage && s->extra_outputs == extra_outputs) return s;
    double t0 = compile_clock();
    HostShader *s = calloc(1, sizeof *s);
    s->code = code;
    s->stage = stage;
    s->extra_outputs = extra_outputs;
    s->next = shader_buckets[b];
    shader_buckets[b] = s;

    uint64_t storage, size;
    if (!nvn_pool_find(code, &storage, &size)) {
        snprintf(s->info.error, sizeof s->info.error, "code isn't in a memory pool");
    } else {
        uint64_t avail = storage + size - code;
        uint32_t c_off = 0, c_size = 0;
        if (control) shader_constants(gptr(control), &c_off, &c_size);
        if (c_off > avail || c_size > avail - c_off) c_off = c_size = 0;
        const uint8_t *p = gptr(code);
        /* LAA_DEBUG_FS=address: that fragment shader outputs 0.5 (is it drawing at all?) */
        int options = nvn_depth_mode() == 0 ? SHADER_GL_DEPTH : 0;   /* (z -w..w -> 0..w, unless already 0..w) */
        if (stage == SHADER_VERTEX && getenv("LAA_DEBUG_VS") && strtoull(getenv("LAA_DEBUG_VS"), NULL, 0) == code) {
            if (!capture_buffer) make_capture_buffer();
            options |= SHADER_DEBUG_CAPTURE;
        }
        if (getenv("LAA_DEBUG_FS") && strtoull(getenv("LAA_DEBUG_FS"), NULL, 0) == code) {
            options |= SHADER_DEBUG_CONST;
            if (getenv("LAA_DEBUG_FS_INPUT")) options |= ((int)strtol(getenv("LAA_DEBUG_FS_INPUT"), NULL, 0) + 1) << 8;
        }
        if (shader_translate(p, avail, stage, p + c_off, c_size, control ? gptr(control) : NULL, options, extra_outputs, &s->info) == 0) {
            VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                            .codeSize = s->info.words * 4, .pCode = s->info.spirv };
            VK_CHECK(vkCreateShaderModule(vk.device, &ci, NULL, &s->module));
            s->constants = code + c_off;
            s->constants_size = c_size;
        }
    }
    if (!s->module) fprintf(stderr, "render: shader %#llx (stage %d) not translated: %s\n",
                            (unsigned long long)code, stage, s->info.error);
    render_compile_ms += compile_clock() - t0;
    return s;
}

/* ================================================================ layouts */

static VkDescriptorSetLayout set_layout;
static VkPipelineLayout pipeline_layout;

static VkDescriptorSetLayout compute_set_layout;
static VkPipelineLayout compute_layout;

/* A set layout for some stages: per stage, 18 uniform buffers and 32 textures */
static void make_layout(const int *stages, int nstages, VkDescriptorSetLayout *set, VkPipelineLayout *layout) {
    VkDescriptorSetLayoutBinding b[2 * (18 + SHADER_MAX_TEXTURES)];
    uint32_t n = 0;
    for (int s = 0; s < nstages; s++) {
        VkShaderStageFlags flag = stages[s] == SHADER_VERTEX ? VK_SHADER_STAGE_VERTEX_BIT
                                : stages[s] == SHADER_COMPUTE ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
        uint32_t base = (uint32_t)stages[s] * SHADER_STAGE_BINDINGS;
        for (uint32_t i = 0; i < 18; i++)
            b[n++] = (VkDescriptorSetLayoutBinding){ base + SHADER_CBUF_BINDING + i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, flag, NULL };
        for (uint32_t i = 0; i < SHADER_MAX_TEXTURES; i++)
            b[n++] = (VkDescriptorSetLayoutBinding){ base + SHADER_TEXTURE_BINDING + i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, flag, NULL };
    }
    VkDescriptorSetLayoutCreateInfo li = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .bindingCount = n, .pBindings = b };
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &li, NULL, set));
    VkPipelineLayoutCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                      .setLayoutCount = 1, .pSetLayouts = set };
    VK_CHECK(vkCreatePipelineLayout(vk.device, &pi, NULL, layout));
}

/* graphics: one layout for everything (vertex and fragment) */
static void make_layouts(void) {
    static const int graphics[2] = { SHADER_VERTEX, SHADER_FRAGMENT }, compute[1] = { SHADER_COMPUTE };
    make_layout(graphics, 2, &set_layout, &pipeline_layout);
    make_layout(compute, 1, &compute_set_layout, &compute_layout);
}

VkDescriptorSetLayout render_set_layout(void) { if (!set_layout) make_layouts(); return set_layout; }
VkPipelineLayout render_pipeline_layout(void) { if (!set_layout) make_layouts(); return pipeline_layout; }
VkDescriptorSetLayout render_compute_set_layout(void) { if (!set_layout) make_layouts(); return compute_set_layout; }
VkPipelineLayout render_compute_layout(void) { if (!set_layout) make_layouts(); return compute_layout; }

/* A compute shader's pipeline: depends on nothing but the shader */
VkPipeline render_compute_pipeline(HostShader *cs) {
    if (cs->compute || !cs->module) return cs->compute;
    VkComputePipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                   .module = cs->module, .pName = "main" },
        .layout = render_compute_layout() };
    double t0 = compile_clock();
    VK_CHECK(vkCreateComputePipelines(vk.device, vk.pipeline_cache, 1, &ci, NULL, &cs->compute));
    render_compile_ms += compile_clock() - t0;
    render_compiles++;
    return cs->compute;
}

/* ================================================================ NVN enums -> Vulkan */

static VkPrimitiveTopology topology(uint32_t mode) {
    switch (mode) {
    case 0: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case 1: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case 4: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case 5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case 6: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case 10: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
    case 11: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
    case 12: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
    case 13: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
    case 14: return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;          /* (quads etc. need converting: later) */
}

static VkBlendFactor blend_factor(uint32_t f) {
    switch (f) {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 16: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 17: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 18: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 19: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 97: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 98: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 99: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 100: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    }
    return VK_BLEND_FACTOR_ONE;
}

static VkBlendOp blend_op(uint32_t e) {
    static const VkBlendOp ops[6] = { VK_BLEND_OP_ADD, VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT,
                                      VK_BLEND_OP_REVERSE_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX };
    return e < 6 ? ops[e] : VK_BLEND_OP_ADD;
}

/* NVN compare functions are NEVER = 1 ... ALWAYS = 8; Vulkan's are the same order from 0 */
VkCompareOp render_compare_op(uint32_t f) { return f >= 1 && f <= 8 ? (VkCompareOp)(f - 1) : VK_COMPARE_OP_ALWAYS; }

static VkStencilOp stencil_op(uint32_t o) {
    static const VkStencilOp ops[9] = { VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE,
        VK_STENCIL_OP_INCREMENT_AND_CLAMP, VK_STENCIL_OP_DECREMENT_AND_CLAMP, VK_STENCIL_OP_INVERT,
        VK_STENCIL_OP_INCREMENT_AND_WRAP, VK_STENCIL_OP_DECREMENT_AND_WRAP };
    return o < 9 ? ops[o] : VK_STENCIL_OP_KEEP;
}

/* Vertex attribute formats: the color ones, plus NVN's "integer read as float" ones */
static VkFormat vertex_format(uint32_t f) {
    static const VkFormat scaled[] = {
        [0x51] = VK_FORMAT_R8_USCALED, [0x52] = VK_FORMAT_R8_SSCALED, [0x53] = VK_FORMAT_R16_USCALED,
        [0x54] = VK_FORMAT_R16_SSCALED, [0x57] = VK_FORMAT_R8G8_USCALED, [0x58] = VK_FORMAT_R8G8_SSCALED,
        [0x59] = VK_FORMAT_R16G16_USCALED, [0x5A] = VK_FORMAT_R16G16_SSCALED, [0x5D] = VK_FORMAT_R8G8B8_USCALED,
        [0x5E] = VK_FORMAT_R8G8B8_SSCALED, [0x5F] = VK_FORMAT_R16G16B16_USCALED, [0x60] = VK_FORMAT_R16G16B16_SSCALED,
        [0x63] = VK_FORMAT_R8G8B8A8_USCALED, [0x64] = VK_FORMAT_R8G8B8A8_SSCALED,
        [0x65] = VK_FORMAT_R16G16B16A16_USCALED, [0x66] = VK_FORMAT_R16G16B16A16_SSCALED,
        [0x69] = VK_FORMAT_A2B10G10R10_SNORM_PACK32, [0x6B] = VK_FORMAT_A2B10G10R10_USCALED_PACK32,
        [0x6C] = VK_FORMAT_A2B10G10R10_SSCALED_PACK32,
    };
    static const VkFormat rgb[] = {          /* 3-component formats (not textures, so not in image.c) */
        [0x19] = VK_FORMAT_R8G8B8_UNORM, [0x1A] = VK_FORMAT_R8G8B8_SNORM, [0x1D] = VK_FORMAT_R16G16B16_SFLOAT,
        [0x1E] = VK_FORMAT_R16G16B16_UNORM, [0x1F] = VK_FORMAT_R16G16B16_SNORM, [0x22] = VK_FORMAT_R32G32B32_SFLOAT,
    };
    if (f < sizeof scaled / sizeof *scaled && scaled[f]) return scaled[f];
    if (f < sizeof rgb / sizeof *rgb && rgb[f]) return rgb[f];
    return render_vk_format(f);
}

/* ================================================================ pipelines */

typedef struct Pipeline {
    struct Pipeline *next;
    PipelineKey key;
    VkPipeline pipeline;
    int ready;                       /* (atomic) compiled, or failed: pipeline is final */
} Pipeline;

#define PIPELINE_BUCKETS 1024
static Pipeline *pipeline_buckets[PIPELINE_BUCKETS];

static uint32_t hash(const void *p, size_t n) {
    const uint8_t *b = p;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

static VkPipeline create(const PipelineKey *k) {
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = k->vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = k->fs, .pName = "main" },
    };

    /* vertex input: only the attributes the vertex shader reads */
    VkVertexInputAttributeDescription attrs[16];
    VkVertexInputBindingDescription binds[16];
    uint32_t nattrs = 0, nbinds = 0, stream_used = 0;
    for (uint32_t i = 0; i < k->nattribs; i++) {
        if (!(k->vs_inputs >> i & 1) || !k->attr[i].format) continue;
        VkFormat f = vertex_format(k->attr[i].format);
        if (f == VK_FORMAT_UNDEFINED) { fprintf(stderr, "render: vertex format %#x\n", k->attr[i].format); continue; }
        attrs[nattrs++] = (VkVertexInputAttributeDescription){ i, k->attr[i].stream, f, k->attr[i].offset };
        stream_used |= 1u << k->attr[i].stream;
    }
    for (uint32_t s = 0; s < 16; s++)
        if (stream_used >> s & 1)
            binds[nbinds++] = (VkVertexInputBindingDescription){ s, k->stream[s].stride,
                k->stream[s].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = nbinds, .pVertexBindingDescriptions = binds,
        .vertexAttributeDescriptionCount = nattrs, .pVertexAttributeDescriptions = attrs };
    VkPrimitiveTopology topo = topology(k->topology);
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = topo,
        .primitiveRestartEnable = k->primitive_restart && (topo == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP ||
                                  topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN) };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                             .viewportCount = 1, .scissorCount = 1 };
    /* cull: NONE, FRONT, BACK, FRONT_AND_BACK. Front face: NVN's CCW is Vulkan's CW (see notes in
     * draw.c: we render with y pointing the other way) */
    static const VkCullModeFlags cull[4] = { VK_CULL_MODE_NONE, VK_CULL_MODE_FRONT_BIT, VK_CULL_MODE_BACK_BIT,
                                             VK_CULL_MODE_FRONT_AND_BACK };
    static const VkPolygonMode poly[3] = { VK_POLYGON_MODE_POINT, VK_POLYGON_MODE_LINE, VK_POLYGON_MODE_FILL };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = VK_FALSE, .rasterizerDiscardEnable = k->rasterizer_discard,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = cull[k->cull & 3],
        .frontFace = k->front_ccw ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable = k->depth_bias, .lineWidth = 1.0f };
    (void)poly;
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT, .alphaToCoverageEnable = k->alpha_to_coverage };
    VkStencilOpState front = { stencil_op(k->stencil_ops[0] & 0xF), stencil_op(k->stencil_ops[0] >> 8 & 0xF),
                               stencil_op(k->stencil_ops[0] >> 4 & 0xF), render_compare_op(k->stencil_func[0]), 0, 0, 0 };
    VkStencilOpState back = { stencil_op(k->stencil_ops[1] & 0xF), stencil_op(k->stencil_ops[1] >> 8 & 0xF),
                              stencil_op(k->stencil_ops[1] >> 4 & 0xF), render_compare_op(k->stencil_func[1]), 0, 0, 0 };
    VkPipelineDepthStencilStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = k->depth_test, .depthWriteEnable = k->depth_write,
        .depthCompareOp = render_compare_op(k->depth_func), .stencilTestEnable = k->stencil_test,
        .front = front, .back = back };
    VkPipelineColorBlendAttachmentState att[8];
    for (uint32_t i = 0; i < k->ncolor; i++) {
        const BlendKey *b = &k->blend[i];
        att[i] = (VkPipelineColorBlendAttachmentState){
            .blendEnable = k->blend_enable >> i & 1,
            .srcColorBlendFactor = blend_factor(b->src_color), .dstColorBlendFactor = blend_factor(b->dst_color),
            .colorBlendOp = blend_op(b->eq_color),
            .srcAlphaBlendFactor = blend_factor(b->src_alpha), .dstAlphaBlendFactor = blend_factor(b->dst_alpha),
            .alphaBlendOp = blend_op(b->eq_alpha),
            .colorWriteMask = k->channel_mask >> (4 * i) & 0xF,
        };
    }
    VkPipelineColorBlendStateCreateInfo cbs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = k->logic_op != 3 && k->logic_op < 16, .logicOp = (VkLogicOp)(k->logic_op & 15),
        .attachmentCount = k->ncolor, .pAttachments = att };
    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                             VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                             VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_LINE_WIDTH };
    VkPipelineDynamicStateCreateInfo dy = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                            .dynamicStateCount = sizeof dyn / sizeof *dyn, .pDynamicStates = dyn };
    VkPipelineRenderingCreateInfo ri = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = k->ncolor, .pColorAttachmentFormats = k->color_format,
        .depthAttachmentFormat = (render_aspects(k->depth_format) & VK_IMAGE_ASPECT_DEPTH_BIT) ? k->depth_format : VK_FORMAT_UNDEFINED,
        .stencilAttachmentFormat = (render_aspects(k->depth_format) & VK_IMAGE_ASPECT_STENCIL_BIT) ? k->depth_format : VK_FORMAT_UNDEFINED };
    VkGraphicsPipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
        .pColorBlendState = &cbs, .pDynamicState = &dy, .layout = render_pipeline_layout() };
    if (render_tracing())
        fprintf(stderr, "render: pipeline vs %#llx fs %#llx, %u attributes, %u targets\n",
                (unsigned long long)k->vs_code, (unsigned long long)k->fs_code, nattrs, k->ncolor);
    if (render_tracing())
        fprintf(stderr, "render:   topo %u blend %#x logic %u mask %#x depth %u/%u/%u stencil %u cull %u ccw %u fmt %d/%d bl %u,%u,%u,%u,%u,%u\n",
                k->topology, k->blend_enable, k->logic_op, k->channel_mask, k->depth_test, k->depth_write, k->depth_func,
                k->stencil_test, k->cull, k->front_ccw, k->color_format[0], k->depth_format, k->blend[0].src_color,
                k->blend[0].dst_color, k->blend[0].src_alpha, k->blend[0].dst_alpha, k->blend[0].eq_color, k->blend[0].eq_alpha);
    if (render_tracing())
        for (uint32_t i = 0; i < nattrs; i++)
            fprintf(stderr, "render:   attribute %u: format %d (nvn %#x) stream %u offset %u, stride %u; color format %d\n",
                    attrs[i].location, attrs[i].format, k->attr[attrs[i].location].format, attrs[i].binding,
                    attrs[i].offset, k->stream[attrs[i].binding].stride, k->color_format[0]);
    VkPipeline p;
    VkResult r = vkCreateGraphicsPipelines(vk.device, vk.pipeline_cache, 1, &ci, NULL, &p);
    if (r != VK_SUCCESS) {           /* the driver refused it: its draws are skipped (counted as "other") */
        fprintf(stderr, "render: pipeline vs %#llx fs %#llx: vkCreateGraphicsPipelines %s\n",
                (unsigned long long)k->vs_code, (unsigned long long)k->fs_code, vk_result_name(r));
        return VK_NULL_HANDLE;
    }
    return p;
}

/* New pipelines are compiled by background threads: the driver takes several ms for one, and
 * walking into a new part of the village needed dozens (frames of 100+ ms). A draw whose pipeline
 * isn't ready waits for it while the frame's budget lasts (COMPILE_WAIT_MS: frames have that much
 * to spare now; with the pipeline cache a compile takes ~1 ms). Beyond it the draw is skipped, the
 * object appearing a frame or two late: skipping all of them made the world pop in after the HUD
 * when a save loaded. LAA_SYNC_PIPELINES=1: compile right away (screenshots that must show all). */
#define COMPILE_WAIT_MS 6.0
static double frame_wait_ms;                     /* waited for compiles this frame */
static uint64_t wait_frame;
#define COMPILE_THREADS 2
#define COMPILE_QUEUE 1024
static Pipeline *compile_queue[COMPILE_QUEUE];
static unsigned compile_head, compile_tail;
static pthread_mutex_t compile_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t compile_cond = PTHREAD_COND_INITIALIZER;
static double async_compile_ms;                  /* (render_stats adds these to the main thread's) */
static unsigned async_compiles;

static void *compile_thread(void *arg) {
    host_thread_init();                          /* (off the game's main core) */
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&compile_lock);
        while (compile_head == compile_tail) pthread_cond_wait(&compile_cond, &compile_lock);
        Pipeline *p = compile_queue[compile_tail++ % COMPILE_QUEUE];
        pthread_mutex_unlock(&compile_lock);
        double t0 = compile_clock();
        VkPipeline v = create(&p->key);
        pthread_mutex_lock(&compile_lock);
        async_compile_ms += compile_clock() - t0;
        async_compiles++;
        pthread_mutex_unlock(&compile_lock);
        p->pipeline = v;
        __atomic_store_n(&p->ready, 1, __ATOMIC_RELEASE);
    }
    return NULL;
}

void render_collect_compile_stats(void) {         /* (render_stats: once in a while, main thread) */
    pthread_mutex_lock(&compile_lock);
    render_compile_ms += async_compile_ms;
    render_compiles += async_compiles;
    async_compile_ms = 0;
    async_compiles = 0;
    pthread_mutex_unlock(&compile_lock);
}

VkPipeline render_pipeline(const PipelineKey *k) {
    static int sync = -1;
    if (sync < 0) {
        sync = getenv("LAA_SYNC_PIPELINES") != NULL;
        render_pipeline_layout();                    /* (made before any thread can use it) */
        for (int i = 0; !sync && i < COMPILE_THREADS; i++) {
            pthread_t t;
            pthread_create(&t, NULL, compile_thread, NULL);
            pthread_detach(t);
        }
    }
    unsigned b = hash(k, sizeof *k) & (PIPELINE_BUCKETS - 1);
    if (nvn_frame_count() != wait_frame) { wait_frame = nvn_frame_count(); frame_wait_ms = 0; }
    for (Pipeline *p = pipeline_buckets[b]; p; p = p->next)
        if (memcmp(&p->key, k, sizeof *k) == 0) {
            if (__atomic_load_n(&p->ready, __ATOMIC_ACQUIRE)) return p->pipeline;
            double t0 = compile_clock();             /* (wait while the frame's budget lasts) */
            while (!__atomic_load_n(&p->ready, __ATOMIC_ACQUIRE) && frame_wait_ms + compile_clock() - t0 < COMPILE_WAIT_MS) {
                struct timespec ts = { 0, 100000 };
                nanosleep(&ts, NULL);
            }
            frame_wait_ms += compile_clock() - t0;
            return __atomic_load_n(&p->ready, __ATOMIC_ACQUIRE) ? p->pipeline : VK_NULL_HANDLE;
        }
    Pipeline *p = calloc(1, sizeof *p);
    p->key = *k;
    p->next = pipeline_buckets[b];
    pipeline_buckets[b] = p;
    pthread_mutex_lock(&compile_lock);
    int queued = !sync && compile_head - compile_tail < COMPILE_QUEUE;
    if (queued) {
        compile_queue[compile_head++ % COMPILE_QUEUE] = p;
        pthread_cond_signal(&compile_cond);
    }
    pthread_mutex_unlock(&compile_lock);
    if (queued) return render_pipeline(k);           /* (found now: waits within the budget) */
    double t0 = compile_clock();                     /* (synchronous: as asked, or the queue is full) */
    p->pipeline = create(k);
    render_compile_ms += compile_clock() - t0;
    render_compiles++;
    __atomic_store_n(&p->ready, 1, __ATOMIC_RELEASE);
    return p->pipeline;
}
