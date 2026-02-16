/*
 * vulkan_dlopen_diag.c — LD_PRELOAD diagnostic shim for Wine/DXVK Vulkan debugging
 *
 * Purpose: Hook dlopen() to log exactly what paths Wine uses when loading Vulkan.
 * This reveals whether FEX's thunk overlay intercepts Wine's dlopen calls.
 *
 * Build (x86-64 cross-compile):
 *   docker run --rm -v ./fex-emu:/src ubuntu:22.04 bash -c \
 *     "apt-get update && apt-get install -y gcc-x86-64-linux-gnu && \
 *      x86_64-linux-gnu-gcc -shared -fPIC -O2 -o /src/libvulkan_dlopen_diag.so \
 *      /src/vulkan_dlopen_diag.c -ldl"
 *
 * Usage inside FEX guest:
 *   export LD_PRELOAD="/usr/lib/libvulkan_dlopen_diag.so"
 *   wine64 notepad
 *
 * Output: DLOPEN_DIAG: messages on stderr showing each dlopen call for Vulkan-related libs.
 *
 * Interpretation:
 *   - "result=0xNNN err=OK" → FEX overlay IS intercepting, problem is downstream
 *   - "result=(nil) err=..." → FEX overlay NOT intercepting, need path redirect
 *   - No DLOPEN_DIAG messages → Wine doesn't use dlopen, or LD_PRELOAD is blocked
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <link.h>

static void* (*real_dlopen)(const char*, int) = NULL;

/* Log all Vulkan-related dlopen calls */
void* dlopen(const char* filename, int flags) {
    if (!real_dlopen) {
        real_dlopen = dlsym(RTLD_NEXT, "dlopen");
        if (!real_dlopen) {
            fprintf(stderr, "DLOPEN_DIAG: FATAL — cannot find real dlopen!\n");
            return NULL;
        }
    }

    /* Log ALL dlopen calls that mention vulkan, vk, mesa, or ICD-related names */
    if (filename && (strstr(filename, "vulkan") || strstr(filename, "Vulkan") ||
                     strstr(filename, "libvk") || strstr(filename, "mesa") ||
                     strstr(filename, "vortek") || strstr(filename, "dxvk") ||
                     strstr(filename, "d3d") || strstr(filename, "wined3d"))) {

        fprintf(stderr, "DLOPEN_DIAG: dlopen(\"%s\", 0x%x)\n", filename, flags);
        fflush(stderr);

        void* result = real_dlopen(filename, flags);
        const char* err = result ? "OK" : dlerror();
        fprintf(stderr, "DLOPEN_DIAG:   result=%p err=%s\n", result, err ? err : "(null)");

        if (result) {
            /* Check if the loaded library has Vulkan entry points */
            void* gipa = dlsym(result, "vkGetInstanceProcAddr");
            void* icd_gipa = dlsym(result, "vk_icdGetInstanceProcAddr");
            void* negotiate = dlsym(result, "vk_icdNegotiateLoaderICDInterfaceVersion");
            fprintf(stderr, "DLOPEN_DIAG:   vkGetInstanceProcAddr=%p\n", gipa);
            fprintf(stderr, "DLOPEN_DIAG:   vk_icdGetInstanceProcAddr=%p\n", icd_gipa);
            fprintf(stderr, "DLOPEN_DIAG:   vk_icdNegotiateLoaderICDInterfaceVersion=%p\n", negotiate);

            /* Try to get the actual file path of the loaded library */
            struct link_map *lm = NULL;
            if (dlinfo(result, RTLD_DI_LINKMAP, &lm) == 0 && lm) {
                fprintf(stderr, "DLOPEN_DIAG:   actual_path=%s\n", lm->l_name ? lm->l_name : "(unknown)");
            }
        }

        fprintf(stderr, "DLOPEN_DIAG: ---\n");
        fflush(stderr);
        return result;
    }

    return real_dlopen(filename, flags);
}

/* Constructor: announce that we're loaded */
__attribute__((constructor))
static void init(void) {
    fprintf(stderr, "DLOPEN_DIAG: === Vulkan dlopen diagnostic shim loaded ===\n");
    fprintf(stderr, "DLOPEN_DIAG: Monitoring dlopen() calls for vulkan/mesa/dxvk libs\n");
    fflush(stderr);
}
