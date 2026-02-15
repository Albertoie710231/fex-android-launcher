/*
 * Minimal test: dlopen guest Vulkan loader → fex_thunk_icd → FEX thunks
 *
 * This mimics what Wine's winevulkan.so does: dlopen("libvulkan.so.1") at
 * runtime, then create an instance and enumerate physical devices.
 * If this hangs, the problem is in the loader→ICD→thunk chain.
 * If it works, the problem is Wine-specific.
 *
 * Build: x86_64-linux-gnu-gcc -o test_vulkan_loader test_vulkan_loader.c -ldl
 * Run:   ./test_vulkan_loader
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdint.h>
#include <time.h>

typedef void (*PFN_vkVoidFunction)(void);
typedef void* VkInstance;
typedef void* VkPhysicalDevice;
typedef int VkResult;
typedef uint32_t VkFlags;

typedef struct {
    int sType; const void* pNext;
    const char* pApplicationName; uint32_t applicationVersion;
    const char* pEngineName; uint32_t engineVersion;
    uint32_t apiVersion;
} VkApplicationInfo;

typedef struct {
    int sType; const void* pNext; VkFlags flags;
    const VkApplicationInfo* pApplicationInfo;
    uint32_t enabledLayerCount;
    const char* const* ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char* const* ppEnabledExtensionNames;
} VkInstanceCreateInfo;

typedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddr)(VkInstance, const char*);
typedef VkResult (*PFN_vkCreateInstance)(const VkInstanceCreateInfo*, const void*, VkInstance*);
typedef VkResult (*PFN_vkEnumeratePhysicalDevices)(VkInstance, uint32_t*, VkPhysicalDevice*);
typedef void (*PFN_vkDestroyInstance)(VkInstance, const void*);
typedef VkResult (*PFN_vkEnumerateInstanceExtensionProperties)(const char*, uint32_t*, void*);

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

#define TLOG(fmt, ...) do { \
    fprintf(stderr, "[%.3f] " fmt, now_sec(), ##__VA_ARGS__); \
    fflush(stderr); \
} while(0)

/* Write a marker file to /tmp for debugging even if stderr is lost */
static void marker(const char* name) {
    char path[256];
    snprintf(path, sizeof(path), "/tmp/vk_test_%s", name);
    FILE* f = fopen(path, "w");
    if (f) { fprintf(f, "%s\n", name); fclose(f); }
}

int main(void) {
    TLOG("=== Vulkan loader→ICD→thunk test ===\n");
    marker("start");

    /* Step 1: dlopen the guest Vulkan loader (same as Wine) */
    TLOG("Step 1: dlopen(\"libvulkan.so.1\")...\n");
    marker("dlopen_start");
    void* vk = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!vk) {
        TLOG("FAIL: %s\n", dlerror());
        marker("dlopen_fail");
        return 1;
    }
    marker("dlopen_done");
    TLOG("OK: libvulkan.so.1 loaded at %p\n", vk);

    /* Step 2: Get vkGetInstanceProcAddr */
    PFN_vkGetInstanceProcAddr gipa =
        (PFN_vkGetInstanceProcAddr)dlsym(vk, "vkGetInstanceProcAddr");
    if (!gipa) {
        TLOG("FAIL: vkGetInstanceProcAddr not found\n");
        return 1;
    }
    TLOG("Step 2: vkGetInstanceProcAddr = %p\n", (void*)gipa);

    /* Step 2b: Enumerate instance extensions (for debugging) */
    TLOG("Step 2b: Enumerating instance extensions...\n");
    marker("enum_ext_start");
    PFN_vkEnumerateInstanceExtensionProperties enumExt =
        (PFN_vkEnumerateInstanceExtensionProperties)gipa(NULL, "vkEnumerateInstanceExtensionProperties");
    if (enumExt) {
        uint32_t count = 0;
        VkResult res = enumExt(NULL, &count, NULL);
        TLOG("  Instance extensions: %u (result=%d)\n", count, res);
    }
    marker("enum_ext_done");

    /* Step 3: Create instance with NO extensions */
    TLOG("Step 3: Creating VkInstance (no extensions)...\n");
    marker("create_start");

    VkApplicationInfo appInfo = {0};
    appInfo.sType = 0; /* VK_STRUCTURE_TYPE_APPLICATION_INFO */
    appInfo.pApplicationName = "vk_loader_test";
    appInfo.apiVersion = (1 << 22) | (3 << 12); /* VK 1.3 */

    VkInstanceCreateInfo ci = {0};
    ci.sType = 1; /* VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO */
    ci.pApplicationInfo = &appInfo;

    PFN_vkCreateInstance createInstance =
        (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    if (!createInstance) {
        TLOG("FAIL: vkCreateInstance not found\n");
        return 1;
    }

    VkInstance instance = NULL;
    VkResult res = createInstance(&ci, NULL, &instance);
    marker("create_done");
    if (res != 0) {
        TLOG("FAIL: vkCreateInstance returned %d\n", res);
        marker("create_fail");
        return 1;
    }
    TLOG("OK: VkInstance = %p\n", instance);

    /* Step 4: THE CRITICAL TEST — vkEnumeratePhysicalDevices */
    TLOG("Step 4: vkEnumeratePhysicalDevices (COUNT ONLY)...\n");
    marker("enum_pd_count_start");

    PFN_vkEnumeratePhysicalDevices enumPD =
        (PFN_vkEnumeratePhysicalDevices)gipa(instance, "vkEnumeratePhysicalDevices");
    if (!enumPD) {
        TLOG("FAIL: vkEnumeratePhysicalDevices not found\n");
        return 1;
    }
    TLOG("  Function pointer: %p\n", (void*)enumPD);
    TLOG("  Calling with pDevices=NULL...\n");

    uint32_t pdCount = 0;
    res = enumPD(instance, &pdCount, NULL);
    marker("enum_pd_count_done");

    if (res != 0) {
        TLOG("FAIL: vkEnumeratePhysicalDevices (count) returned %d\n", res);
        return 1;
    }
    TLOG("OK: %u physical device(s)\n", pdCount);

    /* Step 5: Get actual devices */
    if (pdCount > 0) {
        TLOG("Step 5: vkEnumeratePhysicalDevices (GET DEVICES)...\n");
        marker("enum_pd_get_start");

        VkPhysicalDevice* pds = calloc(pdCount, sizeof(VkPhysicalDevice));
        res = enumPD(instance, &pdCount, pds);
        marker("enum_pd_get_done");

        if (res != 0) {
            TLOG("FAIL: returned %d\n", res);
        } else {
            for (uint32_t i = 0; i < pdCount; i++)
                TLOG("  Device[%u]: %p\n", i, pds[i]);
        }
        free(pds);
    }

    /* Step 6: Cleanup */
    TLOG("Step 6: Destroying instance...\n");
    marker("destroy_start");
    PFN_vkDestroyInstance destroyInstance =
        (PFN_vkDestroyInstance)gipa(instance, "vkDestroyInstance");
    if (destroyInstance) destroyInstance(instance, NULL);
    marker("destroy_done");

    TLOG("=== TEST PASSED ===\n");
    marker("done");
    dlclose(vk);
    return 0;
}
