/*
 * Simple Vulkan headless test - uses VK_EXT_headless_surface
 *
 * Uses weak symbols so LD_PRELOAD can intercept.
 *
 * Compile: gcc -o test_headless test_headless.c
 * Run: LD_PRELOAD=/lib/libvulkan_headless.so LD_LIBRARY_PATH=/usr/lib ./test_headless
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>

// Minimal Vulkan types
typedef uint32_t VkFlags;
typedef uint32_t VkBool32;
typedef uint64_t VkDeviceSize;
typedef int VkResult;
typedef void* VkInstance;
typedef void* VkPhysicalDevice;
typedef uint64_t VkSurfaceKHR;
typedef void (*PFN_vkVoidFunction)(void);

#define VK_SUCCESS 0
#define VK_TRUE 1
#define VK_FALSE 0
#define VK_NULL_HANDLE 0
#define VK_MAX_EXTENSION_NAME_SIZE 256
#define VK_STRUCTURE_TYPE_APPLICATION_INFO 0
#define VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO 1
#define VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT 1000256000

typedef struct VkApplicationInfo {
    int sType;
    const void* pNext;
    const char* pApplicationName;
    uint32_t applicationVersion;
    const char* pEngineName;
    uint32_t engineVersion;
    uint32_t apiVersion;
} VkApplicationInfo;

typedef struct VkInstanceCreateInfo {
    int sType;
    const void* pNext;
    VkFlags flags;
    const VkApplicationInfo* pApplicationInfo;
    uint32_t enabledLayerCount;
    const char* const* ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char* const* ppEnabledExtensionNames;
} VkInstanceCreateInfo;

typedef struct VkExtensionProperties {
    char extensionName[VK_MAX_EXTENSION_NAME_SIZE];
    uint32_t specVersion;
} VkExtensionProperties;

typedef struct VkHeadlessSurfaceCreateInfoEXT {
    int sType;
    const void* pNext;
    VkFlags flags;
} VkHeadlessSurfaceCreateInfoEXT;

// Function pointer types
typedef VkResult (*PFN_vkEnumerateInstanceExtensionProperties)(
    const char*, uint32_t*, VkExtensionProperties*);
typedef VkResult (*PFN_vkCreateInstance)(
    const VkInstanceCreateInfo*, const void*, VkInstance*);
typedef void (*PFN_vkDestroyInstance)(VkInstance, const void*);
typedef VkResult (*PFN_vkEnumeratePhysicalDevices)(
    VkInstance, uint32_t*, VkPhysicalDevice*);
typedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddr)(VkInstance, const char*);
typedef VkResult (*PFN_vkCreateHeadlessSurfaceEXT)(
    VkInstance, const VkHeadlessSurfaceCreateInfoEXT*, const void*, VkSurfaceKHR*);
typedef void (*PFN_vkDestroySurfaceKHR)(VkInstance, VkSurfaceKHR, const void*);

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    printf("=== Vulkan Headless Surface Test ===\n");
    printf("(Loading libvulkan.so.1 - LD_PRELOAD will intercept global symbols)\n\n");

    // Load Vulkan loader dynamically but use global symbol resolution
    // so that LD_PRELOAD can intercept
    void* vulkan = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!vulkan) {
        // Try alternate path
        vulkan = dlopen("/usr/lib/libvulkan.so.1", RTLD_NOW | RTLD_GLOBAL);
    }
    if (!vulkan) {
        vulkan = dlopen("/lib/libvulkan_vortek.so", RTLD_NOW | RTLD_GLOBAL);
    }
    if (!vulkan) {
        fprintf(stderr, "Failed to load Vulkan library: %s\n", dlerror());
        return 1;
    }
    printf("Loaded Vulkan library: %p\n", vulkan);

    // Get vkGetInstanceProcAddr - this should be intercepted by LD_PRELOAD
    // because we loaded with RTLD_GLOBAL
    PFN_vkGetInstanceProcAddr getInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr)dlsym(RTLD_DEFAULT, "vkGetInstanceProcAddr");
    if (!getInstanceProcAddr) {
        // Fall back to library handle
        getInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(vulkan, "vkGetInstanceProcAddr");
    }
    if (!getInstanceProcAddr) {
        fprintf(stderr, "Failed to get vkGetInstanceProcAddr\n");
        return 1;
    }
    printf("Got vkGetInstanceProcAddr: %p\n", (void*)getInstanceProcAddr);

    // Get vkEnumerateInstanceExtensionProperties - try global first (LD_PRELOAD)
    PFN_vkEnumerateInstanceExtensionProperties enumExtProps =
        (PFN_vkEnumerateInstanceExtensionProperties)dlsym(RTLD_DEFAULT, "vkEnumerateInstanceExtensionProperties");
    if (!enumExtProps) {
        enumExtProps = (PFN_vkEnumerateInstanceExtensionProperties)getInstanceProcAddr(NULL, "vkEnumerateInstanceExtensionProperties");
    }
    if (!enumExtProps) {
        fprintf(stderr, "Failed to get vkEnumerateInstanceExtensionProperties\n");
        return 1;
    }
    printf("Got vkEnumerateInstanceExtensionProperties: %p\n", (void*)enumExtProps);

    // List extensions
    uint32_t extCount = 0;
    VkResult result = enumExtProps(NULL, &extCount, NULL);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkEnumerateInstanceExtensionProperties count failed: %d\n", result);
        return 1;
    }
    printf("\nFound %u instance extensions:\n", extCount);

    VkExtensionProperties* exts = malloc(extCount * sizeof(VkExtensionProperties));
    enumExtProps(NULL, &extCount, exts);

    int hasHeadless = 0;
    int hasSurface = 0;
    for (uint32_t i = 0; i < extCount; i++) {
        printf("  [%u] %s (v%u)\n", i, exts[i].extensionName, exts[i].specVersion);
        if (strcmp(exts[i].extensionName, "VK_EXT_headless_surface") == 0) hasHeadless = 1;
        if (strcmp(exts[i].extensionName, "VK_KHR_surface") == 0) hasSurface = 1;
    }
    free(exts);

    if (!hasHeadless) {
        fprintf(stderr, "\n*** VK_EXT_headless_surface not found! ***\n");
        fprintf(stderr, "The LD_PRELOAD intercept is not working.\n");
        fprintf(stderr, "Make sure: LD_PRELOAD=/lib/libvulkan_headless.so\n");
        return 1;
    }
    printf("\n*** VK_EXT_headless_surface is available! ***\n");

    // Create instance
    PFN_vkCreateInstance createInstance =
        (PFN_vkCreateInstance)getInstanceProcAddr(NULL, "vkCreateInstance");
    if (!createInstance) {
        fprintf(stderr, "Failed to get vkCreateInstance\n");
        return 1;
    }

    VkApplicationInfo appInfo = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Headless Test",
        .applicationVersion = 1,
        .pEngineName = "Test Engine",
        .engineVersion = 1,
        .apiVersion = (1 << 22) | (1 << 12) | 0  // Vulkan 1.1.0
    };

    const char* extensions[] = {
        "VK_KHR_surface",
        "VK_EXT_headless_surface"
    };

    VkInstanceCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &appInfo,
        .enabledExtensionCount = hasSurface ? 2 : 1,
        .ppEnabledExtensionNames = hasSurface ? extensions : &extensions[1]
    };

    VkInstance instance = VK_NULL_HANDLE;
    result = createInstance(&createInfo, NULL, &instance);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateInstance failed: %d\n", result);
        return 1;
    }
    printf("Created Vulkan instance: %p\n", instance);

    // Get device count
    PFN_vkEnumeratePhysicalDevices enumDevices =
        (PFN_vkEnumeratePhysicalDevices)getInstanceProcAddr(instance, "vkEnumeratePhysicalDevices");
    if (enumDevices) {
        uint32_t deviceCount = 0;
        enumDevices(instance, &deviceCount, NULL);
        printf("Found %u physical device(s)\n", deviceCount);
    }

    // Try to create headless surface
    PFN_vkCreateHeadlessSurfaceEXT createHeadlessSurface =
        (PFN_vkCreateHeadlessSurfaceEXT)getInstanceProcAddr(instance, "vkCreateHeadlessSurfaceEXT");

    if (createHeadlessSurface) {
        printf("\nvkCreateHeadlessSurfaceEXT is available!\n");

        VkHeadlessSurfaceCreateInfoEXT surfaceInfo = {
            .sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT,
            .pNext = NULL,
            .flags = 0
        };

        VkSurfaceKHR surface = VK_NULL_HANDLE;
        result = createHeadlessSurface(instance, &surfaceInfo, NULL, &surface);
        if (result == VK_SUCCESS) {
            printf("\n*************************************\n");
            printf("* SUCCESS: Created headless surface *\n");
            printf("* Surface handle: 0x%lx              *\n", (unsigned long)surface);
            printf("*************************************\n");

            // Destroy surface
            PFN_vkDestroySurfaceKHR destroySurface =
                (PFN_vkDestroySurfaceKHR)getInstanceProcAddr(instance, "vkDestroySurfaceKHR");
            if (destroySurface) {
                destroySurface(instance, surface, NULL);
                printf("Destroyed headless surface\n");
            }
        } else {
            fprintf(stderr, "vkCreateHeadlessSurfaceEXT failed: %d\n", result);
        }
    } else {
        fprintf(stderr, "\nvkCreateHeadlessSurfaceEXT function not found!\n");
    }

    // Cleanup
    PFN_vkDestroyInstance destroyInstance =
        (PFN_vkDestroyInstance)getInstanceProcAddr(instance, "vkDestroyInstance");
    if (destroyInstance) {
        destroyInstance(instance, NULL);
        printf("Destroyed Vulkan instance\n");
    }

    printf("\n=== Test Complete ===\n");
    return 0;
}
