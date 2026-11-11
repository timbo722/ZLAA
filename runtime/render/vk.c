/*
 * vk.c - Vulkan setup: instance, GPU, device, queue, window surface and swapchain.
 *
 * Steps, as Vulkan requires them:
 *   1. instance: the connection to the Vulkan loader, with the extensions SDL needs for windows
 *   2. physical device: pick a GPU (on WSL there is only "llvmpipe", a CPU renderer)
 *   3. surface: the window, as Vulkan sees it (SDL makes it)
 *   4. device + queue: our handle to the GPU, and the queue we submit work to
 *   5. swapchain: the images that are shown in the window, one after another
 */
#include "vk.h"
#include "render_internal.h"                 /* (render_load/save_pipeline_keys) */
#include "kernel.h"                          /* (host_thread_init) */
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

static void load_pipeline_cache(void);

#define VK_DEFINE(name) PFN_##name name;
VK_GLOBAL_FUNCTIONS(VK_DEFINE)
VK_INSTANCE_FUNCTIONS(VK_DEFINE)
VK_DEVICE_FUNCTIONS(VK_DEFINE)

Vk vk;

const char *vk_result_name(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    default: return "(other VkResult)";
    }
}

/* Why Vulkan couldn't start, in words for the player (render_error); and what the host shows them when a Vulkan
 * call fails for good (Android: a dialog; without a hook only the log has it) */
char vk_error[400];
void (*vk_fatal_hook)(const char *message);

static int cant_start(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(vk_error, sizeof vk_error, fmt, ap);
    va_end(ap);
    fprintf(stderr, "vulkan: %s\n", vk_error);
    return -1;
}

const char *render_error(void) { return vk_error; }
void render_on_fatal(void (*show)(const char *message)) { vk_fatal_hook = show; }

void vk_fail(const char *what, VkResult r) {
    static int failing;
    fprintf(stderr, "vulkan: %s failed: %s (%d)\n", what, vk_result_name(r), (int)r);
    fflush(NULL);
    if (__atomic_exchange_n(&failing, 1, __ATOMIC_SEQ_CST)) for (;;) pause();   /* (another thread is already closing the app) */
    if (vk_fatal_hook) {
        char text[400];
        snprintf(text, sizeof text, "A graphics error closed ZLAA:\n%.200s failed: %s", what, vk_result_name(r));
        vk_fatal_hook(text);
    }
    _exit(1);           /* (exit() would tear down the driver's state under the game's running threads) */
}

int vk_find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < vk.memory_props.memoryTypeCount; i++)
        if ((type_bits & (1u << i)) && (vk.memory_props.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}

uint32_t vk_memory_type(uint32_t type_bits, VkMemoryPropertyFlags want) {
    int i = vk_find_memory_type(type_bits, want);
    if (i >= 0) return (uint32_t)i;
    fprintf(stderr, "vulkan: no memory type with flags %#x\n", want);
    vk_fail("finding a memory type", VK_ERROR_FEATURE_NOT_PRESENT);
    return 0;
}

static int create_instance(void) {
    PFN_vkGetInstanceProcAddr get = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (!get) return cant_start("This device has no Vulkan graphics driver (%s).", SDL_GetError());
#define VK_LOAD_GLOBAL(name) name = (PFN_##name)get(NULL, #name);
    VK_GLOBAL_FUNCTIONS(VK_LOAD_GLOBAL)

    Uint32 n = 0;
    const char *const *sdl_ext = SDL_Vulkan_GetInstanceExtensions(&n);
    const char *layers[1];
    uint32_t nlayers = 0;
    if (getenv("LAA_VK_VALIDATION")) layers[nlayers++] = "VK_LAYER_KHRONOS_validation";
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "ZLAA",
                              .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                                .enabledExtensionCount = n, .ppEnabledExtensionNames = sdl_ext,
                                .enabledLayerCount = nlayers, .ppEnabledLayerNames = layers };
    VkResult r = vkCreateInstance(&ci, NULL, &vk.instance);
    if (r != VK_SUCCESS) return cant_start("Vulkan couldn't start on this device (%s).", vk_result_name(r));
#define VK_LOAD_INSTANCE(name) name = (PFN_##name)get(vk.instance, #name);
    VK_INSTANCE_FUNCTIONS(VK_LOAD_INSTANCE)
    return 0;
}

