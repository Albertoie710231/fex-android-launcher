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

static void* thunk_lib = NULL;
static PFN_vkGetInstanceProcAddr real_gipa = NULL;
static int init_done = 0;

#define LOG(...) do { fprintf(stderr, "fex_thunk_icd: " __VA_ARGS__); fflush(stderr); } while(0)

static void ensure_init(void) {
    if (init_done) return;
    init_done = 1;

    /* Load the FEX Vulkan thunk from ThunkGuestLibs.
     * FEX should recognize this path and activate the thunk mechanism,
     * setting up the host-side bridge (libvulkan-host.so). */
    const char* paths[] = {
        "/opt/fex/share/fex-emu/GuestThunks/libvulkan-guest.so",
        "/opt/fex/share/fex-emu/GuestThunks_32/libvulkan-guest.so",
        NULL
    };

    for (int i = 0; paths[i]; i++) {
        LOG("Trying: %s\n", paths[i]);
        thunk_lib = dlopen(paths[i], RTLD_NOW | RTLD_LOCAL);
        if (thunk_lib) {
            LOG("Loaded FEX thunk from: %s\n", paths[i]);
            break;
        }
        LOG("Failed: %s\n", dlerror());
    }

    if (!thunk_lib) {
        LOG("ERROR: Could not load FEX Vulkan thunk!\n");
        return;
    }

    real_gipa = (PFN_vkGetInstanceProcAddr)dlsym(thunk_lib, "vkGetInstanceProcAddr");
    if (!real_gipa) {
        LOG("ERROR: vkGetInstanceProcAddr not found in thunk!\n");
        return;
    }
    LOG("vkGetInstanceProcAddr resolved: %p\n", (void*)real_gipa);
}

/* ICD protocol entry points */

__attribute__((visibility("default")))
uint32_t vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pVersion) {
    ensure_init();
    LOG("NegotiateVersion: %u\n", *pVersion);
    if (*pVersion > 5) *pVersion = 5;
    return 0; /* VK_SUCCESS */
}

__attribute__((visibility("default")))
PFN_vkVoidFunction vk_icdGetInstanceProcAddr(void *instance, const char *pName) {
    ensure_init();
    if (!real_gipa || !pName) return NULL;
    return real_gipa(instance, pName);
}

__attribute__((visibility("default")))
void* vk_icdGetPhysicalDeviceProcAddr(void *instance, const char *pName) {
    /* Let the loader handle physical device dispatch */
    (void)instance; (void)pName;
    return NULL;
}
