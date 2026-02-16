/*
 * FEX Thunk ICD Shim (x86-64 guest side)
 *
 * Minimal Vulkan ICD that loads the FEX Vulkan thunk guest library from
 * ThunkGuestLibs. When loaded from that path, FEX activates the thunk
 * mechanism and bridges calls to the HOST Vulkan loader.
 *
 * Why: Wine's winevulkan.so does dlopen("libvulkan.so.1") at runtime.
 * FEX's Vulkan thunk only intercepts DT_NEEDED loads, not runtime dlopen.
 * Wine gets the real Mesa loader, which needs an x86-64 ICD.
 * This shim IS that ICD — it bridges to the FEX thunk.
 *
 * Chain: Wine → Mesa loader → THIS SHIM → FEX thunk → host loader → Vortek
 *
 * Build: x86_64-linux-gnu-gcc -shared -fPIC -O2 -o libfex_thunk_icd.so
 *        fex_thunk_icd.c -ldl
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

typedef void (*PFN_vkVoidFunction)(void);
typedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddr)(void*, const char*);
typedef int VkResult;

static void* thunk_lib = NULL;
static PFN_vkGetInstanceProcAddr real_gipa = NULL;
static int init_done = 0;

/* Instance handle saved during vkCreateInstance */
static void* saved_instance = NULL;

#define LOG(...) do { fprintf(stderr, "fex_thunk_icd: " __VA_ARGS__); fflush(stderr); } while(0)

/* File-based debug markers — survives even if stderr is lost */
static void icd_marker(const char* msg) {
    FILE* f = fopen("/tmp/icd_trace.log", "a");
    if (f) { fprintf(f, "%s\n", msg); fclose(f); }
}

static void ensure_init(void) {
    if (init_done) return;
    init_done = 1;
    icd_marker("ensure_init");

    const char* paths[] = {
        "/opt/fex/share/fex-emu/GuestThunks/libvulkan-guest.so",
        "/opt/fex/share/fex-emu/GuestThunks_32/libvulkan-guest.so",
        NULL
    };

    for (int i = 0; paths[i]; i++) {
        LOG("Trying: %s\n", paths[i]);
        icd_marker(paths[i]);
        thunk_lib = dlopen(paths[i], RTLD_NOW | RTLD_LOCAL);
        if (thunk_lib) {
            LOG("Loaded FEX thunk from: %s\n", paths[i]);
            icd_marker("thunk_loaded");
            break;
        }
        LOG("Failed: %s\n", dlerror());
    }

    if (!thunk_lib) {
        LOG("ERROR: Could not load FEX Vulkan thunk!\n");
        icd_marker("thunk_load_FAILED");
        return;
    }

    real_gipa = (PFN_vkGetInstanceProcAddr)dlsym(thunk_lib, "vkGetInstanceProcAddr");
    if (!real_gipa) {
        LOG("ERROR: vkGetInstanceProcAddr not found in thunk!\n");
        icd_marker("gipa_not_found");
        return;
    }
    LOG("vkGetInstanceProcAddr resolved: %p\n", (void*)real_gipa);
    icd_marker("init_done_ok");
}

/* ---- Wrapper: vkCreateInstance ---- */

typedef VkResult (*PFN_vkCreateInstance)(const void*, const void*, void**);
static PFN_vkCreateInstance real_create_instance = NULL;

static VkResult wrapped_CreateInstance(const void* pCreateInfo, const void* pAllocator, void** pInstance) {
    LOG(">> vkCreateInstance (wrapper)\n");
    icd_marker("CreateInstance_ENTER");

    if (!real_create_instance) {
        LOG("ERROR: real_create_instance is NULL!\n");
        icd_marker("CreateInstance_NULL");
        return -3; /* VK_ERROR_INITIALIZATION_FAILED */
    }

    VkResult res = real_create_instance(pCreateInfo, pAllocator, pInstance);
    char buf[128];
    snprintf(buf, sizeof(buf), "CreateInstance_result=%d instance=%p", res, pInstance ? *pInstance : NULL);
    icd_marker(buf);
    LOG("<< vkCreateInstance returned %d, instance=%p\n", res, pInstance ? *pInstance : NULL);

    if (res == 0 && pInstance && *pInstance) {
        saved_instance = *pInstance;
    }
    return res;
}

/* ---- Wrapper: vkEnumeratePhysicalDevices ---- */

typedef VkResult (*PFN_vkEnumPD)(void*, uint32_t*, void**);
static PFN_vkEnumPD real_enum_pd = NULL;

static VkResult wrapped_EnumeratePhysicalDevices(void* instance, uint32_t* pCount, void** pDevices) {
    char buf[256];
    snprintf(buf, sizeof(buf), "EnumPD_ENTER instance=%p pDevices=%p saved=%p",
             instance, (void*)pDevices, saved_instance);
    LOG(">> %s\n", buf);
    icd_marker(buf);

    if (!real_enum_pd) {
        LOG("ERROR: real_enum_pd is NULL!\n");
        icd_marker("EnumPD_NULL");
        return -3;
    }

    LOG("   Calling thunk vkEnumeratePhysicalDevices @ %p...\n", (void*)real_enum_pd);
    icd_marker("EnumPD_CALL_THUNK");

    VkResult res = real_enum_pd(instance, pCount, pDevices);

    snprintf(buf, sizeof(buf), "EnumPD_result=%d count=%u", res, pCount ? *pCount : 0);
    LOG("<< %s\n", buf);
    icd_marker(buf);
    return res;
}

