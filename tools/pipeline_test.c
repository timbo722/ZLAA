/*
 * pipeline_test.c - compile translated shaders with a real Vulkan driver, without the game.
 *
 *   pipeline_test <vertex.spv> <fragment.spv>     build a simple pipeline from the two
 *   pipeline_test <shader.spv>                    build just a shader module
 *
 * Uses the first Vulkan device (on WSL: llvmpipe). For finding shaders that a driver rejects
 * or crashes on.
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>

static uint32_t *load(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    *size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *p = malloc(*size);
    if (fread(p, 1, *size, f) != *size) exit(2);
    fclose(f);
    return p;
}

static VkShaderModule module(VkDevice dev, const char *path) {
    size_t size;
    uint32_t *code = load(path, &size);
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code };
    VkShaderModule m;
    if (vkCreateShaderModule(dev, &ci, NULL, &m) != VK_SUCCESS) { fprintf(stderr, "%s: module failed\n", path); exit(1); }
    return m;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <vs.spv> [fs.spv]\n", argv[0]); return 2; }
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) return 1;
    uint32_t n = 1;
    VkPhysicalDevice gpu;
    vkEnumeratePhysicalDevices(inst, &n, &gpu);
    float prio = 1;
    VkDeviceQueueCreateInfo q = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio };
    VkPhysicalDeviceVulkan13Features v13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = 1 };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &v13, .queueCreateInfoCount = 1, .pQueueCreateInfos = &q };
    VkDevice dev;
    if (vkCreateDevice(gpu, &dci, NULL, &dev) != VK_SUCCESS) return 1;
    VkShaderModule vs = module(dev, argv[1]);
    if (argc < 3) { printf("module ok\n"); return 0; }
    VkShaderModule fs = module(dev, argv[2]);

    /* a layout with every binding our shaders might use */
    VkDescriptorSetLayoutBinding b[100];
    uint32_t nb = 0;
    for (uint32_t s = 0; s < 2; s++)
        for (uint32_t i = 0; i < 50; i++)
            b[nb++] = (VkDescriptorSetLayoutBinding){ s * 64 + (i < 18 ? i : i - 18 + 32),
                i < 18 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                s ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo li = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = nb, .pBindings = b };
    VkDescriptorSetLayout sl;
    vkCreateDescriptorSetLayout(dev, &li, NULL, &sl);
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &sl };
    VkPipelineLayout pl;
    vkCreatePipelineLayout(dev, &pli, NULL, &pl);

    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } };
    /* vertex input: vec2 floats at every location the shader could read (0..7), one stream */
    VkVertexInputAttributeDescription at[8];
    for (uint32_t i = 0; i < 8; i++) at[i] = (VkVertexInputAttributeDescription){ i, 0, VK_FORMAT_R32G32_SFLOAT, 0 };
    VkVertexInputBindingDescription bd = { 0, 16, VK_VERTEX_INPUT_RATE_VERTEX };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &bd,
        .vertexAttributeDescriptionCount = argc > 3 ? (uint32_t)atoi(argv[3]) : 2, .pVertexAttributeDescriptions = at };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .lineWidth = 1 };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = 1 };
    VkPipelineColorBlendAttachmentState att = { .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &att };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkFormat fmt = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    VkPipelineRenderingCreateInfo ri = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &fmt };
    VkGraphicsPipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2, .pStages = st,
        .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &dy, .layout = pl };
    VkPipeline p;
    VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p);
    printf("pipeline: %d\n", (int)r);
    if (r != VK_SUCCESS) return 1;

    /* ---- one real draw (some drivers only compile shaders then) ---- */
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
    #define MEMTYPE(bits) ({ uint32_t t_ = 0; while (!((bits) >> t_ & 1) || !(mp.memoryTypes[t_].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) t_++; t_; })
    /* images: a 64x64 target and a 1x1 texture */
    VkImage img[2];
    VkImageView view[2];
    VkFormat fmts[2] = { fmt, VK_FORMAT_R8G8B8A8_UNORM };
    for (int i = 0; i < 2; i++) {
        VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmts[i],
            .extent = { i ? 1u : 64u, i ? 1u : 64u, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = 1,
            .usage = i ? VK_IMAGE_USAGE_SAMPLED_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT };
        vkCreateImage(dev, &ii, NULL, &img[i]);
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(dev, img[i], &req);
        VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = MEMTYPE(req.memoryTypeBits) };
        VkDeviceMemory m;
        vkAllocateMemory(dev, &ai, NULL, &m);
        vkBindImageMemory(dev, img[i], m, 0);
        VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img[i], .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = fmts[i], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        vkCreateImageView(dev, &vci, NULL, &view[i]);
    }
    VkBuffer buf;
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 0x10000,
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT };
    vkCreateBuffer(dev, &bci, NULL, &buf);
    VkMemoryRequirements breq;
    vkGetBufferMemoryRequirements(dev, buf, &breq);
    VkMemoryAllocateInfo bai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = breq.size, .memoryTypeIndex = MEMTYPE(breq.memoryTypeBits) };
    VkDeviceMemory bm;
    vkAllocateMemory(dev, &bai, NULL, &bm);
    vkBindBufferMemory(dev, buf, bm, 0);
    VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .maxLod = 1000 };
    VkSampler smp;
    vkCreateSampler(dev, &sci, NULL, &smp);
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 64 }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64 } };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = ps };
    VkDescriptorPool dp;
    vkCreateDescriptorPool(dev, &dpi, NULL, &dp);
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &sl };
    VkDescriptorSet set;
    vkAllocateDescriptorSets(dev, &dai, &set);
    VkWriteDescriptorSet w[100];
    VkDescriptorBufferInfo bi = { buf, 0, 0x10000 };
    VkDescriptorImageInfo ii = { smp, view[1], VK_IMAGE_LAYOUT_GENERAL };
    for (uint32_t i = 0; i < nb; i++)
        w[i] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = b[i].binding,
            .descriptorCount = 1, .descriptorType = b[i].descriptorType, .pBufferInfo = &bi, .pImageInfo = &ii };
    vkUpdateDescriptorSets(dev, nb, w, 0, NULL);
    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    VkCommandPool cp;
    vkCreateCommandPool(dev, &cpi, NULL, &cp);
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .commandBufferCount = 1 };
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(dev, &cai, &cmd);
    VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cmd, &cbi);
    VkImageMemoryBarrier bar = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    for (int i = 0; i < 2; i++) {
        bar.image = img[i];
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &bar);
    }
    VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view[0],
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
    VkRenderingInfo rinfo = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { 64, 64 } }, .layerCount = 1,
        .colorAttachmentCount = 1, .pColorAttachments = &ca };
    vkCmdBeginRendering(cmd, &rinfo);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &set, 0, NULL);
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &buf, &off);
    VkViewport vpt = { 0, 0, 64, 64, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { 64, 64 } };
    vkCmdSetViewport(cmd, 0, 1, &vpt);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    vkEndCommandBuffer(cmd);
    VkQueue queue;
    vkGetDeviceQueue(dev, 0, 0, &queue);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    printf("draw: ok\n");
    return 0;
}
