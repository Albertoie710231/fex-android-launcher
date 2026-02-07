/*
 * Vulkan Headless Surface Wrapper for Vortek
 *
 * This wrapper adds VK_EXT_headless_surface support to libvulkan_vortek.so,
 * enabling headless Vulkan rendering without X11/Wayland.
 *
 * Architecture:
 * 1. This library wraps libvulkan_vortek.so
 * 2. Adds VK_EXT_headless_surface to the exposed extensions
 * 3. Implements vkCreateHeadlessSurfaceEXT
 * 4. Creates surfaces that map to Vortek's window system
 */

#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>

#define LOG_TAG "VulkanHeadless"
#ifdef __ANDROID__
#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) fprintf(stderr, "INFO: " __VA_ARGS__); fprintf(stderr, "\n")
#define LOGE(...) fprintf(stderr, "ERROR: " __VA_ARGS__); fprintf(stderr, "\n")
#endif

// The real Vortek library
static void* g_vortek_lib = NULL;
static pthread_mutex_t g_init_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_initialized = 0;

// Function pointers to the real Vortek functions
static PFN_vkGetInstanceProcAddr g_real_vkGetInstanceProcAddr = NULL;
static PFN_vkEnumerateInstanceExtensionProperties g_real_vkEnumerateInstanceExtensionProperties = NULL;
static PFN_vkCreateInstance g_real_vkCreateInstance = NULL;
static PFN_vkDestroyInstance g_real_vkDestroyInstance = NULL;
static PFN_vkDestroySurfaceKHR g_real_vkDestroySurfaceKHR = NULL;
static PFN_vkGetPhysicalDeviceSurfaceSupportKHR g_real_vkGetPhysicalDeviceSurfaceSupportKHR = NULL;
static PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR g_real_vkGetPhysicalDeviceSurfaceCapabilitiesKHR = NULL;
static PFN_vkGetPhysicalDeviceSurfaceFormatsKHR g_real_vkGetPhysicalDeviceSurfaceFormatsKHR = NULL;
static PFN_vkGetPhysicalDeviceSurfacePresentModesKHR g_real_vkGetPhysicalDeviceSurfacePresentModesKHR = NULL;

// Headless surface extension name
#define VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME "VK_EXT_headless_surface"
#define VK_EXT_HEADLESS_SURFACE_SPEC_VERSION 1

// Custom structure to track headless surfaces
typedef struct HeadlessSurface {
    VkSurfaceKHR handle;
    uint32_t width;
    uint32_t height;
    int windowId;
    struct HeadlessSurface* next;
} HeadlessSurface;

static HeadlessSurface* g_headless_surfaces = NULL;
static pthread_mutex_t g_surface_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_next_window_id = 1;

// Magic number to identify our headless surfaces
#define HEADLESS_SURFACE_MAGIC 0xDEADBEEF00000000ULL

static int is_headless_surface(VkSurfaceKHR surface) {
    return ((uint64_t)surface & 0xFFFFFFFF00000000ULL) == HEADLESS_SURFACE_MAGIC;
}

static HeadlessSurface* find_headless_surface(VkSurfaceKHR surface) {
    pthread_mutex_lock(&g_surface_mutex);
    HeadlessSurface* s = g_headless_surfaces;
    while (s) {
        if (s->handle == surface) {
            pthread_mutex_unlock(&g_surface_mutex);
            return s;
        }
        s = s->next;
    }
    pthread_mutex_unlock(&g_surface_mutex);
    return NULL;
}