/* Prefer a real GPU over a CPU renderer; it must have a queue that draws and presents. */
static int pick_gpu(void) {
    VkPhysicalDevice gpus[16];
    uint32_t n = 16;
    VK_CHECK(vkEnumeratePhysicalDevices(vk.instance, &n, gpus));
    int best = -1, best_score = -1;
    uint32_t best_family = 0;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(gpus[i], &p);
        VkQueueFamilyProperties fam[16];
        uint32_t nf = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(gpus[i], &nf, fam);
        for (uint32_t f = 0; f < nf; f++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(gpus[i], f, vk.surface, &present);
            if (!(fam[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
            int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
                      : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
            if (score > best_score) { best = (int)i; best_score = score; best_family = f; }
            break;
        }
    }
    if (best < 0) return cant_start("No graphics processor here can draw to the screen with Vulkan.");
    vk.gpu = gpus[best];
    vk.queue_family = best_family;
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(vk.gpu, &p);
    vkGetPhysicalDeviceMemoryProperties(vk.gpu, &vk.memory_props);
    vk.cpu_renderer = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    /* the driver's own name and version (Vulkan 1.2's driver properties; older drivers leave them empty): bug
     * reports from other devices need them */
    VkPhysicalDeviceDriverProperties driver = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
    VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver };
    if (p.apiVersion >= VK_API_VERSION_1_1) vkGetPhysicalDeviceProperties2(vk.gpu, &p2);
    fprintf(stderr, "vulkan: using %s (Vulkan %u.%u.%u; driver %s %s, version %#x)\n", p.deviceName,
            VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion),
            driver.driverName, driver.driverInfo, p.driverVersion);
    snprintf(vk.gpu_name, sizeof vk.gpu_name, "%s", p.deviceName);
    vk.api_version = p.apiVersion;
    return 0;
}

static int has_extension(const char *name) {
    static VkExtensionProperties ext[512];
    static uint32_t n;
    if (!n) { n = 512; vkEnumerateDeviceExtensionProperties(vk.gpu, NULL, &n, ext); }
    for (uint32_t i = 0; i < n; i++) if (strcmp(ext[i].extensionName, name) == 0) return 1;
    return 0;
}

