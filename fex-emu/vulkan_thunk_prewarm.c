/*
 * vulkan_thunk_prewarm.c — LD_PRELOAD shim to pre-load the FEX Vulkan thunk
 *
 * Problem: FEX's guest thunk (libvulkan-guest.so) crashes with SIGILL when
 * loaded LATE in the Wine/game process lifecycle, after DXVK and many DLLs
 * have been mapped into the address space.
 *
 * Solution: Load the guest thunk EARLY via LD_PRELOAD constructor, before
 * Wine's preloader and game DLLs fragment the address space. The thunk
 * initialization (JIT bridge setup) succeeds in a clean memory layout.
 *
 * Build (x86-64):
 *   x86_64-linux-gnu-gcc -shared -fPIC -O2 -o libvulkan_thunk_prewarm.so \
 *     vulkan_thunk_prewarm.c -ldl
 *
 * Usage:
 *   export LD_PRELOAD="/usr/lib/libvulkan_thunk_prewarm.so"
 *   wine64 game.exe
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

#define LOG(...) do { fprintf(stderr, "THUNK_PREWARM: " __VA_ARGS__); fflush(stderr); } while(0)

static void *thunk_handle = NULL;

__attribute__((constructor))
static void prewarm_thunk(void) {
    LOG("Pre-loading FEX Vulkan guest thunk (early init)...\n");

    const char *paths[] = {
        "/opt/fex/share/fex-emu/GuestThunks/libvulkan-guest.so",
        NULL
    };

    for (int i = 0; paths[i]; i++) {
        LOG("Trying: %s\n", paths[i]);
        thunk_handle = dlopen(paths[i], RTLD_NOW | RTLD_GLOBAL);
        if (thunk_handle) {
            LOG("SUCCESS: Thunk pre-loaded from %s\n", paths[i]);

            /* Verify it has the expected entry point */
            void *gipa = dlsym(thunk_handle, "vkGetInstanceProcAddr");
            LOG("vkGetInstanceProcAddr=%p\n", gipa);
            return;
        }
        LOG("Failed: %s\n", dlerror());
    }

    LOG("WARNING: Could not pre-load thunk (will fall back to lazy init)\n");
}