// Initialize the wrapper - load the real Vortek library
static int init_wrapper(void) {
    pthread_mutex_lock(&g_init_mutex);

    if (g_initialized) {
        pthread_mutex_unlock(&g_init_mutex);
        return 1;
    }

    LOGI("Initializing Vulkan headless wrapper");

    // Try to load the real Vortek library
    const char* lib_paths[] = {
        "/lib/libvulkan_vortek_real.so",  // Renamed original
        "/usr/lib/libvulkan_vortek.so.real",
        "./libvulkan_vortek_real.so",
        NULL
    };

    for (int i = 0; lib_paths[i] != NULL; i++) {
        g_vortek_lib = dlopen(lib_paths[i], RTLD_NOW | RTLD_LOCAL);
        if (g_vortek_lib) {
            LOGI("Loaded real Vortek library from: %s", lib_paths[i]);
            break;
        }
    }

    if (!g_vortek_lib) {
        // If we can't find a renamed library, we might be the only ICD
        // In this case, we need to handle everything ourselves or fail gracefully
        LOGE("Could not load real Vortek library. Headless-only mode.");
        // Continue without the real library - we'll provide minimal headless support
    }

    if (g_vortek_lib) {
        g_real_vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)
            dlsym(g_vortek_lib, "vkGetInstanceProcAddr");
        g_real_vkEnumerateInstanceExtensionProperties = (PFN_vkEnumerateInstanceExtensionProperties)
            dlsym(g_vortek_lib, "vkEnumerateInstanceExtensionProperties");
        g_real_vkCreateInstance = (PFN_vkCreateInstance)
            dlsym(g_vortek_lib, "vkCreateInstance");

        if (!g_real_vkGetInstanceProcAddr) {
            LOGE("Failed to find vkGetInstanceProcAddr in Vortek library");
        }
    }

    g_initialized = 1;
    pthread_mutex_unlock(&g_init_mutex);
    return 1;
}

// VK_EXT_headless_surface implementation
typedef struct VkHeadlessSurfaceCreateInfoEXT {
    VkStructureType sType;
    const void* pNext;
    VkFlags flags;
} VkHeadlessSurfaceCreateInfoEXT;

#define VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT 1000256000

static VKAPI_ATTR VkResult VKAPI_CALL vkCreateHeadlessSurfaceEXT(
    VkInstance instance,
    const VkHeadlessSurfaceCreateInfoEXT* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSurfaceKHR* pSurface)
{
    (void)instance;
    (void)pCreateInfo;
    (void)pAllocator;

    LOGI("Creating headless surface");

    HeadlessSurface* surface = (HeadlessSurface*)malloc(sizeof(HeadlessSurface));
    if (!surface) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    pthread_mutex_lock(&g_surface_mutex);

    // Create a unique handle with our magic number
    surface->windowId = g_next_window_id++;
    surface->handle = (VkSurfaceKHR)(HEADLESS_SURFACE_MAGIC | (uint64_t)surface->windowId);
    surface->width = 1920;  // Default size, will be set by swapchain
    surface->height = 1080;

    // Add to linked list
    surface->next = g_headless_surfaces;
    g_headless_surfaces = surface;

    pthread_mutex_unlock(&g_surface_mutex);

    *pSurface = surface->handle;

    LOGI("Created headless surface: handle=0x%llx, windowId=%d",
         (unsigned long long)surface->handle, surface->windowId);

    return VK_SUCCESS;
}

// Wrapper for vkDestroySurfaceKHR
static VKAPI_ATTR void VKAPI_CALL wrapper_vkDestroySurfaceKHR(
    VkInstance instance,
    VkSurfaceKHR surface,
    const VkAllocationCallbacks* pAllocator)
{
    if (is_headless_surface(surface)) {
        LOGI("Destroying headless surface: 0x%llx", (unsigned long long)surface);

        pthread_mutex_lock(&g_surface_mutex);
        HeadlessSurface** pp = &g_headless_surfaces;
        while (*pp) {
            if ((*pp)->handle == surface) {
                HeadlessSurface* to_free = *pp;
                *pp = (*pp)->next;
                free(to_free);
                break;
            }
            pp = &(*pp)->next;
        }
        pthread_mutex_unlock(&g_surface_mutex);
        return;
    }

    // Forward to real implementation
    if (g_real_vkDestroySurfaceKHR) {
        g_real_vkDestroySurfaceKHR(instance, surface, pAllocator);
    }
}