static int create_device(void) {
    if (getenv("LAA_VK_INFO")) {
        uint32_t n = 512;
        VkExtensionProperties ext[512];
        vkEnumerateDeviceExtensionProperties(vk.gpu, NULL, &n, ext);
        for (uint32_t i = 0; i < n; i++) fprintf(stderr, "vulkan: extension %s\n", ext[i].extensionName);
        VkPhysicalDevicePushDescriptorPropertiesKHR push = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR };
        VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &push };
        vkGetPhysicalDeviceProperties2(vk.gpu, &p2);
        const VkPhysicalDeviceLimits *l = &p2.properties.limits;
        fprintf(stderr, "vulkan: limits: dynamic uniform buffers %u per set, uniform buffers %u per stage / %u per set, "
                "samplers %u per stage, uniform offset alignment %llu, push descriptors %u\n",
                l->maxDescriptorSetUniformBuffersDynamic, l->maxPerStageDescriptorUniformBuffers, l->maxDescriptorSetUniformBuffers,
                l->maxPerStageDescriptorSampledImages, (unsigned long long)l->minUniformBufferOffsetAlignment, push.maxPushDescriptors);
    }

    /* What ZLAA can't do without, checked before the device is made: a clear sentence for the player instead of a
     * failed vkCreateDevice. (LAA_VK_FAIL=1 pretends something is missing: to see the message.) */
    if (vk.api_version < VK_API_VERSION_1_3 || getenv("LAA_VK_FAIL"))
        return cant_start("ZLAA needs Vulkan 1.3. This device's graphics driver (%s) offers Vulkan %u.%u.", vk.gpu_name,
                          VK_API_VERSION_MAJOR(vk.api_version), VK_API_VERSION_MINOR(vk.api_version));
    {
        VkPhysicalDeviceVulkan13Features h13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        VkPhysicalDeviceVulkan12Features h12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &h13 };
        VkPhysicalDeviceVulkan11Features h11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &h12 };
        VkPhysicalDeviceFeatures2 have = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &h11 };
        vkGetPhysicalDeviceFeatures2(vk.gpu, &have);
        char missing[200] = "";
        const struct { VkBool32 has; const char *name; } needs[] = {
            { h13.dynamicRendering, "dynamic rendering" }, { h11.shaderDrawParameters, "shader draw parameters" },
            { h12.bufferDeviceAddress, "buffer device addresses" }, { h12.timelineSemaphore, "timeline semaphores" },
            { has_extension(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME) || has_extension(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME),
              "shared memory (external memory)" },
        };
        for (size_t i = 0; i < sizeof needs / sizeof *needs; i++)
            if (!needs[i].has) snprintf(missing + strlen(missing), sizeof missing - strlen(missing), "%s%s", missing[0] ? ", " : "", needs[i].name);
        if (missing[0])
            return cant_start("This device's graphics driver (%s) lacks Vulkan features ZLAA needs: %s.", vk.gpu_name, missing);
    }
    float priority = 1.0f;
    VkDeviceQueueCreateInfo q = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                  .queueFamilyIndex = vk.queue_family, .queueCount = 1,
                                  .pQueuePriorities = &priority };
    const char *ext[4] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    uint32_t next = 1;
    /* how the GPU gets at the game's memory pools (buffer.c); LAA_POOL_EXPORT=1 tries Android's way on desktop */
    if (has_extension(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME) && !getenv("LAA_POOL_EXPORT")) {
        ext[next++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
        vk.host_import = 1;
    } else if (has_extension(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) {
        ext[next++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
        vk.fd_export = 1;
    }
    /* Vulkan 1.3 features we rely on: rendering without render pass objects */
    VkPhysicalDeviceVulkan13Features v13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                             .dynamicRendering = VK_TRUE };
    /* buffer device addresses: shaders read the game's memory through 64-bit pointers (LDG) */
    /* draw parameters: shaders see the draw's first instance (Maxwell's instance id counts from it) */
    VkPhysicalDeviceVulkan11Features v11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                             .pNext = &v13, .shaderDrawParameters = VK_TRUE };
    VkPhysicalDeviceVulkan12Features v12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                             .pNext = &v11, .bufferDeviceAddress = VK_TRUE,
                                             .timelineSemaphore = VK_TRUE };   /* (submit.c) */
    VkPhysicalDeviceFeatures2 features = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &v12 };
    VkPhysicalDeviceFeatures supported;
    vkGetPhysicalDeviceFeatures(vk.gpu, &supported);
    features.features.shaderInt64 = supported.shaderInt64;
    features.features.shaderImageGatherExtended = supported.shaderImageGatherExtended;
    features.features.textureCompressionBC = supported.textureCompressionBC;
    features.features.textureCompressionASTC_LDR = supported.textureCompressionASTC_LDR;
    VkDeviceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &features,
                              .queueCreateInfoCount = 1, .pQueueCreateInfos = &q,
                              .enabledExtensionCount = next, .ppEnabledExtensionNames = ext };
    VkResult made = vkCreateDevice(vk.gpu, &ci, NULL, &vk.device);
    if (made != VK_SUCCESS)
        return cant_start("This device's graphics driver (%s) refused to start (%s).", vk.gpu_name, vk_result_name(made));
    /* compressed textures the GPU can't sample: ASTC is decoded on the CPU (astc.c); BC has no decoder yet */
    fprintf(stderr, "vulkan: compressed textures: BC %s, ASTC %s\n",
            supported.textureCompressionBC ? "native" : "NOT SUPPORTED (those textures will be missing)",
            supported.textureCompressionASTC_LDR ? "native" : "decoded on the CPU");
