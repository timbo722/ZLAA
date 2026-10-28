/*
 * vk.h - the Vulkan renderer's shared state and the Vulkan functions it uses.
 *
 * We don't link against libvulkan: SDL finds the Vulkan loader at run time (on Android it is
 * part of the system) and gives us vkGetInstanceProcAddr, from which we look up every other
 * function. Each function we use is listed once in VK_FUNCTIONS, and the X-macro trick turns
 * that list into function-pointer variables and the code that loads them.
 */
#ifndef VK_H
#define VK_H

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <stdint.h>

/* X(name): loaded with vkGetInstanceProcAddr(NULL, ...), before an instance exists */
#define VK_GLOBAL_FUNCTIONS(X) \
    X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties) X(vkEnumerateInstanceLayerProperties)

/* loaded with vkGetInstanceProcAddr(instance, ...) */
#define VK_INSTANCE_FUNCTIONS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceFeatures) \
    X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkEnumerateDeviceExtensionProperties) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR) \
    X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceExternalBufferProperties)

/* loaded with vkGetDeviceProcAddr(device, ...) */
#define VK_DEVICE_FUNCTIONS(X) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueSubmit) X(vkQueueWaitIdle) \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) \
    X(vkQueuePresentKHR) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) \
    X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) \
    X(vkCreateSemaphore) X(vkDestroySemaphore) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) \
    X(vkCreateImageView) X(vkDestroyImageView) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdBlitImage) \
    X(vkCmdCopyImage) X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) \
    X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkGetMemoryHostPointerPropertiesEXT) \
    X(vkCreateRenderPass) X(vkCreateFramebuffer) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateGraphicsPipelines) X(vkCreateComputePipelines) X(vkCmdDispatch) X(vkCmdDispatchIndirect) \
    X(vkDestroyPipeline) X(vkCreateDescriptorSetLayout) \
    X(vkCreateDescriptorPool) X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCreateSampler) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers) \
    X(vkCmdBindIndexBuffer) X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdSetViewport) X(vkCmdSetScissor) \
    X(vkCmdSetBlendConstants) X(vkCmdSetStencilReference) X(vkCmdSetStencilCompareMask) \
    X(vkCmdSetStencilWriteMask) X(vkCmdSetDepthBias) X(vkCmdSetLineWidth) X(vkCmdUpdateBuffer) \
    X(vkCmdFillBuffer) X(vkCmdCopyBuffer) X(vkCmdClearAttachments) X(vkGetBufferDeviceAddress) \
    X(vkGetMemoryFdKHR) X(vkCreateQueryPool) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults) \
    X(vkWaitSemaphores) X(vkGetSemaphoreCounterValue) X(vkCmdPushConstants) \
    X(vkCreatePipelineCache) X(vkGetPipelineCacheData) X(vkResetCommandPool)

#define VK_DECLARE(name) extern PFN_##name name;
VK_GLOBAL_FUNCTIONS(VK_DECLARE)
VK_INSTANCE_FUNCTIONS(VK_DECLARE)
VK_DEVICE_FUNCTIONS(VK_DECLARE)

/* everything about our Vulkan setup, in one place */
typedef struct {
    VkInstance instance;
    VkPhysicalDevice gpu;
    VkPhysicalDeviceMemoryProperties memory_props;
    VkDevice device;
    uint32_t queue_family;
    VkQueue queue;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat swapchain_format;
    VkExtent2D extent;
    uint32_t image_count;
    VkImage images[8];
    VkCommandPool pool;
    int host_import;                 /* VK_EXT_external_memory_host: GPU access to guest memory */
    VkDeviceSize import_alignment;
    int fd_export;                   /* otherwise VK_KHR_external_memory_fd: pools become exported memory (buffer.c) */
    VkPhysicalDeviceLimits limits;
    VkPipelineCache pipeline_cache;  /* the driver's compiled pipelines, kept between runs */
} Vk;
extern Vk vk;

/* vk.c */
int vk_init(void *sdl_window);               /* 0 on success */
int vk_create_swapchain(uint32_t width, uint32_t height);
uint32_t vk_memory_type(uint32_t type_bits, VkMemoryPropertyFlags want);   /* exits if there is none */
int vk_find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags want);   /* -1 if there is none */
const char *vk_result_name(VkResult r);
void vk_save_pipeline_cache(void);
#define VK_CHECK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) vk_fail(#call, r_); } while (0)
void vk_fail(const char *what, VkResult r);   /* prints and exits */

#endif
