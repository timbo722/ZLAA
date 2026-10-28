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
#include "kernel.h"                          /* (host_thread_init) */
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <pthread.h>
#include <time.h>

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

void vk_fail(const char *what, VkResult r) {
    fprintf(stderr, "vulkan: %s failed: %s (%d)\n", what, vk_result_name(r), (int)r);
    exit(1);
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
    exit(1);
}

static int create_instance(void) {
    PFN_vkGetInstanceProcAddr get = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (!get) { fprintf(stderr, "vulkan: no Vulkan loader (%s)\n", SDL_GetError()); return -1; }
#define VK_LOAD_GLOBAL(name) name = (PFN_##name)get(NULL, #name);
    VK_GLOBAL_FUNCTIONS(VK_LOAD_GLOBAL)

    Uint32 n = 0;
    const char *const *sdl_ext = SDL_Vulkan_GetInstanceExtensions(&n);
    const char *layers[1];
    uint32_t nlayers = 0;
    if (getenv("LAA_VK_VALIDATION")) layers[nlayers++] = "VK_LAYER_KHRONOS_validation";
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "LAARecomp",
                              .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                                .enabledExtensionCount = n, .ppEnabledExtensionNames = sdl_ext,
                                .enabledLayerCount = nlayers, .ppEnabledLayerNames = layers };
    VK_CHECK(vkCreateInstance(&ci, NULL, &vk.instance));
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
    if (best < 0) { fprintf(stderr, "vulkan: no GPU can draw to this window\n"); return -1; }
    vk.gpu = gpus[best];
    vk.queue_family = best_family;
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(vk.gpu, &p);
    vkGetPhysicalDeviceMemoryProperties(vk.gpu, &vk.memory_props);
    fprintf(stderr, "vulkan: using %s (Vulkan %u.%u)\n", p.deviceName,
            VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion));
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
    VK_CHECK(vkCreateDevice(vk.gpu, &ci, NULL, &vk.device));
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
            fprintf(stderr, "vulkan: depth format %d: attachment %d sampled %d blit src %d blit dst %d\n", depth[i],
                    !!(f & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT), !!(f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT),
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
 * desktop: ~/.cache/LAARecomp). Compiling a pipeline takes the Adreno driver several ms, and
 * each new one stalls the frame that first needs it; with the cache that happens once ever.
 * (The driver checks the file's header and ignores a cache from another driver or device.) */
static char cache_path[1100];

static void load_pipeline_cache(void) {
    const char *dir = getenv("LAA_CACHE_DIR"), *home = getenv("HOME");
    char made[1024];
    if (!dir && home) {                              /* desktop default */
        snprintf(made, sizeof made, "%s/.cache", home);
        mkdir(made, 0755);
        snprintf(made, sizeof made, "%s/.cache/LAARecomp", home);
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
    if (!*cache_path || (last && now - last < 60)) return;
    pthread_mutex_lock(&save_lock);
    int busy = saving;
    saving = 1;
    pthread_mutex_unlock(&save_lock);
    if (busy) return;
    last = now;
    pthread_t t;
    if (pthread_create(&t, NULL, save_cache_now, NULL) == 0) pthread_detach(t);
    else { pthread_mutex_lock(&save_lock); saving = 0; pthread_mutex_unlock(&save_lock); }
}

static void *save_cache_now(void *arg) {
    host_thread_init();                          /* (off the game's main core) */
    (void)arg;
    size_t size = 0;
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
    pthread_mutex_lock(&save_lock);
    saving = 0;
    pthread_mutex_unlock(&save_lock);
    return NULL;
}

int vk_init(void *sdl_window) {
    SDL_Window *window = sdl_window;
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