#define VK_LOAD_DEVICE(name) name = (PFN_##name)vkGetDeviceProcAddr(vk.device, #name);
    VK_DEVICE_FUNCTIONS(VK_LOAD_DEVICE)
    vkGetDeviceQueue(vk.device, vk.queue_family, 0, &vk.queue);
    load_pipeline_cache();
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT host = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT };
    VkPhysicalDeviceSubgroupProperties subgroup = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES,
                                                    .pNext = vk.host_import ? &host : NULL };
    VkPhysicalDeviceProperties2 props = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &subgroup };
    vkGetPhysicalDeviceProperties2(vk.gpu, &props);
    /* Maxwell shaders' SHFL becomes subgroup shuffle / quad operations, mostly in fragment shaders */
    VkSubgroupFeatureFlags need = VK_SUBGROUP_FEATURE_SHUFFLE_BIT | VK_SUBGROUP_FEATURE_QUAD_BIT;
    if ((subgroup.supportedOperations & need) != need || !(subgroup.supportedStages & VK_SHADER_STAGE_FRAGMENT_BIT))
        fprintf(stderr, "vulkan: subgroup shuffle/quad operations in fragment shaders aren't supported (ops %#x, stages %#x)\n",
                subgroup.supportedOperations, subgroup.supportedStages);
    if (getenv("LAA_VK_INFO"))
        fprintf(stderr, "vulkan: subgroup size %u, operations %#x, stages %#x, quad in all stages %u\n", subgroup.subgroupSize,
                subgroup.supportedOperations, subgroup.supportedStages, subgroup.quadOperationsInAllStages);
    if (getenv("LAA_VK_INFO")) {                     /* what the GPU can do with depth images */
        static const VkFormat depth[] = { VK_FORMAT_D16_UNORM, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D32_SFLOAT,
                                          VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT };
        for (size_t i = 0; i < sizeof depth / sizeof *depth; i++) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(vk.gpu, depth[i], &fp);
            VkFormatFeatureFlags f = fp.optimalTilingFeatures;
            fprintf(stderr, "vulkan: depth format %d: attachment %d sampled %d filter linear %d blit src %d blit dst %d\n",
                    depth[i], !!(f & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT), !!(f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT),
                    !!(f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT),
                    !!(f & VK_FORMAT_FEATURE_BLIT_SRC_BIT), !!(f & VK_FORMAT_FEATURE_BLIT_DST_BIT));
        }
    }
    vk.limits = props.properties.limits;
    vk.import_alignment = vk.host_import ? host.minImportedHostPointerAlignment : 0;
    if (vk.fd_export) fprintf(stderr, "vulkan: memory pools are exported GPU memory mapped at their guest addresses\n");
    else if (!vk.host_import)
        fprintf(stderr, "vulkan: neither VK_EXT_external_memory_host nor VK_KHR_external_memory_fd: nothing can draw\n");
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                    .queueFamilyIndex = vk.queue_family };
    VK_CHECK(vkCreateCommandPool(vk.device, &pci, NULL, &vk.pool));
    return 0;
}