// Wrapper for vkGetPhysicalDeviceSurfaceSupportKHR
static VKAPI_ATTR VkResult VKAPI_CALL wrapper_vkGetPhysicalDeviceSurfaceSupportKHR(
    VkPhysicalDevice physicalDevice,
    uint32_t queueFamilyIndex,
    VkSurfaceKHR surface,
    VkBool32* pSupported)
{
    if (is_headless_surface(surface)) {
        // Headless surfaces are supported on all queue families with graphics
        *pSupported = VK_TRUE;
        return VK_SUCCESS;
    }

    if (g_real_vkGetPhysicalDeviceSurfaceSupportKHR) {
        return g_real_vkGetPhysicalDeviceSurfaceSupportKHR(
            physicalDevice, queueFamilyIndex, surface, pSupported);
    }

    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

// Wrapper for vkGetPhysicalDeviceSurfaceCapabilitiesKHR
static VKAPI_ATTR VkResult VKAPI_CALL wrapper_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface,
    VkSurfaceCapabilitiesKHR* pSurfaceCapabilities)
{
    if (is_headless_surface(surface)) {
        HeadlessSurface* hs = find_headless_surface(surface);
        uint32_t width = hs ? hs->width : 1920;
        uint32_t height = hs ? hs->height : 1080;

        pSurfaceCapabilities->minImageCount = 2;
        pSurfaceCapabilities->maxImageCount = 8;
        pSurfaceCapabilities->currentExtent.width = width;
        pSurfaceCapabilities->currentExtent.height = height;
        pSurfaceCapabilities->minImageExtent.width = 1;
        pSurfaceCapabilities->minImageExtent.height = 1;
        pSurfaceCapabilities->maxImageExtent.width = 16384;
        pSurfaceCapabilities->maxImageExtent.height = 16384;
        pSurfaceCapabilities->maxImageArrayLayers = 1;
        pSurfaceCapabilities->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        pSurfaceCapabilities->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        pSurfaceCapabilities->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        pSurfaceCapabilities->supportedUsageFlags =
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        return VK_SUCCESS;
    }

    if (g_real_vkGetPhysicalDeviceSurfaceCapabilitiesKHR) {
        return g_real_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
            physicalDevice, surface, pSurfaceCapabilities);
    }

    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

