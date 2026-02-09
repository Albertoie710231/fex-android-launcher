/*
 * Vortek ICD Wrapper for Vulkan ICD Loader
 *
 * Problem: libvulkan_vortek.so (from Winlator) exports vk_icdGetInstanceProcAddr
 * but it returns NULL for all functions because vortekInitOnce() is never called
 * during the standard ICD loader protocol. Winlator loads the library directly.
 *
 * Solution: This thin wrapper library acts as a proper Vulkan ICD:
 * 1. Loads libvulkan_vortek.so via dlopen
 * 2. Calls vortekInitOnce() to establish the socket connection to VortekRenderer
 * 3. Implements vk_icdGetInstanceProcAddr that maps "vkFoo" -> dlsym("vt_call_vkFoo")
 *
 * The ICD loader calls our vk_icdGetInstanceProcAddr("vkCreateInstance") and we
 * return the address of vt_call_vkCreateInstance from libvulkan_vortek.so.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* Vulkan types we need */
typedef void (*PFN_vkVoidFunction)(void);
typedef uint32_t VkResult;

static void *vortek_lib = NULL;
static int init_done = 0;
static int init_ok = 0;

static void log_msg(const char *msg) {
    fprintf(stderr, "vortek_icd_wrapper: %s\n", msg);
}

static void ensure_init(void) {
    if (init_done) return;
    init_done = 1;

    /* Load the real Vortek library from the same directory or LD_LIBRARY_PATH */
    vortek_lib = dlopen("libvulkan_vortek.so", RTLD_NOW);
    if (!vortek_lib) {
        /* Try with full path from environment */
        const char *nativelib = getenv("FEX_VORTEK_NATIVELIB");
        if (nativelib) {
            char path[512];
            snprintf(path, sizeof(path), "%s/libvulkan_vortek.so", nativelib);
            vortek_lib = dlopen(path, RTLD_NOW);
        }
    }
    if (!vortek_lib) {
        char buf[512];
        snprintf(buf, sizeof(buf), "failed to load libvulkan_vortek.so: %s", dlerror());
        log_msg(buf);
        return;
    }
    log_msg("loaded libvulkan_vortek.so");

    /* Call vortekInitOnce to establish the socket connection to VortekRenderer */
    void (*initFn)(void) = (void (*)(void))dlsym(vortek_lib, "vortekInitOnce");
    if (initFn) {
        log_msg("calling vortekInitOnce...");
        initFn();
        log_msg("vortekInitOnce done");
    } else {
        log_msg("WARNING: vortekInitOnce not found!");
    }

    init_ok = 1;
}

/*
 * Standard ICD interface: negotiate loader/ICD interface version.
 * We support version 5 (latest as of Vulkan 1.3).
 */
__attribute__((visibility("default")))
VkResult vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pVersion) {
    log_msg("vk_icdNegotiateLoaderICDInterfaceVersion called");
    if (*pVersion > 5) {
        *pVersion = 5;
    }
    return 0; /* VK_SUCCESS */
}

/*
 * Standard ICD interface: get Vulkan function pointers.
 *
 * The ICD loader calls this with function names like "vkCreateInstance".
 * We look up "vt_call_vkCreateInstance" in libvulkan_vortek.so and return it.
 *
 * This is the key fix: the original Vortek ICD's vk_icdGetInstanceProcAddr
 * returns NULL because it was never designed for the ICD loader protocol.
 * Our wrapper bridges this gap by prepending "vt_call_" to the function name.
 */
__attribute__((visibility("default")))
PFN_vkVoidFunction vk_icdGetInstanceProcAddr(void *instance, const char *pName) {
    ensure_init();
    if (!init_ok || !vortek_lib || !pName) return NULL;

    /* Map "vkFoo" -> "vt_call_vkFoo" */
    char buf[300];
    snprintf(buf, sizeof(buf), "vt_call_%s", pName);

    PFN_vkVoidFunction fn = (PFN_vkVoidFunction)dlsym(vortek_lib, buf);

    /* Debug logging for key functions */
    if (strstr(pName, "CreateInstance") || strstr(pName, "ProcAddr") ||
        strstr(pName, "EnumerateInstance") || strstr(pName, "EnumeratePhysical")) {
        char msg[512];
        snprintf(msg, sizeof(msg), "  %s -> %s = %p", pName, buf, (void*)fn);
        log_msg(msg);
    }

    return fn;
}