int vk_create_swapchain(uint32_t width, uint32_t height) {
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.gpu, vk.surface, &caps));
    VkSurfaceFormatKHR formats[32];
    uint32_t nf = 32;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk.gpu, vk.surface, &nf, formats);
    /* The game renders sRGB-encoded pixels (format RGBA8 sRGB); an sRGB swapchain shows them right. */
    VkSurfaceFormatKHR chosen = formats[0];
    for (uint32_t i = 0; i < nf; i++)
        if (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB || formats[i].format == VK_FORMAT_R8G8B8A8_SRGB) {
            chosen = formats[i];
            break;
        }
    vk.extent = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : (VkExtent2D){ width, height };
    /* The Thor's panel is natively portrait, so for our landscape window Android reports a
     * 90 degree transform. We don't render rotated ourselves: we ask for none (IDENTITY) and
     * the compositor rotates. (Present then returns VK_SUBOPTIMAL_KHR every time: render.c
     * ignores it.) */
    VkSurfaceTransformFlagBitsKHR transform = caps.currentTransform;
    if (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    fprintf(stderr, "vulkan: swapchain %ux%u, transform %#x (display's %#x)\n", vk.extent.width, vk.extent.height,
            transform, caps.currentTransform);
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = vk.surface, .minImageCount = count,
        .imageFormat = chosen.format, .imageColorSpace = chosen.colorSpace, .imageExtent = vk.extent,
        .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = transform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,               /* wait for the display: always available */
        .clipped = VK_TRUE, .oldSwapchain = vk.swapchain,
    };
    VkSwapchainKHR old = vk.swapchain;
    VK_CHECK(vkCreateSwapchainKHR(vk.device, &ci, NULL, &vk.swapchain));
    if (old) vkDestroySwapchainKHR(vk.device, old, NULL);
    vk.swapchain_format = chosen.format;
    vk.image_count = 8;
    VK_CHECK(vkGetSwapchainImagesKHR(vk.device, vk.swapchain, &vk.image_count, vk.images));
    return 0;
}

/* ---- the pipeline cache: LAA_CACHE_DIR/pipelines.bin (Android: the app folder's cache/,
 * desktop: ~/.cache/ZLAA). Compiling a pipeline takes the Adreno driver several ms, and
 * each new one stalls the frame that first needs it; with the cache that happens once ever.
 * (The driver checks the file's header and ignores a cache from another driver or device.) */
static char cache_path[1100];

static void load_pipeline_cache(void) {
    const char *dir = getenv("LAA_CACHE_DIR"), *home = getenv("HOME");
    char made[1024];
    if (!dir && home) {                              /* desktop default */
        snprintf(made, sizeof made, "%s/.cache", home);
        mkdir(made, 0755);
        snprintf(made, sizeof made, "%s/.cache/ZLAA", home);
        dir = made;
    }
    void *data = NULL;
    size_t size = 0;
    if (dir) {
        mkdir(dir, 0755);
        snprintf(cache_path, sizeof cache_path, "%s/pipelines.bin", dir);
        FILE *f = fopen(cache_path, "rb");
        const char *old = getenv("LAA_CACHE_FALLBACK");   /* (an older place: used if it holds more) */
        struct stat a, b;
        if (old && stat(old, &b) == 0 && (stat(cache_path, &a) != 0 || b.st_size > a.st_size)) {
            if (f) fclose(f);
            f = fopen(old, "rb");
        }
        if (f && fseek(f, 0, SEEK_END) == 0 && (size = (size_t)ftell(f)) > 0 && (data = malloc(size))) {
            rewind(f);
            if (fread(data, 1, size, f) != size) size = 0;
        }
        if (f) fclose(f);
    }
    VkPipelineCacheCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
                                     .initialDataSize = data ? size : 0, .pInitialData = data };
    if (vkCreatePipelineCache(vk.device, &ci, NULL, &vk.pipeline_cache) != VK_SUCCESS) {
        ci.initialDataSize = 0;                      /* (a damaged file: start empty) */
        VK_CHECK(vkCreatePipelineCache(vk.device, &ci, NULL, &vk.pipeline_cache));
    }
    if (data) fprintf(stderr, "vulkan: pipeline cache %s: %zu bytes\n", cache_path, size);
    if (*cache_path) {                               /* (which pipelines to make ahead: pipeline.c) */
        char keys[1200];
        snprintf(keys, sizeof keys, "%.*s/pipeline_keys.bin", (int)(strrchr(cache_path, '/') - cache_path), cache_path);
        render_load_pipeline_keys(keys);
    }
    free(data);
}