// Wrapper for vkGetPhysicalDeviceSurfaceFormatsKHR
static VKAPI_ATTR VkResult VKAPI_CALL wrapper_vkGetPhysicalDeviceSurfaceFormatsKHR(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface,
    uint32_t* pSurfaceFormatCount,
    VkSurfaceFormatKHR* pSurfaceFormats)
{
    if (is_headless_surface(surface)) {
        static const VkSurfaceFormatKHR formats[] = {
            { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
            { VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
            { VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
            { VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
        };
        uint32_t count = sizeof(formats) / sizeof(formats[0]);

        if (pSurfaceFormats == NULL) {
            *pSurfaceFormatCount = count;
            return VK_SUCCESS;
        }

        uint32_t copy_count = *pSurfaceFormatCount < count ? *pSurfaceFormatCount : count;
        memcpy(pSurfaceFormats, formats, copy_count * sizeof(VkSurfaceFormatKHR));
        *pSurfaceFormatCount = copy_count;

        return copy_count < count ? VK_INCOMPLETE : VK_SUCCESS;
    }

    if (g_real_vkGetPhysicalDeviceSurfaceFormatsKHR) {
        return g_real_vkGetPhysicalDeviceSurfaceFormatsKHR(
            physicalDevice, surface, pSurfaceFormatCount, pSurfaceFormats);
    }

    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

// Wrapper for vkGetPhysicalDeviceSurfacePresentModesKHR
static VKAPI_ATTR VkResult VKAPI_CALL wrapper_vkGetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface,
    uint32_t* pPresentModeCount,
    VkPresentModeKHR* pPresentModes)
{
    if (is_headless_surface(surface)) {
        static const VkPresentModeKHR modes[] = {
            VK_PRESENT_MODE_FIFO_KHR,        // Always available
            VK_PRESENT_MODE_IMMEDIATE_KHR,   // No vsync
            VK_PRESENT_MODE_MAILBOX_KHR,     // Triple buffering
        };
        uint32_t count = sizeof(modes) / sizeof(modes[0]);

        if (pPresentModes == NULL) {
            *pPresentModeCount = count;
            return VK_SUCCESS;
        }

        uint32_t copy_count = *pPresentModeCount < count ? *pPresentModeCount : count;
        memcpy(pPresentModes, modes, copy_count * sizeof(VkPresentModeKHR));
        *pPresentModeCount = copy_count;

        return copy_count < count ? VK_INCOMPLETE : VK_SUCCESS;
    }

    if (g_real_vkGetPhysicalDeviceSurfacePresentModesKHR) {
        return g_real_vkGetPhysicalDeviceSurfacePresentModesKHR(
            physicalDevice, surface, pPresentModeCount, pPresentModes);
    }

    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

// Wrapper for vkEnumerateInstanceExtensionProperties
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(
    const char* pLayerName,
    uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties)
{
    init_wrapper();

    // Get the real extensions first
    uint32_t real_count = 0;
    VkResult result = VK_SUCCESS;

    if (g_real_vkEnumerateInstanceExtensionProperties) {
        result = g_real_vkEnumerateInstanceExtensionProperties(pLayerName, &real_count, NULL);
        if (result != VK_SUCCESS) {
            return result;
        }
    }

    // Add our headless extension
    uint32_t total_count = real_count + 1;

    if (pProperties == NULL) {
        *pPropertyCount = total_count;
        return VK_SUCCESS;
    }

    // Get real extensions
    if (g_real_vkEnumerateInstanceExtensionProperties && real_count > 0) {
        uint32_t count = *pPropertyCount > real_count ? real_count : *pPropertyCount;
        result = g_real_vkEnumerateInstanceExtensionProperties(pLayerName, &count, pProperties);
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
            return result;
        }
    }

    // Add headless extension if there's room
    if (*pPropertyCount > real_count) {
        VkExtensionProperties* headless_ext = &pProperties[real_count];
        strncpy(headless_ext->extensionName, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME,
                VK_MAX_EXTENSION_NAME_SIZE);
        headless_ext->specVersion = VK_EXT_HEADLESS_SURFACE_SPEC_VERSION;

        *pPropertyCount = real_count + 1;
        return VK_SUCCESS;
    }

    *pPropertyCount = real_count;
    return VK_INCOMPLETE;
}

// Main entry point - vkGetInstanceProcAddr
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(
    VkInstance instance,
    const char* pName)
{
    init_wrapper();

    // Our wrapper functions
    if (strcmp(pName, "vkEnumerateInstanceExtensionProperties") == 0) {
        return (PFN_vkVoidFunction)vkEnumerateInstanceExtensionProperties;
    }
    if (strcmp(pName, "vkCreateHeadlessSurfaceEXT") == 0) {
        return (PFN_vkVoidFunction)vkCreateHeadlessSurfaceEXT;
    }
    if (strcmp(pName, "vkDestroySurfaceKHR") == 0) {
        return (PFN_vkVoidFunction)wrapper_vkDestroySurfaceKHR;
    }
    if (strcmp(pName, "vkGetPhysicalDeviceSurfaceSupportKHR") == 0) {
        return (PFN_vkVoidFunction)wrapper_vkGetPhysicalDeviceSurfaceSupportKHR;
    }
    if (strcmp(pName, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR") == 0) {
        return (PFN_vkVoidFunction)wrapper_vkGetPhysicalDeviceSurfaceCapabilitiesKHR;
    }
    if (strcmp(pName, "vkGetPhysicalDeviceSurfaceFormatsKHR") == 0) {
        return (PFN_vkVoidFunction)wrapper_vkGetPhysicalDeviceSurfaceFormatsKHR;
    }
    if (strcmp(pName, "vkGetPhysicalDeviceSurfacePresentModesKHR") == 0) {
        return (PFN_vkVoidFunction)wrapper_vkGetPhysicalDeviceSurfacePresentModesKHR;
    }

    // Forward to real implementation
    if (g_real_vkGetInstanceProcAddr) {
        return g_real_vkGetInstanceProcAddr(instance, pName);
    }

    return NULL;
}

// Required ICD negotiation function
VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* pVersion) {
    // Support version 5 of the loader-ICD interface
    if (*pVersion > 5) {
        *pVersion = 5;
    }
    return VK_SUCCESS;
}

// ICD entry point
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(
    VkInstance instance,
    const char* pName)
{
    return vkGetInstanceProcAddr(instance, pName);
}

// Constructor to initialize early
__attribute__((constructor))
static void wrapper_init(void) {
    init_wrapper();
}