/* ---- Wrapper: vkGetDeviceProcAddr ---- */
/* FEX thunks' vkGetDeviceProcAddr only returns ~6 device functions.
 * All others return NULL even though they ARE thunked and accessible via GIPA.
 * Wine's winevulkan calls GDPA for ALL device functions to build its dispatch
 * table, so NULL entries cause an assertion crash.
 * Fix: fall back to GIPA(instance, name) when GDPA returns NULL. */

typedef PFN_vkVoidFunction (*PFN_vkGetDeviceProcAddr)(void*, const char*);
static PFN_vkGetDeviceProcAddr real_gdpa = NULL;

static PFN_vkVoidFunction wrapped_GetDeviceProcAddr(void* device, const char* pName) {
    /* FEX thunks' real GDPA crashes (segfault) for most device functions
     * (e.g., vkQueueSubmit). Only ~6 functions work via GDPA.
     * Use GIPA exclusively — it returns valid pointers for ALL device functions
     * via instance-level dispatch. This is how vkcube/vulkaninfo work. */
    PFN_vkVoidFunction fn = NULL;
    (void)device; /* Not used — GIPA uses saved_instance instead */

    if (real_gipa && saved_instance)
        fn = real_gipa(saved_instance, pName);

    return fn;
}

/* ---- Wrapper: vkDestroyInstance ---- */

typedef void (*PFN_vkDestroyInstance)(void*, const void*);
static PFN_vkDestroyInstance real_destroy_instance = NULL;

static void wrapped_DestroyInstance(void* instance, const void* pAllocator) {
    LOG(">> vkDestroyInstance(%p)\n", instance);
    icd_marker("DestroyInstance_ENTER");
    if (real_destroy_instance) real_destroy_instance(instance, pAllocator);
    icd_marker("DestroyInstance_DONE");
    saved_instance = NULL;
    real_gdpa = NULL;
}

/* ICD protocol entry points */

__attribute__((visibility("default")))
uint32_t vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pVersion) {
    ensure_init();
    LOG("NegotiateVersion: %u\n", *pVersion);
    icd_marker("NegotiateVersion");
    if (*pVersion > 5) *pVersion = 5;
    return 0; /* VK_SUCCESS */
}

__attribute__((visibility("default")))
PFN_vkVoidFunction vk_icdGetInstanceProcAddr(void *instance, const char *pName) {
    ensure_init();
    if (!real_gipa || !pName) return NULL;

    /* Intercept specific functions to add tracing wrappers */
    if (strcmp(pName, "vkCreateInstance") == 0) {
        real_create_instance = (PFN_vkCreateInstance)real_gipa(instance, pName);
        LOG("GIPA: vkCreateInstance -> real=%p, wrapper=%p\n",
            (void*)real_create_instance, (void*)wrapped_CreateInstance);
        icd_marker("GIPA_vkCreateInstance");
        return (PFN_vkVoidFunction)wrapped_CreateInstance;
    }

    if (strcmp(pName, "vkEnumeratePhysicalDevices") == 0) {
        real_enum_pd = (PFN_vkEnumPD)real_gipa(instance, pName);
        LOG("GIPA: vkEnumeratePhysicalDevices -> real=%p, wrapper=%p\n",
            (void*)real_enum_pd, (void*)wrapped_EnumeratePhysicalDevices);
        icd_marker("GIPA_vkEnumeratePhysicalDevices");
        return (PFN_vkVoidFunction)wrapped_EnumeratePhysicalDevices;
    }

    if (strcmp(pName, "vkDestroyInstance") == 0) {
        real_destroy_instance = (PFN_vkDestroyInstance)real_gipa(instance, pName);
        LOG("GIPA: vkDestroyInstance -> real=%p, wrapper=%p\n",
            (void*)real_destroy_instance, (void*)wrapped_DestroyInstance);
        return (PFN_vkVoidFunction)wrapped_DestroyInstance;
    }

    if (strcmp(pName, "vkGetDeviceProcAddr") == 0) {
        real_gdpa = (PFN_vkGetDeviceProcAddr)real_gipa(instance, pName);
        LOG("GIPA: vkGetDeviceProcAddr -> real=%p, wrapper=%p\n",
            (void*)real_gdpa, (void*)wrapped_GetDeviceProcAddr);
        icd_marker("GIPA_vkGetDeviceProcAddr");
        return (PFN_vkVoidFunction)wrapped_GetDeviceProcAddr;
    }

    /* Everything else: pass through directly */
    PFN_vkVoidFunction fn = real_gipa(instance, pName);
    /* Only log non-spammy functions */
    if (strncmp(pName, "vkCreate", 8) == 0 ||
        strncmp(pName, "vkEnum", 6) == 0 ||
        strncmp(pName, "vkGet", 5) == 0) {
        LOG("GIPA: %s -> %p\n", pName, (void*)fn);
    }
    return fn;
}

__attribute__((visibility("default")))
void* vk_icdGetPhysicalDeviceProcAddr(void *instance, const char *pName) {
    /* Let the loader handle physical device dispatch */
    (void)instance; (void)pName;
    return NULL;
}