/* Saving copies megabytes to storage: on a thread of its own, at most once a minute (it took
 * the game thread long enough to miss frames every 5 s while walking around). */
static void *save_cache_now(void *arg);
static pthread_mutex_t save_lock = PTHREAD_MUTEX_INITIALIZER;
static int saving;

void vk_save_pipeline_cache(void) {
    static time_t last;
    time_t now = time(NULL);
    if (!*cache_path) return;
    int keys_only = last && now - last < 60;         /* (the small list of pipelines: every time) */
    pthread_mutex_lock(&save_lock);
    int busy = saving;
    saving = 1;
    pthread_mutex_unlock(&save_lock);
    if (busy) return;
    if (!keys_only) last = now;
    pthread_t t;
    if (pthread_create(&t, NULL, save_cache_now, keys_only ? (void *)1 : NULL) == 0) pthread_detach(t);
    else { pthread_mutex_lock(&save_lock); saving = 0; pthread_mutex_unlock(&save_lock); }
}

static void save_keys(void) {
    char keys[1200];
    snprintf(keys, sizeof keys, "%.*s/pipeline_keys.bin", (int)(strrchr(cache_path, '/') - cache_path), cache_path);
    render_save_pipeline_keys(keys);
}

static void *save_cache_now(void *arg) {
    host_thread_init();                          /* (off the game's main core) */
    size_t size = 0;
    if (arg) {                                       /* (keys only) */
        save_keys();
        pthread_mutex_lock(&save_lock); saving = 0; pthread_mutex_unlock(&save_lock);
        return NULL;
    }
    if (vkGetPipelineCacheData(vk.device, vk.pipeline_cache, &size, NULL) != VK_SUCCESS || !size) {
        pthread_mutex_lock(&save_lock); saving = 0; pthread_mutex_unlock(&save_lock);
        return NULL;
    }
    void *data = malloc(size);
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.new", cache_path);
    FILE *f = data && vkGetPipelineCacheData(vk.device, vk.pipeline_cache, &size, data) == VK_SUCCESS ? fopen(tmp, "wb") : NULL;
    if (f) {
        int ok = fwrite(data, 1, size, f) == size;
        if (fclose(f) == 0 && ok) rename(tmp, cache_path);   /* (a crash mid-write can't damage the old file) */
    }
    free(data);
    save_keys();
    pthread_mutex_lock(&save_lock);
    saving = 0;
    pthread_mutex_unlock(&save_lock);
    return NULL;
}

static SDL_Window *window;

int vk_init(void *sdl_window) {
    window = sdl_window;
    if (create_instance() != 0) return -1;
    if (!SDL_Vulkan_CreateSurface(window, vk.instance, NULL, &vk.surface)) {
        fprintf(stderr, "vulkan: no window surface (%s)\n", SDL_GetError());
        return -1;
    }
    if (pick_gpu() != 0 || create_device() != 0) return -1;
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    return vk_create_swapchain((uint32_t)w, (uint32_t)h);
}

/* Android takes the window away when the app goes to the background and gives a new one when it
 * comes back: a new surface and swapchain on that. The caller holds the queue, idle. */
int vk_recreate_surface(void) {
    if (vk.swapchain) vkDestroySwapchainKHR(vk.device, vk.swapchain, NULL);
    vk.swapchain = VK_NULL_HANDLE;
    SDL_Vulkan_DestroySurface(vk.instance, vk.surface, NULL);
    vk.surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(window, vk.instance, NULL, &vk.surface)) {
        fprintf(stderr, "vulkan: no window surface (%s)\n", SDL_GetError());
        return -1;
    }
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    return vk_create_swapchain((uint32_t)w, (uint32_t)h);
}
