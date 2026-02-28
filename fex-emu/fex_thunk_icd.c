/*
 * FEX Thunk ICD Shim — Handle Wrapper Architecture (Thread-Safe)
 *
 * Replaces dispatch-swapping trampolines with handle wrappers.
 * Instead of temporarily modifying *(void**)device (which races with
 * concurrent threads), return wrapper handles where:
 *   offset 0: loader_dispatch  (written by loader/layers, harmless)
 *   offset 8: real_handle      (thunk handle, immutable after creation)
 *
 * All device-level functions unwrap the first arg (read offset 8) before
 * calling the thunk. No locks, no dispatch swapping, fully thread-safe.
 *
 * Build: x86_64-linux-gnu-gcc -shared -fPIC -O2 -o libfex_thunk_icd.so \
 *        fex_thunk_icd.c -ldl
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/mman.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

typedef void (*PFN_vkVoidFunction)(void);
typedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddr)(void*, const char*);
typedef int VkResult;

static void* thunk_lib = NULL;
static PFN_vkGetInstanceProcAddr real_gipa = NULL;
static int init_done = 0;
static void* saved_instance = NULL;

/* File-based logging: stderr may not be captured, so also write to /tmp/icd_debug.txt */
static FILE* g_icd_log = NULL;
static void icd_log_init(void) {
    if (!g_icd_log) {
        g_icd_log = fopen("/tmp/icd_debug.txt", "a");
        if (g_icd_log) {
            fprintf(g_icd_log, "=== ICD LOG START (pid=%d) ===\n", getpid());
            fflush(g_icd_log);
        }
    }
}
static void log_timestamp(FILE* f) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    fprintf(f, "[%02d:%02d:%02d.%03ld] ", tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
}
#define LOG(...) do { \
    log_timestamp(stderr); fprintf(stderr, "fex_thunk_icd: " __VA_ARGS__); fflush(stderr); \
    icd_log_init(); \
    if (g_icd_log) { log_timestamp(g_icd_log); fprintf(g_icd_log, "fex_thunk_icd: " __VA_ARGS__); fflush(g_icd_log); } \
} while(0)

/* Forward declaration: injected extensions list (defined near extension filter) */
static const char* g_injected_extensions[];

/* ==== Handle Wrapper ====
 *
 * 16-byte struct that stands in for dispatchable handles (VkDevice, VkQueue,
 * VkCommandBuffer). The Vulkan loader writes its dispatch table to offset 0.
 * We store the real thunk handle at offset 8, never touched by anyone else.
 *
 * Thread safety: offset 8 is write-once (set at creation). Multiple threads
 * can read it concurrently with zero synchronization.
 */

typedef struct {
    void* loader_dispatch;  /* offset 0: loader/layers write here */
    void* real_handle;      /* offset 8: real thunk handle (immutable) */
} HandleWrapper;

static HandleWrapper* wrap_handle(void* real_handle) {
    HandleWrapper* w = (HandleWrapper*)malloc(sizeof(HandleWrapper));
    if (!w) {
        LOG("wrap_handle: malloc failed!\n");
        return NULL;
    }
    w->loader_dispatch = NULL;
    w->real_handle = real_handle;
    return w;
}

static inline void* unwrap(void* wrapper) {
    if (!wrapper) return NULL;
    return ((HandleWrapper*)wrapper)->real_handle;
}

static void free_wrapper(void* wrapper) {
    free(wrapper);
}

/* ==== Unwrap Trampoline Generator ====
 *
 * 16-byte x86-64 code stub that unwraps the first argument (reads real
 * handle from wrapper offset 8) and tail-calls the real function.
 * All other arguments (rsi, rdx, rcx, r8, r9, stack) are preserved.
 *
 * Assembly:
 *   mov rdi, [rdi + 8]       ; unwrap: load real handle from offset 8
 *   movabs rax, <real_func>  ; load target function address
 *   jmp rax                  ; tail call
 */

#define TRAMPOLINE_SIZE 16

static uint8_t* tramp_pages[64] = {0};
static int tramp_page_idx = 0;
static int tramp_offset = 0;

static PFN_vkVoidFunction make_unwrap_trampoline(PFN_vkVoidFunction real_func) {
    if (!tramp_pages[tramp_page_idx] || tramp_offset + TRAMPOLINE_SIZE > 4096) {
        void* page = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED) {
            LOG("make_unwrap_trampoline: mmap failed!\n");
            return real_func;
        }
        if (tramp_page_idx < 63)
            tramp_pages[++tramp_page_idx] = page;
        else
            tramp_pages[tramp_page_idx] = page;
        tramp_offset = 0;
    }

    uint8_t* c = tramp_pages[tramp_page_idx] + tramp_offset;

    /* mov rdi, [rdi + 8]  (4 bytes) */
    c[0] = 0x48; c[1] = 0x8B; c[2] = 0x7F; c[3] = 0x08;
    /* movabs rax, imm64   (10 bytes) */
    c[4] = 0x48; c[5] = 0xB8;
    memcpy(c + 6, &real_func, 8);
    /* jmp rax              (2 bytes) */
    c[14] = 0xFF; c[15] = 0xE0;

    tramp_offset += TRAMPOLINE_SIZE;
    return (PFN_vkVoidFunction)c;
}

/* ==== Init ==== */

static void ensure_init(void) {
    if (init_done) return;
    init_done = 1;

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
    LOG("Init OK: gipa=%p\n", (void*)real_gipa);
}

/* ==== Virtual Heap Split ====
 *
 * Mali unified memory: the single heap is both DEVICE_LOCAL and HOST_VISIBLE.
 * Vortek/FEX thunks have a mapping limit (~174MB observed for vkMapMemory).
 *
 * Problem: capping the heap size makes DXVK think there's not enough VRAM
 * and it refuses to create a D3D11 device.
 *
 * Solution: split the unified heap into two virtual heaps:
 *   - Big heap (original size): for DEVICE_LOCAL-only allocations (textures)
 *   - Small heap (capped): for HOST_VISIBLE allocations (staging buffers)
 *
 * On fully-unified GPUs where ALL memory types are HOST_VISIBLE, we also add
 * a new DEVICE_LOCAL-only memory type pointing to the big heap. Memory
 * requirements are patched to include this type, and AllocateMemory remaps
 * the virtual type index back to the original for the real driver.
 *
 * VkPhysicalDeviceMemoryProperties layout (x86-64):
 * offset 0:   memoryTypeCount (uint32_t)
 * offset 4:   memoryTypes[32] (each 8 bytes: propertyFlags(4) + heapIndex(4))
 * offset 260: memoryHeapCount (uint32_t)
 * offset 264: memoryHeaps[16] (each 16 bytes: size(8) + flags(4) + pad(4))
 *
 * VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT = 0x02
 * VK_MEMORY_PROPERTY_HOST_COHERENT_BIT = 0x04
 * VK_MEMORY_PROPERTY_HOST_CACHED_BIT = 0x08
 * VK_MEMORY_HEAP_DEVICE_LOCAL_BIT = 0x01
 */

#define STAGING_HEAP_CAP (512ULL * 1024 * 1024)  /* 512 MiB — generous; real limit is Vortek/Mali, not memory */
#define MAP_BYTE_LIMIT  (4096ULL * 1024 * 1024) /* 4 GiB — must exceed ALLOC_BYTE_CAP to prevent fake maps */
#define ALLOC_BYTE_CAP  (2048ULL * 1024 * 1024)  /* 2 GiB — BC→RGBA 4x memory increase */

/* Tracks the virtual type we added so other wrappers can patch accordingly */
static int g_added_type_index = -1;    /* index of added DEVICE_LOCAL-only type, -1 if none */
static int g_remap_to_type = -1;       /* real type index to use for virtual type allocations */

static void split_unified_heaps(uint8_t* p) {
    uint32_t* pTypeCount = (uint32_t*)(p + 0);
    uint32_t typeCount = *pTypeCount;
    uint32_t* pHeapCount = (uint32_t*)(p + 260);
    uint32_t heapCount = *pHeapCount;

    if (typeCount == 0 || heapCount == 0) return;

    LOG("HeapSplit: ENTRY typeCount=%u heapCount=%u\n", typeCount, heapCount);
    for (uint32_t i = 0; i < typeCount && i < 32; i++) {
        uint32_t tf = *(uint32_t*)(p + 4 + i * 8);
        uint32_t th = *(uint32_t*)(p + 4 + i * 8 + 4);
        LOG("  type[%u] flags=0x%x heap=%u\n", i, tf, th);
    }
    for (uint32_t h2 = 0; h2 < heapCount; h2++) {
        uint64_t hs = *(uint64_t*)(p + 264 + h2 * 16);
        uint32_t hf = *(uint32_t*)(p + 264 + h2 * 16 + 8);
        LOG("  heap[%u] size=%lluMB flags=0x%x\n", h2, (unsigned long long)(hs/(1024*1024)), hf);
    }

    for (uint32_t h = 0; h < heapCount; h++) {
        uint64_t heapSize = *(uint64_t*)(p + 264 + h * 16);
        uint32_t heapFlags = *(uint32_t*)(p + 264 + h * 16 + 8);

        if (!(heapFlags & 0x01)) continue;  /* skip non-DEVICE_LOCAL heaps */
        if (heapSize <= STAGING_HEAP_CAP) continue;  /* already small enough */

        /* Count HOST_VISIBLE vs non-HOST_VISIBLE types for this heap.
         * LAZILY_ALLOCATED (0x10) types are NOT usable for regular allocations
         * (only for transient framebuffer attachments), so don't count them. */
        int hv_count = 0, usable_non_hv_count = 0;
        int first_hv_type = -1;
        for (uint32_t i = 0; i < typeCount && i < 32; i++) {
            uint32_t tflags = *(uint32_t*)(p + 4 + i * 8);
            uint32_t theap = *(uint32_t*)(p + 4 + i * 8 + 4);
            if (theap != h) continue;
            if (tflags & 0x02) {  /* HOST_VISIBLE_BIT */
                hv_count++;
                if (first_hv_type < 0) first_hv_type = (int)i;
            } else if (!(tflags & 0x10)) {  /* not LAZILY_ALLOCATED */
                usable_non_hv_count++;
            }
        }

        if (hv_count == 0) continue;  /* no HOST_VISIBLE types on this heap */
        if (*pHeapCount >= 16) continue;  /* can't add more heaps */

        /* Create new capped heap for HOST_VISIBLE (staging) allocations */
        uint32_t stagingHeap = *pHeapCount;
        *(uint64_t*)(p + 264 + stagingHeap * 16) = STAGING_HEAP_CAP;
        *(uint32_t*)(p + 264 + stagingHeap * 16 + 8) = heapFlags;
        (*pHeapCount)++;

        /* Redirect all HOST_VISIBLE types to the new capped heap */
        for (uint32_t i = 0; i < typeCount && i < 32; i++) {
            uint32_t tflags = *(uint32_t*)(p + 4 + i * 8);
            uint32_t* theap = (uint32_t*)(p + 4 + i * 8 + 4);
            if (*theap == h && (tflags & 0x02)) {
                *theap = stagingHeap;
                LOG("HeapSplit: type[%u] flags=0x%x -> staging heap %u (%lluMB)\n",
                    i, tflags, stagingHeap,
                    (unsigned long long)(STAGING_HEAP_CAP / (1024*1024)));
            }
        }

        LOG("HeapSplit: usable_non_hv=%d hv=%d typeCount=%u first_hv=%d\n",
            usable_non_hv_count, hv_count, *pTypeCount, first_hv_type);

        if (usable_non_hv_count == 0 && *pTypeCount < 32) {
            /* ALL types are HOST_VISIBLE (fully unified memory).
             * Add a type pointing to the big (uncapped) heap so DXVK sees
             * enough "VRAM" and doesn't refuse to create a D3D11 device.
             * Keep HOST_VISIBLE so DXVK can Map buffers directly instead of
             * using staging copies — on Mali there's no real VRAM anyway.
             * Without HOST_VISIBLE, DXVK uses vkCmdCopyBuffer for constant
             * buffers, which can fail if barrier v2->v1 drops sync. */
            uint32_t newIdx = *pTypeCount;
            uint32_t origFlags = *(uint32_t*)(p + 4 + first_hv_type * 8);
            uint32_t newFlags = origFlags;  /* keep HOST_VISIBLE+HOST_COHERENT */
            *(uint32_t*)(p + 4 + newIdx * 8) = newFlags;
            *(uint32_t*)(p + 4 + newIdx * 8 + 4) = h;  /* original big heap */
            (*pTypeCount)++;

            g_added_type_index = (int)newIdx;
            g_remap_to_type = first_hv_type;

            LOG("HeapSplit: added type[%u] flags=0x%x -> heap %u (%lluMB) [HOST_VISIBLE + DEVICE_LOCAL]\n",
                newIdx, newFlags, h,
                (unsigned long long)(heapSize / (1024*1024)));
        }

        LOG("HeapSplit: heap[%u]=%lluMB (textures), heap[%u]=%lluMB (staging)\n",
            h, (unsigned long long)(heapSize / (1024*1024)),
            stagingHeap, (unsigned long long)(STAGING_HEAP_CAP / (1024*1024)));
        break;  /* only split the first unified heap */
    }
}

typedef void (*PFN_vkGetPhysDeviceMemProps)(void*, void*);
static PFN_vkGetPhysDeviceMemProps real_get_mem_props = NULL;

static void wrapped_GetPhysicalDeviceMemoryProperties(void* physDev, void* pProps) {
    real_get_mem_props(physDev, pProps);
    if (pProps) split_unified_heaps((uint8_t*)pProps);
}

typedef void (*PFN_vkGetPhysDeviceMemProps2)(void*, void*);
static PFN_vkGetPhysDeviceMemProps2 real_get_mem_props2 = NULL;

static void wrapped_GetPhysicalDeviceMemoryProperties2(void* physDev, void* pProps2) {
    real_get_mem_props2(physDev, pProps2);
    if (pProps2) split_unified_heaps((uint8_t*)pProps2 + 16);
}

/* ==== vkGetPhysicalDeviceFormatProperties wrapper ====
 *
 * FEX thunks may not correctly forward VkFormatProperties for all formats.
 * BC (Block Compression) formats are critical for DXVK game rendering.
 * If textureCompressionBC=1 is reported but format properties return 0,
 * we inject the standard BC format features so DXVK can create textures. */

typedef void (*PFN_vkGetPhysDeviceFormatProps)(void*, uint32_t, void*);
static PFN_vkGetPhysDeviceFormatProps real_get_format_props = NULL;

/* VkFormatFeatureFlagBits */
#define MY_VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT          0x0001
#define MY_VK_FORMAT_FEATURE_BLIT_SRC_BIT               0x0400
#define MY_VK_FORMAT_FEATURE_TRANSFER_SRC_BIT           0x4000
#define MY_VK_FORMAT_FEATURE_TRANSFER_DST_BIT           0x8000
#define MY_VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT 0x1000
#define MY_VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT          0x0040

/* Check if format is USCALED or SSCALED (vertex formats we emulate for Mali) */
static int is_scaled_format(uint32_t fmt) {
    switch (fmt) {
        case 11: case 12: /* R8 */
        case 18: case 19: /* R8G8 */
        case 25: case 26: /* R8G8B8 */
        case 32: case 33: /* B8G8R8 */
        case 39: case 40: /* R8G8B8A8 */
        case 46: case 47: /* B8G8R8A8 */
        case 53: case 54: /* A8B8G8R8 */
        case 60: case 61: /* A2R10G10B10 */
        case 67: case 68: /* A2B10G10R10 */
        case 72: case 73: /* R16 */
        case 79: case 80: /* R16G16 */
        case 86: case 87: /* R16G16B16 */
        case 93: case 94: /* R16G16B16A16 */
            return 1;
        default:
            return 0;
    }
}

/* Standard features for BC compressed textures (optimal tiling) */
#define BC_OPTIMAL_FEATURES (MY_VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | \
                             MY_VK_FORMAT_FEATURE_BLIT_SRC_BIT | \
                             MY_VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | \
                             MY_VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | \
                             MY_VK_FORMAT_FEATURE_TRANSFER_DST_BIT)

/* BC format range: VK_FORMAT_BC1_RGB_UNORM_BLOCK(131) through VK_FORMAT_BC7_SRGB_BLOCK(146) */
static int is_bc_format(uint32_t format) {
    return format >= 131 && format <= 146;
}

/* BC→RGBA format substitution: Mali doesn't support BC formats.
 * Map BC formats to uncompressed equivalents so vkCreateImage succeeds.
 * SRGB variants: 132, 134, 136, 138, 146 → R8G8B8A8_SRGB (43)
 * UNORM variants: everything else → R8G8B8A8_UNORM (37) */
static uint32_t bc_to_rgba_format(uint32_t bc_fmt) {
    switch (bc_fmt) {
        case 132: case 134: case 136: case 138: case 146:
            return 43; /* VK_FORMAT_R8G8B8A8_SRGB */
        default:
            return 37; /* VK_FORMAT_R8G8B8A8_UNORM */
    }
}

/* Track BC-substituted images: image handle → original BC format */
#define BC_IMG_MAX 4096
static struct {
    uint64_t image;
    uint32_t bc_format;
    uint32_t rgba_format;
} g_bc_images[BC_IMG_MAX];
static int g_bc_img_count = 0;

static void bc_img_track(uint64_t image, uint32_t bc_fmt, uint32_t rgba_fmt) {
    if (g_bc_img_count < BC_IMG_MAX) {
        g_bc_images[g_bc_img_count].image = image;
        g_bc_images[g_bc_img_count].bc_format = bc_fmt;
        g_bc_images[g_bc_img_count].rgba_format = rgba_fmt;
        g_bc_img_count++;
    }
}

static int bc_img_lookup(uint64_t image) {
    for (int i = 0; i < g_bc_img_count; i++)
        if (g_bc_images[i].image == image)
            return i;
    return -1;
}

static int fmt_prop_call_count = 0;

static void wrapped_GetPhysicalDeviceFormatProperties(void* physDev, uint32_t format, void* pProps) {
    fmt_prop_call_count++;
    if (fmt_prop_call_count <= 5 || is_bc_format(format)) {
        LOG("FormatProperties CALLED #%d: fmt=%u pd=%p pProps=%p\n",
            fmt_prop_call_count, format, physDev, pProps);
    }

    if (real_get_format_props) {
        real_get_format_props(physDev, format, pProps);
    }

    if (pProps && is_bc_format(format)) {
        /* VkFormatProperties: { linearTilingFeatures(4), optimalTilingFeatures(4), bufferFeatures(4) } */
        uint32_t* linear  = (uint32_t*)((uint8_t*)pProps + 0);
        uint32_t* optimal = (uint32_t*)((uint8_t*)pProps + 4);
        uint32_t* buffer  = (uint32_t*)((uint8_t*)pProps + 8);

        LOG("FormatProperties: fmt=%u (BC) linear=0x%x optimal=0x%x buf=0x%x\n",
            format, *linear, *optimal, *buffer);
        if (*optimal == 0) {
            LOG("FormatProperties: fmt=%u -> INJECTING optimal=0x%x\n",
                format, BC_OPTIMAL_FEATURES);
            *optimal = BC_OPTIMAL_FEATURES;
        }
    }

    /* Report USCALED/SSCALED as supporting VERTEX_BUFFER_BIT.
     * Our pipeline creation hook emulates these via UINT/SINT + SPIR-V conversion. */
    if (pProps && is_scaled_format(format)) {
        uint32_t* buffer = (uint32_t*)((uint8_t*)pProps + 8);
        if (!(*buffer & MY_VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) {
            *buffer |= MY_VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
            LOG("FormatProperties: fmt=%u (SCALED) -> INJECTING VERTEX_BUFFER_BIT\n", format);
        }
    }
}

/* vkGetPhysicalDeviceFormatProperties2 wrapper */
typedef void (*PFN_vkGetPhysDeviceFormatProps2)(void*, uint32_t, void*);
static PFN_vkGetPhysDeviceFormatProps2 real_get_format_props2 = NULL;

static int fmt_prop2_call_count = 0;

static void wrapped_GetPhysicalDeviceFormatProperties2(void* physDev, uint32_t format, void* pProps) {
    fmt_prop2_call_count++;
    if (fmt_prop2_call_count <= 5 || is_bc_format(format)) {
        LOG("FormatProperties2 CALLED #%d: fmt=%u pd=%p\n",
            fmt_prop2_call_count, format, physDev);
    }

    if (real_get_format_props2) {
        real_get_format_props2(physDev, format, pProps);
    }

    /* VkFormatProperties2: sType(4)+pad(4)+pNext(8)+formatProperties(12) */
    if (pProps && is_bc_format(format)) {
        uint32_t* optimal = (uint32_t*)((uint8_t*)pProps + 20);
        LOG("FormatProperties2: fmt=%u (BC) optimal=0x%x\n", format, *optimal);
        if (*optimal == 0) {
            LOG("FormatProperties2: fmt=%u -> INJECTING optimal=0x%x\n",
                format, BC_OPTIMAL_FEATURES);
            *optimal = BC_OPTIMAL_FEATURES;
        }
    }

    /* Report USCALED/SSCALED as supporting VERTEX_BUFFER_BIT (emulated in pipeline creation) */
    if (pProps && is_scaled_format(format)) {
        /* formatProperties starts at offset 16 in VkFormatProperties2, bufferFeatures at +8 */
        uint32_t* buffer = (uint32_t*)((uint8_t*)pProps + 24);
        if (!(*buffer & MY_VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) {
            *buffer |= MY_VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
            LOG("FormatProperties2: fmt=%u (SCALED) -> INJECTING VERTEX_BUFFER_BIT\n", format);
        }
    }
}

/* ==== API Version Cap ====
 *
 * VK_KHR_dynamic_rendering and VK_KHR_synchronization2 have suspected FEX thunk
 * marshaling issues. We hide them as extensions (g_hidden_extensions) and zero
 * their features in GetPhysicalDeviceFeatures2 to force DXVK legacy paths.
 *
 * Note: We CANNOT cap to Vulkan 1.2 because DXVK in Proton-GE requires 1.3
 * and refuses to start with 1.2. Keep apiVersion at the real value (1.3).
 */
#define TARGET_API_VERSION 0x00FFFFFF  /* effectively disabled — never lower than real */

typedef void (*PFN_vkGetPhysDeviceProps)(void*, void*);
static PFN_vkGetPhysDeviceProps real_get_phys_dev_props = NULL;

static void wrapped_GetPhysicalDeviceProperties(void* physDev, void* pProps) {
    real_get_phys_dev_props(physDev, pProps);
    if (pProps) {
        /* VkPhysicalDeviceProperties: apiVersion at offset 0 (uint32_t) */
        uint32_t* apiVer = (uint32_t*)pProps;
        uint32_t orig = *apiVer;
        if (orig > TARGET_API_VERSION) {
            *apiVer = TARGET_API_VERSION;
            LOG("GetPhysDeviceProps: apiVersion capped 0x%x -> 0x%x (1.%d.%d -> 1.2.0)\n",
                orig, TARGET_API_VERSION,
                (orig >> 12) & 0x3FF, orig & 0xFFF);
        }
    }
}

typedef void (*PFN_vkGetPhysDeviceProps2)(void*, void*);
static PFN_vkGetPhysDeviceProps2 real_get_phys_dev_props2 = NULL;

static void wrapped_GetPhysicalDeviceProperties2(void* physDev, void* pProps2) {
    real_get_phys_dev_props2(physDev, pProps2);
    if (pProps2) {
        /* VkPhysicalDeviceProperties2: sType(4)+pad(4)+pNext(8)+properties(...)
         * apiVersion is at offset 16 (start of VkPhysicalDeviceProperties) */
        uint32_t* apiVer = (uint32_t*)((uint8_t*)pProps2 + 16);
        uint32_t orig = *apiVer;
        if (orig > TARGET_API_VERSION) {
            *apiVer = TARGET_API_VERSION;
            LOG("GetPhysDeviceProps2: apiVersion capped 0x%x -> 0x%x\n",
                orig, TARGET_API_VERSION);
        }
    }
}

/* ==== GetPhysicalDeviceFeatures2 wrapper ====
 *
 * DXVK sends a large pNext chain (Vulkan11/12/13 features + extensions).
 * FEX thunks need to marshal each struct. If a struct is unknown or the
 * chain is too deep, the thunk could hang or crash. This wrapper logs
 * entry/exit to detect hangs in the thunk layer.
 *
 * With API version capped to 1.2, DXVK shouldn't query Vulkan 1.3 features,
 * but as a safety measure we also zero out dynamicRendering and synchronization2
 * in VkPhysicalDeviceVulkan13Features if present in the pNext chain. */

typedef void (*PFN_vkGetPhysDeviceFeatures2)(void*, void*);
static PFN_vkGetPhysDeviceFeatures2 real_get_features2 = NULL;

static void wrapped_GetPhysicalDeviceFeatures2(void* physDev, void* pFeatures) {
    LOG("GetFeatures2 ENTER: pd=%p pF=%p\n", physDev, pFeatures);

    /* Walk pNext chain BEFORE the call to log what DXVK is requesting */
    if (pFeatures) {
        typedef struct { uint32_t sType; uint32_t _pad; void* pNext; } BaseS;
        BaseS* s = (BaseS*)((uint8_t*)pFeatures + 16); /* pNext at offset 16 in VkPhysicalDeviceFeatures2 */
        /* Actually: VkPhysicalDeviceFeatures2 = sType(4)+pad(4)+pNext(8)+features(VkPhysicalDeviceFeatures)
         * pNext is at offset 8 */
        s = (BaseS*)(*(void**)((uint8_t*)pFeatures + 8)); /* read pNext pointer */
        int idx = 0;
        while (s && idx < 50) {
            LOG("  pNext[%d] sType=%u (0x%x)\n", idx, s->sType, s->sType);
            s = (BaseS*)s->pNext;
            idx++;
        }
        LOG("  pNext chain: %d structs\n", idx);
    }

    if (real_get_features2) {
        LOG("GetFeatures2: calling thunk %p...\n", (void*)real_get_features2);
        real_get_features2(physDev, pFeatures);
        LOG("GetFeatures2 RETURNED OK\n");
    } else {
        LOG("GetFeatures2: real function is NULL!\n");
    }

    /* Spoof core features for D3D_FEATURE_LEVEL_11_1.
     * VkPhysicalDeviceFeatures2 layout: sType(4)+pad(4)+pNext(8)+features(...)
     * VkPhysicalDeviceFeatures offsets: logicOp=32, vertexPipelineStoresAndAtomics=100 */
    if (pFeatures) {
        uint32_t* logicOp = (uint32_t*)((uint8_t*)pFeatures + 16 + 32);
        uint32_t* vertPSA = (uint32_t*)((uint8_t*)pFeatures + 16 + 100);
        LOG("  Core: logicOp=%u vertexPSA=%u\n", *logicOp, *vertPSA);
        if (!*logicOp) {
            *logicOp = 1;
            LOG("  -> SPOOFED logicOp=1\n");
        }
        if (!*vertPSA) {
            *vertPSA = 1;
            LOG("  -> SPOOFED vertexPipelineStoresAndAtomics=1\n");
        }
    }

    /* Walk pNext chain: spoof features DXVK requires */
    if (pFeatures) {
        typedef struct { uint32_t sType; uint32_t _pad; void* pNext; } VkBase;
        VkBase* node = (VkBase*)(*(void**)((uint8_t*)pFeatures + 8)); /* pNext */
        int found_robust2 = 0;
        int chain_len = 0;
        while (node) {
            chain_len++;
            if (node->sType == 53) { /* VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES */
                uint32_t sync2 = *(uint32_t*)((uint8_t*)node + 52);
                uint32_t dynRender = *(uint32_t*)((uint8_t*)node + 64);
                LOG("  Vulkan13Features: synchronization2=%u dynamicRendering=%u\n",
                    sync2, dynRender);
            }
            /* VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT = 1000286000
             * Layout: sType(4)+pad(4)+pNext(8)+robustBufferAccess2(4)+robustImageAccess2(4)+nullDescriptor(4) */
            if (node->sType == 1000286000) {
                found_robust2 = 1;
                uint32_t* robustBuf = (uint32_t*)((uint8_t*)node + 16);
                uint32_t* robustImg = (uint32_t*)((uint8_t*)node + 20);
                uint32_t* nullDesc  = (uint32_t*)((uint8_t*)node + 24);
                LOG("  Robustness2: buf=%u img=%u null=%u", *robustBuf, *robustImg, *nullDesc);
                /* Spoof all three robustness2 features.  DXVK hard-requires
                 * robustBufferAccess2 AND nullDescriptor for adapter selection.
                 * We do NOT strip these from CreateDevice pNext — Vortek/Mali
                 * may honour the feature struct even without the extension name,
                 * and Mali generally handles OOB/null gracefully. */
                if (!*robustBuf) { *robustBuf = 1; LOG(" -> SPOOFED buf=1"); }
                if (!*robustImg) { *robustImg = 1; LOG(" -> SPOOFED img=1"); }
                if (!*nullDesc)  { *nullDesc = 1;  LOG(" -> SPOOFED null=1"); }
                LOG("\n");
            }
            /* VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR = 1000470000
             * Layout: sType(4)+pad(4)+pNext(8)+maintenance5(4) */
            if (node->sType == 1000470000) {
                uint32_t* maint5 = (uint32_t*)((uint8_t*)node + 16);
                if (!*maint5) {
                    *maint5 = 1;
                    LOG("  Maintenance5: SPOOFED=1\n");
                }
            }
            node = (VkBase*)node->pNext;
        }
        LOG("GetFeatures2 EXIT: chain=%d found_robust2=%d\n", chain_len, found_robust2);
    }
}

/* ==== Instance-level wrappers ==== */

typedef VkResult (*PFN_vkCreateInstance)(const void*, const void*, void**);
static PFN_vkCreateInstance real_create_instance = NULL;

static VkResult wrapped_CreateInstance(const void* pCreateInfo,
                                       const void* pAllocator, void** pInstance) {
    if (!real_create_instance) return -3;
    VkResult res = real_create_instance(pCreateInfo, pAllocator, pInstance);
    if (res == 0 && pInstance && *pInstance) {
        saved_instance = *pInstance;
        LOG("CreateInstance OK: instance=%p\n", *pInstance);
    }
    return res;
}

typedef void (*PFN_vkDestroyInstance)(void*, const void*);
static PFN_vkDestroyInstance real_destroy_instance = NULL;

static void wrapped_DestroyInstance(void* instance, const void* pAllocator) {
    if (real_destroy_instance) real_destroy_instance(instance, pAllocator);
    if (instance == saved_instance)
        saved_instance = NULL;
}

/* ==== Device-level C wrappers ====
 *
 * These handle functions where dispatchable handles appear in non-first-arg
 * positions, or where new dispatchable handles are created/destroyed.
 * All other device functions use the simple unwrap trampoline.
 */

/* ---- vkCreateDevice: wrap returned device ---- */

typedef VkResult (*PFN_vkCreateDevice)(void*, const void*, const void*, void**);
static PFN_vkCreateDevice real_create_device = NULL;
static int g_device_count = 0;  /* track device creation order for tracing */

/* Forward declaration: thunk's real GDPA for device-level function resolution */
typedef PFN_vkVoidFunction (*PFN_vkGetDeviceProcAddr)(void*, const char*);
static PFN_vkGetDeviceProcAddr real_gdpa = NULL;

/* Shared-device with queue mutex: one real VkDevice for all CreateDevice calls.
 *
 * Mali/Vortek crashes (SIGSEGV in libGLES_mali.so) when two real VkDevices
 * coexist. Ys IX needs TWO D3D11 devices (both for real work), so we share
 * one real VkDevice but give each caller its own HandleWrapper.
 *
 * Queue serialization: Two dxvk-submit threads race on the same real VkQueue.
 * VkQueue requires external synchronization for submit/wait ops. We use a
 * pthread mutex around all queue operations to prevent DEVICE_LOST. */
static void* shared_real_device = NULL;
static int device_ref_count = 0;
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ==== VK_EXT_device_fault: query GPU fault details on DEVICE_LOST ====
 *
 * VkDeviceFaultCountsEXT (x86-64):
 *   offset 0:  sType (4) = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT = 1000341001
 *   offset 4:  pad (4)
 *   offset 8:  pNext (8)
 *   offset 16: addressInfoCount (4)
 *   offset 20: vendorInfoCount (4)
 *   offset 24: vendorBinarySize (8)
 *
 * VkDeviceFaultInfoEXT (x86-64):
 *   offset 0:  sType (4) = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT = 1000341002
 *   offset 4:  pad (4)
 *   offset 8:  pNext (8)
 *   offset 16: description[256] (char array)
 *   offset 272: pAddressInfos (8)
 *   offset 280: pVendorInfos (8)
 *   offset 288: pVendorBinaryData (8)
 *
 * VkDeviceFaultAddressInfoEXT (24 bytes):
 *   offset 0:  addressType (4)
 *   offset 4:  pad (4)
 *   offset 8:  reportedAddress (8)
 *   offset 16: addressPrecision (8)
 *
 * VkDeviceFaultVendorInfoEXT (280 bytes):
 *   offset 0:  description[256]
 *   offset 256: vendorFaultCode (8)
 *   offset 264: vendorFaultData (8)
 */

typedef VkResult (*PFN_vkGetDeviceFaultInfoEXT)(void*, void*, void*);
static PFN_vkGetDeviceFaultInfoEXT real_get_device_fault_info = NULL;
static int g_fault_queried = 0;

static void query_device_fault(void) {
    if (g_fault_queried || !real_get_device_fault_info || !shared_real_device) return;
    g_fault_queried = 1;

    LOG("=== QUERYING VK_EXT_device_fault ===\n");

    /* First call: get counts */
    uint8_t counts[32];
    memset(counts, 0, sizeof(counts));
    *(uint32_t*)(counts + 0) = 1000341001; /* sType */

    VkResult res = real_get_device_fault_info(shared_real_device, counts, NULL);
    uint32_t addrCount = *(uint32_t*)(counts + 16);
    uint32_t vendorCount = *(uint32_t*)(counts + 20);
    uint64_t binarySize = *(uint64_t*)(counts + 24);
    LOG("  GetDeviceFaultInfo(counts): result=%d addrInfos=%u vendorInfos=%u binarySize=%llu\n",
        res, addrCount, vendorCount, (unsigned long long)binarySize);

    if (res != 0 && res != 5 /* VK_INCOMPLETE */) return;
    if (addrCount == 0 && vendorCount == 0) return;

    /* Second call: get actual info */
    uint8_t* addrInfos = NULL;
    uint8_t* vendorInfos = NULL;
    if (addrCount > 0) addrInfos = (uint8_t*)calloc(addrCount, 24);
    if (vendorCount > 0) vendorInfos = (uint8_t*)calloc(vendorCount, 280);

    uint8_t info[296];
    memset(info, 0, sizeof(info));
    *(uint32_t*)(info + 0) = 1000341002; /* sType */
    *(void**)(info + 272) = addrInfos;
    *(void**)(info + 280) = vendorInfos;

    /* Reset counts struct for second call */
    *(uint32_t*)(counts + 16) = addrCount;
    *(uint32_t*)(counts + 20) = vendorCount;
    *(uint64_t*)(counts + 24) = 0; /* don't allocate binary data */

    res = real_get_device_fault_info(shared_real_device, counts, info);
    LOG("  GetDeviceFaultInfo(info): result=%d\n", res);
    LOG("  Description: %.256s\n", (char*)(info + 16));

    for (uint32_t i = 0; i < addrCount && addrInfos; i++) {
        uint32_t type = *(uint32_t*)(addrInfos + i * 24);
        uint64_t addr = *(uint64_t*)(addrInfos + i * 24 + 8);
        uint64_t prec = *(uint64_t*)(addrInfos + i * 24 + 16);
        LOG("  AddrInfo[%u]: type=%u addr=0x%llx precision=0x%llx\n",
            i, type, (unsigned long long)addr, (unsigned long long)prec);
    }

    for (uint32_t i = 0; i < vendorCount && vendorInfos; i++) {
        char* desc = (char*)(vendorInfos + i * 280);
        uint64_t code = *(uint64_t*)(vendorInfos + i * 280 + 256);
        uint64_t data = *(uint64_t*)(vendorInfos + i * 280 + 264);
        LOG("  VendorInfo[%u]: code=0x%llx data=0x%llx desc=%.256s\n",
            i, (unsigned long long)code, (unsigned long long)data, desc);
    }

    LOG("=== END device_fault ===\n");
    if (addrInfos) free(addrInfos);
    if (vendorInfos) free(vendorInfos);
}

static VkResult wrapped_CreateDevice(void* physDev, const void* pCreateInfo,
                                     const void* pAllocator, void** pDevice) {
    LOG("CD_ENTER rcd=%d srd=%d gc=%d rc=%d\n",
        real_create_device ? 1 : 0, shared_real_device ? 1 : 0,
        g_device_count, device_ref_count);
    if (!real_create_device) {
        LOG("CD_FAIL: real_create_device is NULL!\n");
        return -3;
    }

    g_device_count++;

    if (shared_real_device) {
        /* Reuse the existing real VkDevice for subsequent CreateDevice calls.
         * Each gets its own wrapper so DXVK sees separate VkDevices, but they
         * all map to the same underlying device + queue. */
        device_ref_count++;
        HandleWrapper* w = wrap_handle(shared_real_device);
        if (!w) {
            device_ref_count--;
            return -1;
        }
        *pDevice = w;
        LOG("CreateDevice #%d SHARED: real=%p wrapper=%p refcount=%d\n",
            g_device_count, shared_real_device, (void*)w, device_ref_count);
        return 0;
    }

    /* Strip injected extensions from the create info — the real device
     * doesn't actually support them, so passing them through would cause
     * VK_ERROR_EXTENSION_NOT_PRESENT.
     *
     * VkDeviceCreateInfo layout (relevant fields):
     *   sType(4) + pad(4) + pNext(8) + flags(4) + queueCreateInfoCount(4)
     *   + pQueueCreateInfos(8) + enabledLayerCount(4) + pad(4)
     *   + ppEnabledLayerNames(8) + enabledExtensionCount(4) + pad(4)
     *   + ppEnabledExtensionNames(8) + pEnabledFeatures(8)
     * enabledExtensionCount at offset 48, ppEnabledExtensionNames at offset 56 */
    uint32_t orig_ext_count = *(uint32_t*)((uint8_t*)pCreateInfo + 48);
    const char* const* orig_ext_names = *(const char* const**)((uint8_t*)pCreateInfo + 56);

    /* Build filtered extension list (strip injected ones) */
    const char** filtered_names = NULL;
    uint32_t filtered_count = 0;
    if (orig_ext_count > 0 && orig_ext_names) {
        filtered_names = (const char**)malloc(orig_ext_count * sizeof(char*));
        if (filtered_names) {
            for (uint32_t i = 0; i < orig_ext_count; i++) {
                int injected = 0;
                for (int j = 0; g_injected_extensions[j]; j++) {
                    if (strcmp(orig_ext_names[i], g_injected_extensions[j]) == 0) {
                        injected = 1;
                        LOG("CD: stripping injected ext [%s]\n", orig_ext_names[i]);
                        break;
                    }
                }
                if (!injected)
                    filtered_names[filtered_count++] = orig_ext_names[i];
            }
            /* Patch the create info (temporarily) */
            *(uint32_t*)((uint8_t*)pCreateInfo + 48) = filtered_count;
            *(const char***)((uint8_t*)pCreateInfo + 56) = filtered_names;
        }
    }

    /* Strip spoofed features from pEnabledFeatures and pNext chain.
     * We spoof features in GetFeatures2 so DXVK accepts the adapter,
     * but the real driver doesn't support them — passing them through
     * would cause VK_ERROR_FEATURE_NOT_PRESENT.
     *
     * VkPhysicalDeviceFeatures offsets: logicOp=32, vertexPipelineStoresAndAtomics=100
     * pEnabledFeatures at offset 64 in VkDeviceCreateInfo */
    typedef struct { uint32_t sType; uint32_t _pad; void* pNext; } PNBase;

    /* Case A: pEnabledFeatures (flat VkPhysicalDeviceFeatures pointer) */
    void* pEnabledFeatures = *(void**)((uint8_t*)pCreateInfo + 64);
    uint32_t save_pef_logicOp = 0, save_pef_vertPSA = 0;
    if (pEnabledFeatures) {
        uint32_t* lo = (uint32_t*)((uint8_t*)pEnabledFeatures + 32);
        uint32_t* vp = (uint32_t*)((uint8_t*)pEnabledFeatures + 100);
        save_pef_logicOp = *lo; save_pef_vertPSA = *vp;
        if (*lo) { *lo = 0; LOG("CD: stripped pEnabledFeatures.logicOp\n"); }
        if (*vp) { *vp = 0; LOG("CD: stripped pEnabledFeatures.vertexPSA\n"); }
    }

    /* Case B: Walk pNext chain for VkPhysicalDeviceFeatures2 and extension features */
    uint32_t save_f2_logicOp = 0, save_f2_vertPSA = 0;
    uint32_t save_robust[3] = {0, 0, 0};
    uint32_t save_maint5 = 0;
    {
        PNBase* pn = (PNBase*)(*(void**)((uint8_t*)pCreateInfo + 8));
        while (pn) {
            if (pn->sType == 51) { /* VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 */
                uint32_t* lo = (uint32_t*)((uint8_t*)pn + 16 + 32);
                uint32_t* vp = (uint32_t*)((uint8_t*)pn + 16 + 100);
                save_f2_logicOp = *lo; save_f2_vertPSA = *vp;
                if (*lo) { *lo = 0; LOG("CD: stripped features2.logicOp\n"); }
                if (*vp) { *vp = 0; LOG("CD: stripped features2.vertexPSA\n"); }
            }
            if (pn->sType == 1000286000) { /* VkPhysicalDeviceRobustness2FeaturesEXT */
                uint32_t* f = (uint32_t*)((uint8_t*)pn + 16);
                save_robust[0] = f[0]; save_robust[1] = f[1]; save_robust[2] = f[2];
                /* Do NOT strip robustness2 — let Vortek/Mali see the request.
                 * Mali generally handles null descriptors and OOB gracefully. */
                LOG("CD: passing robustness2 through (%u,%u,%u)\n", f[0], f[1], f[2]);
            }
            if (pn->sType == 1000470000) { /* VkPhysicalDeviceMaintenance5FeaturesKHR */
                uint32_t* f = (uint32_t*)((uint8_t*)pn + 16);
                save_maint5 = *f;
                if (*f) { *f = 0; LOG("CD: stripped maintenance5\n"); }
            }
            pn = (PNBase*)pn->pNext;
        }
    }

    VkResult res = real_create_device(physDev, pCreateInfo, pAllocator, pDevice);

    /* Restore all stripped features */
    if (pEnabledFeatures) {
        *(uint32_t*)((uint8_t*)pEnabledFeatures + 32) = save_pef_logicOp;
        *(uint32_t*)((uint8_t*)pEnabledFeatures + 100) = save_pef_vertPSA;
    }
    {
        PNBase* pn = (PNBase*)(*(void**)((uint8_t*)pCreateInfo + 8));
        while (pn) {
            if (pn->sType == 51) {
                *(uint32_t*)((uint8_t*)pn + 16 + 32) = save_f2_logicOp;
                *(uint32_t*)((uint8_t*)pn + 16 + 100) = save_f2_vertPSA;
            }
            if (pn->sType == 1000286000) {
                uint32_t* f = (uint32_t*)((uint8_t*)pn + 16);
                f[0] = save_robust[0]; f[1] = save_robust[1]; f[2] = save_robust[2];
            }
            if (pn->sType == 1000470000) {
                *(uint32_t*)((uint8_t*)pn + 16) = save_maint5;
            }
            pn = (PNBase*)pn->pNext;
        }
    }

    /* Restore original extensions */
    if (filtered_names) {
        *(uint32_t*)((uint8_t*)pCreateInfo + 48) = orig_ext_count;
        *(const char* const**)((uint8_t*)pCreateInfo + 56) = orig_ext_names;
        free(filtered_names);
    }

    if (res == 0 && pDevice && *pDevice) {
        void* real_device = *pDevice;
        shared_real_device = real_device;
        device_ref_count = 1;

        /* Resolve thunk's real GDPA on first successful device creation.
         * We need this for device-level functions that GIPA doesn't resolve
         * (e.g. vkBeginCommandBuffer, vkCmdDraw, etc.) */
        if (!real_gdpa && real_gipa && saved_instance) {
            real_gdpa = (PFN_vkGetDeviceProcAddr)real_gipa(saved_instance, "vkGetDeviceProcAddr");
            LOG("Thunk GDPA resolved: %p\n", (void*)real_gdpa);
        }

        HandleWrapper* w = wrap_handle(real_device);
        if (!w) {
            LOG("CreateDevice: FATAL: wrap_handle failed (OOM)\n");
            PFN_vkVoidFunction dfn = real_gipa(saved_instance, "vkDestroyDevice");
            if (dfn) ((void(*)(void*,const void*))dfn)(real_device, pAllocator);
            shared_real_device = NULL;
            device_ref_count = 0;
            return -1; /* VK_ERROR_OUT_OF_HOST_MEMORY */
        }
        *pDevice = w;
        LOG("CreateDevice #%d OK: real=%p wrapper=%p refcount=%d\n",
            g_device_count, real_device, (void*)w, device_ref_count);
    }
    return res;
}

/* ---- vkDestroyDevice: unwrap + free wrapper, ref-count real device ---- */

typedef void (*PFN_vkDestroyDevice)(void*, const void*);
static PFN_vkDestroyDevice real_destroy_device = NULL;

static void wrapper_DestroyDevice(void* device, const void* pAllocator) {
    if (!device) return;
    void* real = unwrap(device);
    LOG("DestroyDevice: wrapper=%p real=%p refcount=%d\n",
        device, real, device_ref_count);
    free_wrapper(device);
    device_ref_count--;
    if (device_ref_count <= 0) {
        LOG("DestroyDevice: last ref, destroying real device %p\n", real);
        if (real_destroy_device) real_destroy_device(real, pAllocator);
        shared_real_device = NULL;
        device_ref_count = 0;
    }
}

/* ---- vkGetDeviceQueue: unwrap device, wrap returned queue ---- */

typedef void (*PFN_vkGetDeviceQueue)(void*, uint32_t, uint32_t, void**);
static PFN_vkGetDeviceQueue real_get_device_queue = NULL;

static void wrapper_GetDeviceQueue(void* device, uint32_t qfi,
                                   uint32_t qi, void** pQueue) {
    void* real = unwrap(device);
    real_get_device_queue(real, qfi, qi, pQueue);
    if (pQueue && *pQueue) {
        void* real_queue = *pQueue;
        HandleWrapper* w = wrap_handle(real_queue);
        if (w) {
            *pQueue = w;
            LOG("GetDeviceQueue: qfi=%u qi=%u real=%p wrapper=%p\n",
                qfi, qi, real_queue, (void*)w);
        }
    }
}

/* ---- vkGetDeviceQueue2: unwrap device, wrap returned queue ---- */

typedef void (*PFN_vkGetDeviceQueue2)(void*, const void*, void**);
static PFN_vkGetDeviceQueue2 real_get_device_queue2 = NULL;

static void wrapper_GetDeviceQueue2(void* device, const void* pQueueInfo,
                                    void** pQueue) {
    void* real = unwrap(device);
    real_get_device_queue2(real, pQueueInfo, pQueue);
    if (pQueue && *pQueue) {
        HandleWrapper* w = wrap_handle(*pQueue);
        if (w) *pQueue = w;
    }
}

/* ---- vkAllocateCommandBuffers: unwrap device, wrap returned cmdBufs ---- */

typedef VkResult (*PFN_vkAllocCmdBufs)(void*, const void*, void**);
static PFN_vkAllocCmdBufs real_alloc_cmdbufs = NULL;

static VkResult wrapper_AllocateCommandBuffers(void* device,
                                               const void* pAllocInfo,
                                               void** pCmdBufs) {
    void* real = unwrap(device);
    /* VkCommandBufferAllocateInfo on x86-64:
     * offset 28: commandBufferCount (uint32_t) */
    uint32_t count = pAllocInfo
        ? *(const uint32_t*)((const char*)pAllocInfo + 28) : 0;

    VkResult res = real_alloc_cmdbufs(real, pAllocInfo, pCmdBufs);
    LOG("[D%d] vkAllocateCommandBuffers: dev=%p count=%u result=%d\n",
        g_device_count, real, count, res);
    if (res == 0 && pCmdBufs && count > 0) {
        for (uint32_t i = 0; i < count; i++) {
            if (pCmdBufs[i]) {
                HandleWrapper* w = wrap_handle(pCmdBufs[i]);
                if (w) pCmdBufs[i] = w;
            }
        }
    }
    return res;
}

/* ---- vkFreeCommandBuffers: unwrap device + cmdBufs, free wrappers ---- */

typedef void (*PFN_vkFreeCmdBufs)(void*, uint64_t, uint32_t, void* const*);
static PFN_vkFreeCmdBufs real_free_cmdbufs = NULL;

static void wrapper_FreeCommandBuffers(void* device, uint64_t pool,
                                       uint32_t count, void* const* pCmdBufs) {
    void* real = unwrap(device);

    if (count == 0 || !pCmdBufs) {
        real_free_cmdbufs(real, pool, count, pCmdBufs);
        return;
    }

    /* Unwrap all into temp array, then free wrappers */
    void** real_bufs = (void**)alloca(count * sizeof(void*));
    for (uint32_t i = 0; i < count; i++) {
        if (pCmdBufs[i]) {
            real_bufs[i] = unwrap((void*)pCmdBufs[i]);
            free_wrapper((void*)pCmdBufs[i]);
        } else {
            real_bufs[i] = NULL;
        }
    }

    real_free_cmdbufs(real, pool, count, real_bufs);
}

/* ---- vkQueueSubmit: unwrap queue + cmdBufs in VkSubmitInfo ---- */

/* VkSubmitInfo layout on x86-64 (72 bytes) */
typedef struct {
    uint32_t    sType;                  /* 0 */
    const void* pNext;                  /* 8 */
    uint32_t    waitSemaphoreCount;     /* 16 */
    const void* pWaitSemaphores;        /* 24 */
    const void* pWaitDstStageMask;      /* 32 */
    uint32_t    commandBufferCount;     /* 40 */
    void**      pCommandBuffers;        /* 48 */
    uint32_t    signalSemaphoreCount;   /* 56 */
    const void* pSignalSemaphores;      /* 64 */
} ICD_VkSubmitInfo;

typedef VkResult (*PFN_vkQueueSubmit)(void*, uint32_t, const ICD_VkSubmitInfo*, uint64_t);
static PFN_vkQueueSubmit real_queue_submit = NULL;

static int submit_count_global = 0;
static int g_cmd_op_count = 0;  /* Cmd* operation counter (defined here, used in Cmd* traces below) */

static VkResult wrapper_QueueSubmit(void* queue, uint32_t submitCount,
                                    const ICD_VkSubmitInfo* pSubmits,
                                    uint64_t fence) {
    void* real_queue = unwrap(queue);

    if (submitCount == 0 || !pSubmits) {
        pthread_mutex_lock(&queue_mutex);
        VkResult r = real_queue_submit(real_queue, submitCount, pSubmits, fence);
        pthread_mutex_unlock(&queue_mutex);
        return r;
    }

    /* Count total cmdBufs to unwrap */
    uint32_t total = 0;
    for (uint32_t s = 0; s < submitCount; s++)
        total += pSubmits[s].commandBufferCount;

    if (total == 0) {
        pthread_mutex_lock(&queue_mutex);
        VkResult r = real_queue_submit(real_queue, submitCount, pSubmits, fence);
        pthread_mutex_unlock(&queue_mutex);
        return r;
    }

    /* Create temp copies with unwrapped cmdBuf arrays */
    ICD_VkSubmitInfo* tmp = (ICD_VkSubmitInfo*)alloca(
        submitCount * sizeof(ICD_VkSubmitInfo));
    void** bufs = (void**)alloca(total * sizeof(void*));
    uint32_t idx = 0;

    for (uint32_t s = 0; s < submitCount; s++) {
        tmp[s] = pSubmits[s];
        if (pSubmits[s].commandBufferCount > 0 && pSubmits[s].pCommandBuffers) {
            tmp[s].pCommandBuffers = &bufs[idx];
            for (uint32_t c = 0; c < pSubmits[s].commandBufferCount; c++)
                bufs[idx++] = unwrap(pSubmits[s].pCommandBuffers[c]);
        }
    }

    int sn = ++submit_count_global;
    LOG("[D%d] vkQueueSubmit #%d: queue=%p submits=%u cmdBufs=%u\n",
        g_device_count, sn, real_queue, submitCount, total);

    /* TSO fix: With TSOEnabled=0, ARM64 stores from x86-64 guest code may
     * still be in the CPU store buffer. Force all prior stores to be visible
     * before submitting work to the GPU. This mfence is always translated to
     * a full ARM64 barrier (dmb sy) by FEX, even with TSO disabled. */
    __sync_synchronize();

    /* Serialize queue operations — shared device means shared queue */
    pthread_mutex_lock(&queue_mutex);
    VkResult res = real_queue_submit(real_queue, submitCount, tmp, fence);
    pthread_mutex_unlock(&queue_mutex);
    if (res != 0)
        LOG("[D%d] vkQueueSubmit #%d FAILED: %d\n", g_device_count, sn, res);

    return res;
}

/* ===== Secondary CB Command Replay =====
 * Vortek IPC doesn't properly handle secondary command buffers:
 *   - Dynamic state not inherited from primary→secondary (confirmed)
 *   - Possible pNext chain (InheritanceRenderingInfo) not serialized
 * Instead of using CmdExecuteCommands, we record commands from secondary CBs
 * and replay them directly into the primary CB context. This makes all
 * commands execute within the primary CB's render pass + dynamic state. */

#define MAX_REPLAY_CBS 64
#define MAX_REPLAY_CMDS 512
#define MAX_REPLAY_DATA 256  /* max push constant data */

enum {
    RCMD_NONE = 0,
    RCMD_BIND_VB,
    RCMD_BIND_IB,
    RCMD_BIND_PIPELINE,
    RCMD_BIND_DESC_SETS,
    RCMD_PUSH_CONSTS,
    RCMD_DRAW_INDEXED,
    RCMD_DRAW,
    /* Extended dynamic state — generic slot-based recording */
    RCMD_EDS_UINT,       /* (cmdBuf, uint32_t value) — cull, depth test, etc. */
    RCMD_EDS_VIEWPORT,   /* (cmdBuf, count, pViewports) */
    RCMD_EDS_SCISSOR,    /* (cmdBuf, count, pScissors) */
    RCMD_EDS_DEPTHBIAS,  /* (cmdBuf, float, float, float) */
    RCMD_EDS_BLEND,      /* (cmdBuf, float[4]) */
    RCMD_EDS_STENCIL2,   /* (cmdBuf, faceMask, value) */
    RCMD_EDS_STENCILOP,  /* (cmdBuf, faceMask, failOp, passOp, depthFailOp, compareOp) */
};

typedef struct {
    uint8_t type;
    union {
        struct { uint32_t first, count; uint64_t buffers[8]; uint64_t offsets[8]; uint64_t strides[8]; } vb;
        struct { uint64_t buffer; uint64_t offset; uint32_t indexType; } ib;
        struct { uint32_t bindPoint; uint64_t pipeline; } pipe;
        struct { uint32_t bindPoint; uint64_t layout; uint32_t firstSet, setCount;
                 uint64_t sets[4]; uint32_t dynOffCount; uint32_t dynOffs[8]; } desc;
        struct { uint64_t layout; uint32_t stageFlags, offset, size;
                 uint8_t data[MAX_REPLAY_DATA]; } pc;
        struct { uint32_t indexCount, instanceCount, firstIndex;
                 int32_t vertexOffset; uint32_t firstInstance; } draw_idx;
        struct { uint32_t vertexCount, instanceCount, firstVertex, firstInstance; } draw;
        /* EDS generic */
        struct { int slot; uint32_t value; } eds_uint;
        struct { int slot; uint32_t count; float data[6]; } eds_viewport; /* max 1 viewport: 6 floats */
        struct { int slot; uint32_t count; uint32_t data[4]; } eds_scissor; /* max 1 scissor: 4 u32 */
        struct { float a, b, c; } eds_depthbias;
        struct { float vals[4]; } eds_blend;
        struct { int slot; uint32_t face, val; } eds_stencil2;
        struct { uint32_t face, fail, pass, dfail, cmp; } eds_stencilop;
    };
} ReplayCmd;

typedef struct {
    void* wrapped_cb;      /* wrapped CB handle (for lookup) */
    int is_secondary;      /* 1 if RENDER_PASS_CONTINUE was set */
    int cmd_count;         /* number of recorded commands */
    ReplayCmd cmds[MAX_REPLAY_CMDS];
} ReplayCB;

static ReplayCB g_replay_cbs[MAX_REPLAY_CBS];
static int g_replay_cb_count = 0;

static ReplayCB* find_replay_cb(void* wrapped, int create) {
    for (int i = 0; i < g_replay_cb_count; i++)
        if (g_replay_cbs[i].wrapped_cb == wrapped)
            return &g_replay_cbs[i];
    if (create && g_replay_cb_count < MAX_REPLAY_CBS) {
        ReplayCB* cb = &g_replay_cbs[g_replay_cb_count++];
        memset(cb, 0, sizeof(ReplayCB));
        return cb;
    }
    return NULL;
}

static ReplayCmd* add_replay_cmd(void* wrapped_cb, int type) {
    ReplayCB* rcb = find_replay_cb(wrapped_cb, 0);
    if (!rcb || !rcb->is_secondary || rcb->cmd_count >= MAX_REPLAY_CMDS) return NULL;
    ReplayCmd* cmd = &rcb->cmds[rcb->cmd_count++];
    memset(cmd, 0, sizeof(ReplayCmd));
    cmd->type = type;
    return cmd;
}

/* Forward declaration — defined after all real_cmd_* function pointers */
static void replay_secondary_into_primary(void* real_primary, ReplayCB* rcb);

/* ---- vkCmdExecuteCommands: unwrap + forward ---- */

typedef void (*PFN_vkCmdExecCmds)(void*, uint32_t, void* const*);
static PFN_vkCmdExecCmds real_cmd_exec_cmds = NULL;

/* Detailed logging callback — set after state variables are defined */
typedef void (*ExecCmdsLogFn)(void* real_primary, uint32_t count, void* const* pSecondary);
static ExecCmdsLogFn g_exec_cmds_log = NULL;

/* Callback to inject VB/IB state onto a primary CB — set after globals are defined */
typedef void (*InjectVBIBFn)(void* real_primary);
static InjectVBIBFn g_inject_vb_ib = NULL;

static void wrapper_CmdExecuteCommands(void* cmdBuf, uint32_t count,
                                       void* const* pSecondary) {
    void* real_cmd = unwrap(cmdBuf);
    if (g_exec_cmds_log)
        g_exec_cmds_log(real_cmd, count, pSecondary);

    /* NATIVE path: unwrap all secondary handles and pass through.
     * Replay is disabled to test if real secondary CBs have VB bindings
     * that our replay recording might be missing. */
    void** native_sec = (void**)alloca(count * sizeof(void*));
    for (uint32_t i = 0; i < count; i++)
        native_sec[i] = unwrap((void*)pSecondary[i]);

    static int exec_log = 0;
    if (++exec_log <= 20)
        LOG("  EXEC_NATIVE: primary=%p count=%u sec[0]=%p->%p\n",
            real_cmd, count, count > 0 ? pSecondary[0] : NULL,
            count > 0 ? native_sec[0] : NULL);

    real_cmd_exec_cmds(real_cmd, count, native_sec);
}

/* ---- vkQueueSubmit2: pass-through with handle unwrapping ----
 *
 * Vortek natively supports QueueSubmit2 (vt_handle_vkQueueSubmit2 exists
 * in libvortekrenderer.so). We just need to unwrap the queue handle and
 * any command buffer handles embedded in VkCommandBufferSubmitInfo.
 */

/* VkCommandBufferSubmitInfo (32 bytes on x86-64) */
typedef struct {
    uint32_t    sType;          /* 0 */
    uint32_t    _pad0;          /* 4 */
    const void* pNext;          /* 8 */
    void*       commandBuffer;  /* 16 */
    uint32_t    deviceMask;     /* 24 */
    uint32_t    _pad1;          /* 28 */
} ICD_VkCommandBufferSubmitInfo;

/* VkSubmitInfo2 (64 bytes on x86-64) */
typedef struct {
    uint32_t    sType;                      /* 0 */
    uint32_t    _pad0;                      /* 4 */
    const void* pNext;                      /* 8 */
    uint32_t    flags;                      /* 16 */
    uint32_t    waitSemaphoreInfoCount;     /* 20 */
    const void* pWaitSemaphoreInfos;        /* 24 */
    uint32_t    commandBufferInfoCount;     /* 32 */
    uint32_t    _pad1;                      /* 36 */
    const ICD_VkCommandBufferSubmitInfo* pCommandBufferInfos; /* 40 */
    uint32_t    signalSemaphoreInfoCount;   /* 48 */
    uint32_t    _pad2;                      /* 52 */
    const void* pSignalSemaphoreInfos;      /* 56 */
} ICD_VkSubmitInfo2;

typedef VkResult (*PFN_vkQueueSubmit2)(void*, uint32_t, const ICD_VkSubmitInfo2*, uint64_t);
static PFN_vkQueueSubmit2 real_queue_submit2 = NULL;

static VkResult wrapper_QueueSubmit2(void* queue, uint32_t submitCount,
                                     const ICD_VkSubmitInfo2* pSubmits,
                                     uint64_t fence) {
    void* real_queue = unwrap(queue);

    if (submitCount == 0 || !pSubmits) {
        VkResult r = real_queue_submit2(real_queue, 0, NULL, fence);
        return r;
    }

    /* Count total cmdBufs to unwrap */
    uint32_t total = 0;
    for (uint32_t s = 0; s < submitCount; s++)
        total += pSubmits[s].commandBufferInfoCount;

    if (total == 0) {
        /* No command buffers to unwrap — pass through directly */
        int sn = ++submit_count_global;
        LOG("[D%d] vkQueueSubmit2 #%d: queue=%p submits=%u cmdBufs=0 (passthrough)\n",
            g_device_count, sn, real_queue, submitCount);
        pthread_mutex_lock(&queue_mutex);
        VkResult r = real_queue_submit2(real_queue, submitCount, pSubmits, fence);
        pthread_mutex_unlock(&queue_mutex);
        LOG("[D%d] vkQueueSubmit2 #%d: result=%d\n", g_device_count, sn, r);
        return r;
    }

    /* Create temp copies of VkSubmitInfo2 with unwrapped cmdBuf handles. */
    ICD_VkSubmitInfo2* tmp = (ICD_VkSubmitInfo2*)alloca(
        submitCount * sizeof(ICD_VkSubmitInfo2));
    ICD_VkCommandBufferSubmitInfo* cbInfos = (ICD_VkCommandBufferSubmitInfo*)alloca(
        total * sizeof(ICD_VkCommandBufferSubmitInfo));
    uint32_t ci = 0;

    for (uint32_t s = 0; s < submitCount; s++) {
        tmp[s] = pSubmits[s];  /* shallow copy preserves all semaphore info as-is */
        if (pSubmits[s].commandBufferInfoCount > 0 && pSubmits[s].pCommandBufferInfos) {
            tmp[s].pCommandBufferInfos = &cbInfos[ci];
            for (uint32_t c = 0; c < pSubmits[s].commandBufferInfoCount; c++) {
                cbInfos[ci] = pSubmits[s].pCommandBufferInfos[c]; /* copy struct */
                cbInfos[ci].commandBuffer = unwrap(cbInfos[ci].commandBuffer);
                ci++;
            }
        }
    }

    int sn = ++submit_count_global;
    LOG("[D%d] vkQueueSubmit2 #%d: queue=%p submits=%u cmdBufs=%u (cmd_ops_so_far=%d)\n",
        g_device_count, sn, real_queue, submitCount, total, g_cmd_op_count);

    /* TSO fix: ensure all CPU stores (UBO data, etc.) are committed before GPU reads */
    __sync_synchronize();

    pthread_mutex_lock(&queue_mutex);
    VkResult res = real_queue_submit2(real_queue, submitCount, tmp, fence);
    pthread_mutex_unlock(&queue_mutex);
    if (res != 0)
        LOG("[D%d] vkQueueSubmit2 #%d FAILED: %d\n", g_device_count, sn, res);
    else
        LOG("[D%d] vkQueueSubmit2 #%d OK\n", g_device_count, sn);

    return res;
}

/* ---- vkQueueWaitIdle: mutex-protected (shared queue) ---- */

typedef VkResult (*PFN_vkQueueWaitIdle)(void*);
static PFN_vkQueueWaitIdle real_queue_wait_idle = NULL;

static VkResult wrapper_QueueWaitIdle(void* queue) {
    void* real_queue = unwrap(queue);
    pthread_mutex_lock(&queue_mutex);
    VkResult res = real_queue_wait_idle(real_queue);
    pthread_mutex_unlock(&queue_mutex);
    return res;
}

/* ==== Tracing wrappers for device initialization ====
 *
 * These log VkResult + handle for key functions during device init.
 * Helps identify which Vulkan call fails during the second D3D11 device
 * creation (feat 11_1) that causes the ACCESS_VIOLATION crash.
 * All wrappers unwrap the device handle before calling the real function.
 */

typedef VkResult (*PFN_vkCreateCommandPool)(void*, const void*, const void*, uint64_t*);
static PFN_vkCreateCommandPool real_create_cmd_pool = NULL;

static VkResult trace_CreateCommandPool(void* device, const void* pCreateInfo,
                                        const void* pAllocator, uint64_t* pPool) {
    void* real = unwrap(device);
    /* VkCommandPoolCreateInfo: sType(4) + pad(4) + pNext(8) + flags(4) + queueFamilyIndex(4) */
    uint32_t flags = 0, qfi = 0;
    if (pCreateInfo) {
        flags = *(const uint32_t*)((const char*)pCreateInfo + 16);
        qfi = *(const uint32_t*)((const char*)pCreateInfo + 20);
    }
    VkResult res = real_create_cmd_pool(real, pCreateInfo, pAllocator, pPool);
    LOG("[D%d] vkCreateCommandPool: dev=%p qfi=%u flags=0x%x result=%d pool=0x%llx\n",
        g_device_count, real, qfi, flags, res,
        pPool ? (unsigned long long)*pPool : 0);
    return res;
}

typedef VkResult (*PFN_vkAllocateMemory)(void*, const void*, const void*, uint64_t*);
static PFN_vkAllocateMemory real_alloc_memory = NULL;
static uint64_t g_staging_alloc_total = 0;  /* total allocated from HOST_VISIBLE types */

/* Forward declarations for auto-map in trace_AllocateMemory */
typedef VkResult (*PFN_vkMapMemory)(void*, uint64_t, uint64_t, uint64_t, uint32_t, void**);
static PFN_vkMapMemory real_map_memory;  /* defined later, set by GDPA */
typedef struct { uint64_t memory; void* pointer; uint64_t mapOffset; } MapPtrEntry;
#define MAX_MAP_PTR 2048
static MapPtrEntry g_map_ptrs[MAX_MAP_PTR];
static int g_map_ptr_count;

/* Types 0 and 1 are HOST_VISIBLE (staging heap). Check if a type is HOST_VISIBLE.
 * Our virtual type (g_added_type_index) is also HOST_VISIBLE now (Mali unified). */
static int is_staging_type(uint32_t mem_type) {
    return (mem_type == 0 || mem_type == 1);
}

static VkResult trace_AllocateMemory(void* device, const void* pAllocInfo,
                                     const void* pAllocator, uint64_t* pMemory) {
    void* real = unwrap(device);
    /* VkMemoryAllocateInfo layout (x86-64):
     * offset 0:  sType (4) + pad (4)
     * offset 8:  pNext (8)
     * offset 16: allocationSize (8)
     * offset 24: memoryTypeIndex (4) + pad (4)
     * Total: 32 bytes */
    uint64_t alloc_size = 0;
    uint32_t mem_type = 0;
    if (pAllocInfo) {
        alloc_size = *(const uint64_t*)((const char*)pAllocInfo + 16);
        mem_type = *(const uint32_t*)((const char*)pAllocInfo + 24);
    }

    /* Remap virtual DEVICE_LOCAL-only type to the real type index.
     * The real driver doesn't know about our added type — it only knows
     * the original types. The remapped type (HOST_VISIBLE+DEVICE_LOCAL)
     * allocates from the same physical unified memory. */
    const void* alloc_info = pAllocInfo;
    uint32_t real_type = mem_type;
    uint8_t local_info[32];
    if (g_added_type_index >= 0 && mem_type == (uint32_t)g_added_type_index && pAllocInfo) {
        memcpy(local_info, pAllocInfo, 32);
        *(uint32_t*)(local_info + 24) = (uint32_t)g_remap_to_type;
        real_type = (uint32_t)g_remap_to_type;
        alloc_info = local_info;
        LOG("[D%d] vkAllocateMemory: REMAP type %u -> %u (virtual DEVICE_LOCAL -> real)\n",
            g_device_count, mem_type, real_type);
    }

    /* Pre-flight: reject staging allocations that would exceed ALLOC_BYTE_CAP.
     * Use mem_type (original, before remap) so that virtual DEVICE_LOCAL-only
     * type (g_added_type_index) is NOT capped — it's for the large texture heap.
     * DXVK handles -1 by retrying smaller chunk sizes (16→8→4→2→1 MB). */
    if (is_staging_type(mem_type) && g_staging_alloc_total + alloc_size > ALLOC_BYTE_CAP) {
        LOG("[D%d] vkAllocateMemory: CAPPED type=%u size=%llu staging=%llu MB (cap=%llu MB) -> OOM\n",
            g_device_count, mem_type, (unsigned long long)alloc_size,
            (unsigned long long)(g_staging_alloc_total / (1024*1024)),
            (unsigned long long)(ALLOC_BYTE_CAP / (1024*1024)));
        if (pMemory) *pMemory = 0;
        return -1; /* VK_ERROR_OUT_OF_DEVICE_MEMORY */
    }

    VkResult res = real_alloc_memory(real, alloc_info, pAllocator, pMemory);

    /* Convert DEVICE_LOST (-4) from AllocateMemory to OUT_OF_DEVICE_MEMORY (-1).
     * Query device fault info for diagnostics. DXVK treats -4 as fatal
     * but handles -1 gracefully (retries smaller sizes, falls back). */
    if (res == -4) {
        LOG("[D%d] vkAllocateMemory: DEVICE_LOST! type=%u size=%llu staging=%llu MB\n",
            g_device_count, mem_type, (unsigned long long)alloc_size,
            (unsigned long long)(g_staging_alloc_total / (1024*1024)));
        query_device_fault();
        if (pMemory) *pMemory = 0;
        return -1; /* VK_ERROR_OUT_OF_DEVICE_MEMORY */
    }

    /* Track staging heap usage on success */
    if (res == 0 && is_staging_type(mem_type))
        g_staging_alloc_total += alloc_size;

    LOG("[D%d] vkAllocateMemory: dev=%p size=%llu type=%u(%u) result=%d mem=0x%llx staging=%llu MB\n",
        g_device_count, real, (unsigned long long)alloc_size, mem_type, real_type, res,
        pMemory ? (unsigned long long)*pMemory : 0,
        (unsigned long long)(g_staging_alloc_total / (1024*1024)));

    /* Deferred auto-map: we can't map here because DXVK might map the same
     * memory later, causing a double-map error.  Instead, we map on-demand
     * in lookup_ubo_ptr when we encounter an unmapped memory. */

    return res;
}

typedef VkResult (*PFN_vkCreateBuffer)(void*, const void*, const void*, uint64_t*);
static PFN_vkCreateBuffer real_create_buffer = NULL;

static VkResult trace_CreateBuffer(void* device, const void* pCreateInfo,
                                   const void* pAllocator, uint64_t* pBuffer) {
    void* real = unwrap(device);
    /* VkBufferCreateInfo on x86-64:
     * offset 0:  sType (uint32_t)
     * offset 8:  pNext (pointer)
     * offset 16: flags (uint32_t)
     * offset 24: size (uint64_t, aligned)
     * offset 32: usage (uint32_t)
     * offset 36: sharingMode (uint32_t)
     * offset 40: queueFamilyIndexCount (uint32_t)
     * offset 48: pQueueFamilyIndices (pointer) */
    uint32_t flags = 0, usage = 0, sharing = 0;
    uint64_t size = 0;
    const void* pNext = NULL;
    if (pCreateInfo) {
        pNext = *(const void**)((const char*)pCreateInfo + 8);
        flags = *(const uint32_t*)((const char*)pCreateInfo + 16);
        size = *(const uint64_t*)((const char*)pCreateInfo + 24);
        usage = *(const uint32_t*)((const char*)pCreateInfo + 32);
        sharing = *(const uint32_t*)((const char*)pCreateInfo + 36);
    }
    LOG("[D%d] vkCreateBuffer: dev=%p size=%llu usage=0x%x flags=0x%x sharing=%u pNext=%p\n",
        g_device_count, real, (unsigned long long)size, usage, flags, sharing, pNext);

    VkResult res = real_create_buffer(real, pCreateInfo, pAllocator, pBuffer);
    LOG("[D%d] vkCreateBuffer: result=%d buf=0x%llx\n",
        g_device_count, res, pBuffer ? (unsigned long long)*pBuffer : 0);
    if (res != 0) {
        LOG("[D%d] *** CreateBuffer FAILED: size=%llu usage=0x%x flags=0x%x ***\n",
            g_device_count, (unsigned long long)size, usage, flags);
    }
    return res;
}

typedef VkResult (*PFN_vkCreateImage)(void*, const void*, const void*, uint64_t*);
static PFN_vkCreateImage real_create_image = NULL;

static VkResult trace_CreateImage(void* device, const void* pCreateInfo,
                                  const void* pAllocator, uint64_t* pImage) {
    void* real = unwrap(device);
    /* VkImageCreateInfo on x86-64:
     * offset 0:  sType (uint32_t)
     * offset 8:  pNext (pointer)
     * offset 16: flags (uint32_t)
     * offset 20: imageType (uint32_t)
     * offset 24: format (uint32_t)
     * offset 28: extent.width (uint32_t)
     * offset 32: extent.height (uint32_t)
     * offset 36: extent.depth (uint32_t)
     * offset 40: mipLevels (uint32_t)
     * offset 44: arrayLayers (uint32_t)
     * offset 48: samples (uint32_t)
     * offset 52: tiling (uint32_t)
     * offset 56: usage (uint32_t) */
    uint32_t fmt = 0, w = 0, h = 0, usage = 0, tiling = 0;
    if (pCreateInfo) {
        fmt = *(const uint32_t*)((const char*)pCreateInfo + 24);
        w = *(const uint32_t*)((const char*)pCreateInfo + 28);
        h = *(const uint32_t*)((const char*)pCreateInfo + 32);
        tiling = *(const uint32_t*)((const char*)pCreateInfo + 52);
        usage = *(const uint32_t*)((const char*)pCreateInfo + 56);
    }

    /* BC format substitution: Vortek reports BC features but CreateImage returns -11.
     * Substitute BC → RGBA so image creation succeeds. Track for ImageView fixup. */
    char ci_copy[128];
    const void* actual_ci = pCreateInfo;
    uint32_t rgba_fmt = 0;
    if (pCreateInfo && is_bc_format(fmt)) {
        rgba_fmt = bc_to_rgba_format(fmt);
        memcpy(ci_copy, pCreateInfo, 72);
        *(uint32_t*)(ci_copy + 24) = rgba_fmt;
        actual_ci = ci_copy;
    }

    VkResult res = real_create_image(real, actual_ci, pAllocator, pImage);

    if (res == 0 && rgba_fmt && pImage) {
        bc_img_track(*pImage, fmt, rgba_fmt);
        LOG("[D%d] vkCreateImage: BC SUBST fmt=%u->%u %ux%u result=0 img=0x%llx\n",
            g_device_count, fmt, rgba_fmt, w, h, (unsigned long long)*pImage);
        return res;
    }

    /* Convert DEVICE_LOST: query fault info, then return recoverable error */
    if (res == -4) {
        LOG("[D%d] vkCreateImage: DEVICE_LOST! fmt=%u %ux%u tiling=%u usage=0x%x\n",
            g_device_count, fmt, w, h, tiling, usage);
        query_device_fault();
        return -1;
    }
    LOG("[D%d] vkCreateImage: dev=%p fmt=%u %ux%u tiling=%u usage=0x%x result=%d img=0x%llx\n",
        g_device_count, real, fmt, w, h, tiling, usage, res,
        pImage ? (unsigned long long)*pImage : 0);
    return res;
}

typedef VkResult (*PFN_vkCreateFence)(void*, const void*, const void*, uint64_t*);
static PFN_vkCreateFence real_create_fence = NULL;

static VkResult trace_CreateFence(void* device, const void* pCreateInfo,
                                  const void* pAllocator, uint64_t* pFence) {
    void* real = unwrap(device);
    VkResult res = real_create_fence(real, pCreateInfo, pAllocator, pFence);
    LOG("[D%d] vkCreateFence: dev=%p result=%d fence=0x%llx\n",
        g_device_count, real, res, pFence ? (unsigned long long)*pFence : 0);
    return res;
}

typedef VkResult (*PFN_vkCreateSemaphoreICD)(void*, const void*, const void*, uint64_t*);
static PFN_vkCreateSemaphoreICD real_create_semaphore = NULL;

static VkResult trace_CreateSemaphore(void* device, const void* pCreateInfo,
                                      const void* pAllocator, uint64_t* pSem) {
    void* real = unwrap(device);
    VkResult res = real_create_semaphore(real, pCreateInfo, pAllocator, pSem);
    LOG("[D%d] vkCreateSemaphore: dev=%p result=%d sem=0x%llx\n",
        g_device_count, real, res, pSem ? (unsigned long long)*pSem : 0);
    return res;
}

/* PFN_vkMapMemory and real_map_memory forward-declared above (for auto-map) */
typedef void (*PFN_vkUnmapMemory)(void*, uint64_t);
static PFN_vkUnmapMemory real_unmap_memory;

/* Cache coherence fix: on ARM (Vortek/Mali), HOST_COHERENT may not guarantee
 * that GPU writes are visible to CPU without explicit invalidation.
 * Call InvalidateMappedMemoryRanges after every successful REAL MapMemory. */
typedef VkResult (*PFN_vkInvalidateMappedMemoryRanges)(void*, uint32_t, const void*);
static PFN_vkInvalidateMappedMemoryRanges real_invalidate_mapped = NULL;
typedef VkResult (*PFN_vkFlushMappedMemoryRanges)(void*, uint32_t, const void*);
static PFN_vkFlushMappedMemoryRanges real_flush_mapped = NULL;

static uint64_t g_total_mapped_bytes = 0;
static int g_map_count = 0;

/* Fake MapMemory: when total mapped would exceed MAP_BYTE_LIMIT, return a
 * pointer into a shared scratch buffer instead of calling the real vkMapMemory.
 * DXVK thinks the mapping succeeded; CPU writes go to scratch (data lost),
 * but GPU operations use VkDeviceMemory handles and still work.
 *
 * IMPORTANT: Uses ONE shared scratch buffer (16MB) for ALL fake mappings.
 * Each fake mapping gets a unique offset within the scratch to avoid aliasing.
 * This minimizes VA space consumption (one mmap vs N*16MB). */

#define SCRATCH_SIZE (16ULL * 1024 * 1024)  /* 16 MB shared scratch */
static void* g_scratch_buf = NULL;
static int g_scratch_inited = 0;

static void ensure_scratch(void) {
    if (g_scratch_inited) return;
    g_scratch_inited = 1;
    g_scratch_buf = mmap(NULL, SCRATCH_SIZE, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_scratch_buf == MAP_FAILED) {
        g_scratch_buf = NULL;
        LOG("SCRATCH: mmap failed!\n");
    } else {
        LOG("SCRATCH: allocated %llu MB at %p\n",
            (unsigned long long)(SCRATCH_SIZE / (1024*1024)), g_scratch_buf);
    }
}

/* Track fake-mapped handles (shared scratch, no real GPU mapping) */
#define MAX_FAKE_MAPS 64
static uint64_t g_fake_map_handles[MAX_FAKE_MAPS];
static int g_fake_map_count = 0;

/* Track real-mapped handles so we can decrement g_total_mapped_bytes on unmap.
 * Without this, the counter is monotonically increasing and eventually ALL maps
 * become FAKE (including the headless layer's small staging buffer → black frames). */
typedef struct {
    uint64_t handle;
    uint64_t mapped_size;
} RealMapEntry;

#define MAX_REAL_MAPS 512
static RealMapEntry g_real_maps[MAX_REAL_MAPS];
static int g_real_map_count = 0;

/* === UBO data readback tracking ===
 * Track mapped pointers and buffer→memory bindings so we can read back
 * UBO data at descriptor write time to verify CPU-side correctness.
 * NOTE: MapPtrEntry, g_map_ptrs, g_map_ptr_count, MAX_MAP_PTR, PFN_vkMapMemory,
 * real_map_memory are forward-declared before trace_AllocateMemory (for auto-map). */

typedef struct {
    uint64_t buffer;
    uint64_t memory;
    uint64_t memOffset;  /* offset in vkBindBufferMemory */
} BufMemEntry;

#define MAX_BUF_MEM 8192
static BufMemEntry g_buf_mem[MAX_BUF_MEM];
static int g_buf_mem_count = 0;

/* Look up mapped pointer for a buffer at a given descriptor offset.
 * Returns pointer to the data, or NULL if not trackable. */
static void* lookup_ubo_ptr(uint64_t buffer, uint64_t descOffset) {
    /* Find buffer → memory binding */
    uint64_t mem = 0;
    uint64_t memOff = 0;
    for (int i = g_buf_mem_count - 1; i >= 0; i--) {
        if (g_buf_mem[i].buffer == buffer) {
            mem = g_buf_mem[i].memory;
            memOff = g_buf_mem[i].memOffset;
            break;
        }
    }
    if (!mem) return NULL;

    /* Find memory → mapped pointer */
    for (int i = g_map_ptr_count - 1; i >= 0; i--) {
        if (g_map_ptrs[i].memory == mem && g_map_ptrs[i].pointer) {
            uint8_t* base = (uint8_t*)g_map_ptrs[i].pointer;
            uint64_t mapOff = g_map_ptrs[i].mapOffset;
            void* result = base + memOff + descOffset - mapOff;
            return result;
        }
    }

    /* On-demand map: memory not yet mapped (DXVK DEFAULT usage).
     * On Mali unified memory, all types are HOST_VISIBLE so MapMemory works.
     * Use VK_WHOLE_SIZE (-1) since we don't know the allocation size. */
    if (real_map_memory && shared_real_device && g_map_ptr_count < MAX_MAP_PTR) {
        void* ptr = NULL;
        VkResult mr = real_map_memory(shared_real_device, mem, 0, (uint64_t)-1, 0, &ptr);
        if (mr == 0 && ptr) {
            g_map_ptrs[g_map_ptr_count].memory = mem;
            g_map_ptrs[g_map_ptr_count].pointer = ptr;
            g_map_ptrs[g_map_ptr_count].mapOffset = 0;
            g_map_ptr_count++;
            LOG("ON-DEMAND-MAP: mem=0x%lx ptr=%p\n", (unsigned long)mem, ptr);
            return (uint8_t*)ptr + memOff + descOffset;
        }
    }
    return NULL;
}

static VkResult trace_MapMemory(void* device, uint64_t memory, uint64_t offset,
                                uint64_t size, uint32_t flags, void** ppData) {
    void* real = unwrap(device);
    uint64_t map_size = (size != (uint64_t)-1) ? size : (16ULL * 1024 * 1024);

    /* Check if this mapping would exceed the FEX thunk VA space limit */
    if (g_total_mapped_bytes + map_size > MAP_BYTE_LIMIT) {
        ensure_scratch();
        if (g_scratch_buf && g_fake_map_count < MAX_FAKE_MAPS) {
            g_fake_map_handles[g_fake_map_count++] = memory;
            g_map_count++;
            if (ppData) *ppData = g_scratch_buf; /* all fakes share one buffer */
            LOG("[D%d] vkMapMemory #%d FAKE: mem=0x%llx sz=%llu scratch=%p total_real=%llu MB (limit=%llu MB)\n",
                g_device_count, g_map_count, (unsigned long long)memory,
                (unsigned long long)map_size, g_scratch_buf,
                (unsigned long long)(g_total_mapped_bytes / (1024*1024)),
                (unsigned long long)(MAP_BYTE_LIMIT / (1024*1024)));
            return 0; /* VK_SUCCESS */
        }
    }

    /* Lazily resolve invalidate/flush fn ptrs via dlsym if GDPA hasn't captured them */
    if (!real_invalidate_mapped && thunk_lib)
        real_invalidate_mapped = (PFN_vkInvalidateMappedMemoryRanges)
            dlsym(thunk_lib, "vkInvalidateMappedMemoryRanges");
    if (!real_flush_mapped && thunk_lib)
        real_flush_mapped = (PFN_vkFlushMappedMemoryRanges)
            dlsym(thunk_lib, "vkFlushMappedMemoryRanges");

    VkResult res = real_map_memory(real, memory, offset, size, flags, ppData);
    /* Convert DEVICE_LOST from VA exhaustion to recoverable error */
    if (res == -4) {
        LOG("[D%d] vkMapMemory: DEVICE_LOST -> MEMORY_MAP_FAILED (VA exhausted) total=%llu MB\n",
            g_device_count, (unsigned long long)(g_total_mapped_bytes / (1024*1024)));
        return -5; /* VK_ERROR_MEMORY_MAP_FAILED */
    }
    if (res == 0) {
        g_map_count++;
        uint64_t tracked = (size != (uint64_t)-1) ? size : (16ULL * 1024 * 1024);
        g_total_mapped_bytes += tracked;
        /* Track handle→size for decrement on unmap */
        if (g_real_map_count < MAX_REAL_MAPS) {
            g_real_maps[g_real_map_count].handle = memory;
            g_real_maps[g_real_map_count].mapped_size = tracked;
            g_real_map_count++;
        }
        /* Track memory→pointer for UBO readback */
        if (ppData && *ppData && g_map_ptr_count < MAX_MAP_PTR) {
            g_map_ptrs[g_map_ptr_count].memory = memory;
            g_map_ptrs[g_map_ptr_count].pointer = *ppData;
            g_map_ptrs[g_map_ptr_count].mapOffset = offset;
            g_map_ptr_count++;
        }
    }
    LOG("[D%d] vkMapMemory #%d: mem=0x%llx sz=%llu result=%d total_mapped=%llu MB\n",
        g_device_count, g_map_count, (unsigned long long)memory,
        (unsigned long long)size, res,
        (unsigned long long)(g_total_mapped_bytes / (1024*1024)));
    if (res != 0) {
        LOG("  !!! MapMemory FAILED (result=%d) after %llu MB total mapped\n",
            res, (unsigned long long)(g_total_mapped_bytes / (1024*1024)));
    }

    /* Cache coherence fix: invalidate CPU cache for newly mapped memory.
     * On ARM/Vortek, HOST_COHERENT may not guarantee GPU→CPU visibility
     * through FEX thunk shared memory without explicit invalidation. */
    if (res == 0 && real_invalidate_mapped) {
        uint8_t mmr[40]; /* VkMappedMemoryRange */
        memset(mmr, 0, 40);
        *(uint32_t*)(mmr + 0) = 6;       /* VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE */
        *(uint64_t*)(mmr + 16) = memory;  /* memory */
        *(uint64_t*)(mmr + 24) = offset;  /* offset */
        *(uint64_t*)(mmr + 32) = size;    /* size */
        VkResult inv = real_invalidate_mapped(real, 1, mmr);
        (void)inv; /* ignore result — best effort */
    }

    return res;
}

/* UnmapMemory: if fake-mapped, just remove from tracking (scratch is shared).
 * If real-mapped, call real unmap AND decrement g_total_mapped_bytes so the
 * counter stays accurate (fixes: all maps becoming FAKE after enough cycles). */

static void trace_UnmapMemory(void* device, uint64_t memory) {
    /* Check fake maps first */
    for (int i = 0; i < g_fake_map_count; i++) {
        if (g_fake_map_handles[i] == memory) {
            LOG("[D%d] vkUnmapMemory FAKE: mem=0x%llx\n",
                g_device_count, (unsigned long long)memory);
            for (int j = i; j < g_fake_map_count - 1; j++)
                g_fake_map_handles[j] = g_fake_map_handles[j + 1];
            g_fake_map_count--;
            return;
        }
    }

    /* Real map: decrement tracked bytes */
    for (int i = 0; i < g_real_map_count; i++) {
        if (g_real_maps[i].handle == memory) {
            g_total_mapped_bytes -= g_real_maps[i].mapped_size;
            LOG("[D%d] vkUnmapMemory REAL: mem=0x%llx freed=%llu MB total_mapped=%llu MB\n",
                g_device_count, (unsigned long long)memory,
                (unsigned long long)(g_real_maps[i].mapped_size / (1024*1024)),
                (unsigned long long)(g_total_mapped_bytes / (1024*1024)));
            for (int j = i; j < g_real_map_count - 1; j++)
                g_real_maps[j] = g_real_maps[j + 1];
            g_real_map_count--;
            break;
        }
    }

    void* real = unwrap(device);
    real_unmap_memory(real, memory);
}

typedef VkResult (*PFN_vkBindBufferMemory)(void*, uint64_t, uint64_t, uint64_t);
static PFN_vkBindBufferMemory real_bind_buf_mem = NULL;

static VkResult trace_BindBufferMemory(void* device, uint64_t buffer,
                                       uint64_t memory, uint64_t offset) {
    void* real = unwrap(device);
    VkResult res = real_bind_buf_mem(real, buffer, memory, offset);
    LOG("[D%d] vkBindBufferMemory: dev=%p buf=0x%llx mem=0x%llx off=%llu result=%d\n",
        g_device_count, real, (unsigned long long)buffer,
        (unsigned long long)memory, (unsigned long long)offset, res);
    /* Track buffer→memory for UBO readback */
    if (res == 0 && g_buf_mem_count < MAX_BUF_MEM) {
        g_buf_mem[g_buf_mem_count].buffer = buffer;
        g_buf_mem[g_buf_mem_count].memory = memory;
        g_buf_mem[g_buf_mem_count].memOffset = offset;
        g_buf_mem_count++;
    }
    return res;
}

/* --- BindBufferMemory2 (Vulkan 1.1) ---
 * VkBindBufferMemoryInfo: sType(0) pNext(8) buffer(16) memory(24) memoryOffset(32) = 40 bytes */
typedef VkResult (*PFN_vkBindBufferMemory2)(void*, uint32_t, const void*);
static PFN_vkBindBufferMemory2 real_bind_buf_mem2 = NULL;

static int g_bind_buf_mem2_calls = 0;
static VkResult trace_BindBufferMemory2(void* device, uint32_t bindInfoCount,
                                         const void* pBindInfos) {
    void* real = unwrap(device);
    VkResult res = real_bind_buf_mem2(real, bindInfoCount, pBindInfos);
    g_bind_buf_mem2_calls++;
    if (g_bind_buf_mem2_calls <= 20) {
        LOG("BindBufferMemory2: call#%d count=%u result=%d total_tracked=%d\n",
            g_bind_buf_mem2_calls, bindInfoCount, res, g_buf_mem_count);
    }
    if (res == 0 && pBindInfos) {
        const uint8_t* infos = (const uint8_t*)pBindInfos;
        for (uint32_t i = 0; i < bindInfoCount; i++) {
            const uint8_t* bi = infos + i * 40;
            uint64_t buffer = *(const uint64_t*)(bi + 16);
            uint64_t memory = *(const uint64_t*)(bi + 24);
            uint64_t memOffset = *(const uint64_t*)(bi + 32);
            if (g_bind_buf_mem2_calls <= 5) {
                LOG("  BBM2[%u]: buf=0x%lx mem=0x%lx off=%lu\n",
                    i, (unsigned long)buffer, (unsigned long)memory, (unsigned long)memOffset);
            }
            if (g_buf_mem_count < MAX_BUF_MEM) {
                g_buf_mem[g_buf_mem_count].buffer = buffer;
                g_buf_mem[g_buf_mem_count].memory = memory;
                g_buf_mem[g_buf_mem_count].memOffset = memOffset;
                g_buf_mem_count++;
            }
        }
    }
    return res;
}

typedef VkResult (*PFN_vkBindImageMemory)(void*, uint64_t, uint64_t, uint64_t);
static PFN_vkBindImageMemory real_bind_img_mem = NULL;

static VkResult trace_BindImageMemory(void* device, uint64_t image,
                                      uint64_t memory, uint64_t offset) {
    void* real = unwrap(device);
    VkResult res = real_bind_img_mem(real, image, memory, offset);
    LOG("[D%d] vkBindImageMemory: dev=%p img=0x%llx mem=0x%llx result=%d\n",
        g_device_count, real, (unsigned long long)image,
        (unsigned long long)memory, res);
    return res;
}

typedef VkResult (*PFN_vkCreateDescSetLayout)(void*, const void*, const void*, uint64_t*);
static PFN_vkCreateDescSetLayout real_create_dsl = NULL;

static VkResult trace_CreateDescriptorSetLayout(void* device, const void* pCreateInfo,
                                                const void* pAllocator, uint64_t* pLayout) {
    void* real = unwrap(device);
    VkResult res = real_create_dsl(real, pCreateInfo, pAllocator, pLayout);
    LOG("[D%d] vkCreateDescriptorSetLayout: dev=%p result=%d layout=0x%llx\n",
        g_device_count, real, res, pLayout ? (unsigned long long)*pLayout : 0);
    return res;
}

typedef VkResult (*PFN_vkCreatePipelineLayout)(void*, const void*, const void*, uint64_t*);
static PFN_vkCreatePipelineLayout real_create_pl = NULL;

static VkResult trace_CreatePipelineLayout(void* device, const void* pCreateInfo,
                                           const void* pAllocator, uint64_t* pLayout) {
    void* real = unwrap(device);
    VkResult res = real_create_pl(real, pCreateInfo, pAllocator, pLayout);
    LOG("[D%d] vkCreatePipelineLayout: dev=%p result=%d layout=0x%llx\n",
        g_device_count, real, res, pLayout ? (unsigned long long)*pLayout : 0);
    return res;
}

/* Trace: vkBeginCommandBuffer (first arg is VkCommandBuffer, not VkDevice) */
typedef VkResult (*PFN_vkBeginCmdBuf)(void*, const void*);
static PFN_vkBeginCmdBuf real_begin_cmd_buf = NULL;

static VkResult trace_BeginCommandBuffer(void* cmdBuf, const void* pBeginInfo) {
    void* real = unwrap(cmdBuf);
    /* VkCommandBufferBeginInfo: sType(4)+pad(4)+pNext(8)+flags(4) at offset 16 */
    uint32_t flags = pBeginInfo ? *(const uint32_t*)((const uint8_t*)pBeginInfo + 16) : 0;
    VkResult res = real_begin_cmd_buf(real, pBeginInfo);
    LOG("[D%d] vkBeginCommandBuffer: cb=%p(real=%p) flags=0x%x%s result=%d\n",
        g_device_count, cmdBuf, real, flags,
        (flags & 0x02) ? " RENDER_PASS_CONTINUE(SECONDARY)" : "",
        res);
    /* Track secondary CBs for command replay.
     * RENDER_PASS_CONTINUE_BIT = 0x02 means this is a secondary CB. */
    if (flags & 0x02) {
        ReplayCB* rcb = find_replay_cb(cmdBuf, 1);
        if (rcb) {
            rcb->wrapped_cb = cmdBuf;
            rcb->is_secondary = 1;
            rcb->cmd_count = 0;  /* reset for new recording */
        }
    }
    return res;
}

/* Trace: vkEndCommandBuffer — prototype (definition after all Cmd wrappers) */
typedef VkResult (*PFN_vkEndCmdBuf)(void*);
static PFN_vkEndCmdBuf real_end_cmd_buf = NULL;
static VkResult trace_EndCommandBuffer(void* cmdBuf); /* defined below */

/* Trace: vkCreateImageView — with imageView→image tracking */
typedef VkResult (*PFN_vkCreateImageView)(void*, const void*, const void*, uint64_t*);
static PFN_vkCreateImageView real_create_image_view = NULL;

/* Track imageView→image for the last N views (ring buffer) */
#define IV_TRACK_MAX 256
static struct { uint64_t view; uint64_t image; } g_iv_track[IV_TRACK_MAX];
static int g_iv_idx = 0;

static uint64_t iv_lookup_image(uint64_t view) {
    for (int i = 0; i < IV_TRACK_MAX; i++)
        if (g_iv_track[i].view == view) return g_iv_track[i].image;
    return 0;
}

static VkResult trace_CreateImageView(void* device, const void* pCreateInfo,
                                      const void* pAllocator, uint64_t* pView) {
    void* real = unwrap(device);
    /* VkImageViewCreateInfo x86-64:
     * offset 0: sType(4) pad(4)
     * offset 8: pNext(8)
     * offset 16: flags(4) pad(4)
     * offset 24: image(8)
     * offset 32: viewType(4)
     * offset 36: format(4)
     * offset 40: components (4x4=16 bytes)
     * offset 56: subresourceRange (5x4=20 bytes) */
    uint64_t src_image = 0;
    uint32_t view_fmt = 0;
    if (pCreateInfo) {
        src_image = *(const uint64_t*)((const char*)pCreateInfo + 24);
        view_fmt = *(const uint32_t*)((const char*)pCreateInfo + 36);
    }

    /* If this image was BC-substituted, fix the view format too */
    char ivci_copy[80];
    const void* actual_ci = pCreateInfo;
    int bc_idx = bc_img_lookup(src_image);
    if (bc_idx >= 0 && pCreateInfo && is_bc_format(view_fmt)) {
        memcpy(ivci_copy, pCreateInfo, 76); /* VkImageViewCreateInfo is ~76 bytes */
        *(uint32_t*)(ivci_copy + 36) = g_bc_images[bc_idx].rgba_format;
        actual_ci = ivci_copy;
    }

    VkResult res = real_create_image_view(real, actual_ci, pAllocator, pView);
    if (res == 0 && pView) {
        g_iv_track[g_iv_idx % IV_TRACK_MAX].view = *pView;
        g_iv_track[g_iv_idx % IV_TRACK_MAX].image = src_image;
        g_iv_idx++;
    }
    if (bc_idx >= 0) {
        LOG("[D%d] vkCreateImageView: BC img=0x%llx fmt=%u->%u view=0x%llx result=%d\n",
            g_device_count, (unsigned long long)src_image, view_fmt,
            g_bc_images[bc_idx].rgba_format,
            pView ? (unsigned long long)*pView : 0, res);
    } else {
        LOG("[D%d] vkCreateImageView: dev=%p img=0x%llx view=0x%llx result=%d\n",
            g_device_count, real, (unsigned long long)src_image,
            pView ? (unsigned long long)*pView : 0, res);
    }
    return res;
}

/* Trace: vkCreateSampler */
typedef VkResult (*PFN_vkCreateSampler)(void*, const void*, const void*, uint64_t*);
static PFN_vkCreateSampler real_create_sampler = NULL;

static VkResult trace_CreateSampler(void* device, const void* pCreateInfo,
                                    const void* pAllocator, uint64_t* pSampler) {
    void* real = unwrap(device);
    VkResult res = real_create_sampler(real, pCreateInfo, pAllocator, pSampler);
    LOG("[D%d] vkCreateSampler: dev=%p result=%d sampler=0x%llx\n",
        g_device_count, real, res, pSampler ? (unsigned long long)*pSampler : 0);
    return res;
}

/* Trace: vkCreateShaderModule */
typedef VkResult (*PFN_vkCreateShaderModule)(void*, const void*, const void*, uint64_t*);
static PFN_vkCreateShaderModule real_create_shader_module = NULL;

static VkResult trace_CreateShaderModule(void* device, const void* pCreateInfo,
                                         const void* pAllocator, uint64_t* pModule) {
    void* real = unwrap(device);

    /* Dump SPIR-V for vertex-pulling shader analysis.
     * VkShaderModuleCreateInfo: sType(4) pad(4) pNext(8) flags(4) pad(4) codeSize(8) pCode(8) */
    const uint8_t* ci = (const uint8_t*)pCreateInfo;
    uint64_t codeSize = *(const uint64_t*)(ci + 24);
    const uint32_t* pCode = *(const uint32_t* const*)(ci + 32);
    uint32_t wordCount = (uint32_t)(codeSize / 4);
    static int shader_dump_count = 0;

    /* Dump ALL shaders with >100 words (D2 = DXVK rendering device) */
    if (g_device_count >= 2 && pCode && codeSize >= 400 && shader_dump_count < 50) {
        char fname[128];
        snprintf(fname, sizeof(fname), "/tmp/shader_%04d_w%u.spv", shader_dump_count, wordCount);
        FILE* f = fopen(fname, "wb");
        if (f) {
            fwrite(pCode, 1, codeSize, f);
            fclose(f);
            LOG("[D%d] SHADER-DUMP: %s (%u words, %lu bytes)\n",
                g_device_count, fname, wordCount, (unsigned long)codeSize);
            shader_dump_count++;
        }
    }

    VkResult res = real_create_shader_module(real, pCreateInfo, pAllocator, pModule);
    LOG("[D%d] vkCreateShaderModule: dev=%p result=%d module=0x%llx words=%u\n",
        g_device_count, real, res, pModule ? (unsigned long long)*pModule : 0, wordCount);
    return res;
}

/* ==== Cmd* tracing ====
 *
 * Lightweight tracing for command buffer recording operations.
 * Shows exactly what ops DXVK records before QueueSubmit2.
 * All Cmd* functions take VkCommandBuffer (wrapper) as first arg.
 */

/* --- CmdPipelineBarrier2 (Vulkan 1.3 / KHR) ---
 *
 * VkDependencyInfo (x86-64):
 *   offset 0:  sType (4)
 *   offset 8:  pNext (8)
 *   offset 16: dependencyFlags (4)
 *   offset 20: memoryBarrierCount (4)
 *   offset 24: pMemoryBarriers (8)
 *   offset 32: bufferMemoryBarrierCount (4)
 *   offset 40: pBufferMemoryBarriers (8)
 *   offset 48: imageMemoryBarrierCount (4)
 *   offset 56: pImageMemoryBarriers (8)
 *
 * VkImageMemoryBarrier2 (x86-64, 96 bytes):
 *   offset 0:  sType (4)
 *   offset 8:  pNext (8)
 *   offset 16: srcStageMask (8)
 *   offset 24: srcAccessMask (8)
 *   offset 32: dstStageMask (8)
 *   offset 40: dstAccessMask (8)
 *   offset 48: oldLayout (4)
 *   offset 52: newLayout (4)
 *   offset 56: srcQueueFamilyIndex (4)
 *   offset 60: dstQueueFamilyIndex (4)
 *   offset 64: image (8)
 *   offset 72: subresourceRange (20): aspectMask(4)+baseMipLevel(4)+levelCount(4)+baseArrayLayer(4)+layerCount(4)
 */

typedef void (*PFN_vkCmdPipelineBarrier2)(void*, const void*);
static PFN_vkCmdPipelineBarrier2 real_cmd_pipeline_barrier2 = NULL;

/* CmdPipelineBarrier v1 function pointer for the converter */
typedef void (*PFN_vkCmdPipelineBarrierV1)(void*, uint32_t, uint32_t, uint32_t,
    uint32_t, const void*, uint32_t, const void*, uint32_t, const void*);
static PFN_vkCmdPipelineBarrierV1 real_cmd_pipeline_barrier_v1 = NULL;

/* Convert VkPipelineStageFlags2 (64-bit) to VkPipelineStageFlags (32-bit).
 * New v2-only bits (>= bit 32) mapped to ALL_COMMANDS for correctness. */
static uint32_t stage2_to_v1(uint64_t f) {
    uint32_t v = (uint32_t)(f & 0xFFFFFFFF);
    if (f >> 32) v |= 0x10000; /* VK_PIPELINE_STAGE_ALL_COMMANDS_BIT */
    return v;
}

/* Convert VkAccessFlags2 (64-bit) to VkAccessFlags (32-bit).
 * New v2-only bits mapped to MEMORY_READ|MEMORY_WRITE. */
static uint32_t access2_to_v1(uint64_t f) {
    uint32_t v = (uint32_t)(f & 0xFFFFFFFF);
    if (f >> 32) v |= 0x8000 | 0x10000; /* MEMORY_READ | MEMORY_WRITE */
    return v;
}

/* CmdPipelineBarrier2 → CmdPipelineBarrier v1 converter.
 * Bypasses FEX thunk marshaling of VkDependencyInfo/VkImageMemoryBarrier2
 * by converting to v1 structs (proven working through thunks) and calling
 * CmdPipelineBarrier instead.
 *
 * Struct sizes (x86-64):
 *   VkMemoryBarrier2:       48 bytes → VkMemoryBarrier:       24 bytes
 *   VkBufferMemoryBarrier2: 80 bytes → VkBufferMemoryBarrier: 56 bytes
 *   VkImageMemoryBarrier2:  96 bytes → VkImageMemoryBarrier:  72 bytes
 */
static void converter_CmdPipelineBarrier2(void* cmdBuf, const void* pDependencyInfo) {
    void* real = unwrap(cmdBuf);

    /* PREFER v2 passthrough (2 args, both in registers → safe for Vortek IPC).
     * v1 conversion has 10 args (4 on stack) which Vortek IPC may drop,
     * causing broken buffer/image barriers and stale GPU data. */
    if (real_cmd_pipeline_barrier2) {
        int op = ++g_cmd_op_count;
        static int b2_log = 0;
        if (++b2_log <= 30) {
            uint32_t memCount = 0, bufCount = 0, imgCount = 0;
            if (pDependencyInfo) {
                const uint8_t* di = (const uint8_t*)pDependencyInfo;
                memCount = *(const uint32_t*)(di + 20);
                bufCount = *(const uint32_t*)(di + 32);
                imgCount = *(const uint32_t*)(di + 48);
            }
            LOG("[CMD#%d] Barrier2 PASSTHROUGH: cb=%p mem=%u buf=%u img=%u\n",
                op, real, memCount, bufCount, imgCount);
        }
        real_cmd_pipeline_barrier2(real, pDependencyInfo);
        return;
    }

    if (!pDependencyInfo || !real_cmd_pipeline_barrier_v1) {
        return;
    }

    const uint8_t* di = (const uint8_t*)pDependencyInfo;
    uint32_t depFlags  = *(const uint32_t*)(di + 16);
    uint32_t memCount  = *(const uint32_t*)(di + 20);
    const uint8_t* pMem = *(const uint8_t**)(di + 24);
    uint32_t bufCount  = *(const uint32_t*)(di + 32);
    const uint8_t* pBuf = *(const uint8_t**)(di + 40);
    uint32_t imgCount  = *(const uint32_t*)(di + 48);
    const uint8_t* pImg = *(const uint8_t**)(di + 56);

    /* Limit to stack-allocated arrays */
    if (memCount > 16) memCount = 16;
    if (bufCount > 8)  bufCount = 8;
    if (imgCount > 16) imgCount = 16;

    uint32_t srcStages = 0, dstStages = 0;

    /* Convert VkMemoryBarrier2 (48 bytes) → VkMemoryBarrier (24 bytes) */
    uint8_t memV1[16 * 24];
    for (uint32_t i = 0; i < memCount; i++) {
        const uint8_t* b2 = pMem + i * 48;
        uint8_t* v1 = memV1 + i * 24;
        memset(v1, 0, 24);
        *(uint32_t*)(v1 + 0) = 46; /* VK_STRUCTURE_TYPE_MEMORY_BARRIER */
        srcStages |= stage2_to_v1(*(const uint64_t*)(b2 + 16));
        dstStages |= stage2_to_v1(*(const uint64_t*)(b2 + 32));
        *(uint32_t*)(v1 + 16) = access2_to_v1(*(const uint64_t*)(b2 + 24));
        *(uint32_t*)(v1 + 20) = access2_to_v1(*(const uint64_t*)(b2 + 40));
    }

    /* Convert VkBufferMemoryBarrier2 (80 bytes) → VkBufferMemoryBarrier (56 bytes) */
    uint8_t bufV1[8 * 56];
    for (uint32_t i = 0; i < bufCount; i++) {
        const uint8_t* b2 = pBuf + i * 80;
        uint8_t* v1 = bufV1 + i * 56;
        memset(v1, 0, 56);
        *(uint32_t*)(v1 + 0) = 44; /* VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER */
        srcStages |= stage2_to_v1(*(const uint64_t*)(b2 + 16));
        dstStages |= stage2_to_v1(*(const uint64_t*)(b2 + 32));
        *(uint32_t*)(v1 + 16) = access2_to_v1(*(const uint64_t*)(b2 + 24));
        *(uint32_t*)(v1 + 20) = access2_to_v1(*(const uint64_t*)(b2 + 40));
        *(uint32_t*)(v1 + 24) = *(const uint32_t*)(b2 + 48); /* srcQueueFamilyIndex */
        *(uint32_t*)(v1 + 28) = *(const uint32_t*)(b2 + 52); /* dstQueueFamilyIndex */
        *(uint64_t*)(v1 + 32) = *(const uint64_t*)(b2 + 56); /* buffer */
        *(uint64_t*)(v1 + 40) = *(const uint64_t*)(b2 + 64); /* offset */
        *(uint64_t*)(v1 + 48) = *(const uint64_t*)(b2 + 72); /* size */
    }

    /* Convert VkImageMemoryBarrier2 (96 bytes) → VkImageMemoryBarrier (72 bytes) */
    uint8_t imgV1[16 * 72];
    for (uint32_t i = 0; i < imgCount; i++) {
        const uint8_t* b2 = pImg + i * 96;
        uint8_t* v1 = imgV1 + i * 72;
        memset(v1, 0, 72);
        *(uint32_t*)(v1 + 0) = 45; /* VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER */
        srcStages |= stage2_to_v1(*(const uint64_t*)(b2 + 16));
        dstStages |= stage2_to_v1(*(const uint64_t*)(b2 + 32));
        *(uint32_t*)(v1 + 16) = access2_to_v1(*(const uint64_t*)(b2 + 24));
        *(uint32_t*)(v1 + 20) = access2_to_v1(*(const uint64_t*)(b2 + 40));
        *(uint32_t*)(v1 + 24) = *(const uint32_t*)(b2 + 48); /* oldLayout */
        *(uint32_t*)(v1 + 28) = *(const uint32_t*)(b2 + 52); /* newLayout */
        *(uint32_t*)(v1 + 32) = *(const uint32_t*)(b2 + 56); /* srcQueueFamilyIndex */
        *(uint32_t*)(v1 + 36) = *(const uint32_t*)(b2 + 60); /* dstQueueFamilyIndex */
        *(uint64_t*)(v1 + 40) = *(const uint64_t*)(b2 + 64); /* image */
        memcpy(v1 + 48, b2 + 72, 20);                        /* subresourceRange */
    }

    if (!srcStages) srcStages = 0x1;    /* TOP_OF_PIPE */
    if (!dstStages) dstStages = 0x2000; /* BOTTOM_OF_PIPE */

    int op = ++g_cmd_op_count;
    LOG("[CMD#%d] Barrier2->v1: cb=%p src=0x%x dst=0x%x dep=0x%x mem=%u buf=%u img=%u\n",
        op, real, srcStages, dstStages, depFlags, memCount, bufCount, imgCount);
    /* Log image barrier layout transitions (first 4) to diagnose rendering */
    for (uint32_t i = 0; i < imgCount && i < 4; i++) {
        uint32_t old_l = *(uint32_t*)(imgV1 + i * 72 + 24);
        uint32_t new_l = *(uint32_t*)(imgV1 + i * 72 + 28);
        uint64_t img_h = *(uint64_t*)(imgV1 + i * 72 + 40);
        LOG("[CMD#%d]   img[%u] 0x%llx layout %u->%u\n",
            op, i, (unsigned long long)img_h, old_l, new_l);
    }

    real_cmd_pipeline_barrier_v1(real, srcStages, dstStages, depFlags,
        memCount, memCount ? (const void*)memV1 : NULL,
        bufCount, bufCount ? (const void*)bufV1 : NULL,
        imgCount, imgCount ? (const void*)imgV1 : NULL);
}

/* --- CmdCopyBuffer --- */
typedef void (*PFN_vkCmdCopyBuffer)(void*, uint64_t, uint64_t, uint32_t, const void*);
static PFN_vkCmdCopyBuffer real_cmd_copy_buffer = NULL;

/* Track SSBO[0] buffer for staging copy readback (set by template update) */
static uint64_t g_ssbo0_buf = 0;
static uint64_t g_ssbo0_off = 0;
static uint64_t g_ssbo0_range = 0;

static void trace_CmdCopyBuffer(void* cmdBuf, uint64_t srcBuf, uint64_t dstBuf,
                                 uint32_t regionCount, const void* pRegions) {
    void* real = unwrap(cmdBuf);
    int op = ++g_cmd_op_count;
    static int copy_log = 0;
    copy_log++;

    /* VkBufferCopy: srcOffset(8) + dstOffset(8) + size(8) = 24 bytes */
    /* Check if this copy targets the SSBO[0] constant buffer */
    if (g_ssbo0_buf && dstBuf == g_ssbo0_buf) {
        for (uint32_t r = 0; r < regionCount; r++) {
            const uint8_t* reg = (const uint8_t*)pRegions + r * 24;
            uint64_t srcOff = *(const uint64_t*)(reg + 0);
            uint64_t dstOff = *(const uint64_t*)(reg + 8);
            uint64_t sz     = *(const uint64_t*)(reg + 16);
            /* Check if this region overlaps SSBO[0] descriptor range */
            if (dstOff <= g_ssbo0_off && dstOff + sz > g_ssbo0_off) {
                /* Found it — read staging data */
                uint64_t data_start = srcOff + (g_ssbo0_off - dstOff);
                void* staging_ptr = lookup_ubo_ptr(srcBuf, data_start);
                static int staging_dump = 0;
                staging_dump++;
                if (staging_dump <= 10) {
                    LOG("[CMD#%d] STAGING-CB: src=0x%llx dst=0x%llx srcOff=%lu dstOff=%lu sz=%lu -> SSBO[0] staging_ptr=%p\n",
                        op, (unsigned long long)srcBuf, (unsigned long long)dstBuf,
                        (unsigned long)srcOff, (unsigned long)dstOff, (unsigned long)sz, staging_ptr);
                    if (staging_ptr) {
                        const uint32_t* u = (const uint32_t*)staging_ptr;
                        const float* f = (const float*)staging_ptr;
                        uint32_t nw = g_ssbo0_range / 4;
                        if (nw > 64) nw = 64;
                        for (uint32_t wi = 0; wi < nw; wi += 4) {
                            LOG("  CB[%2u]: %08x(%g) %08x(%g) %08x(%g) %08x(%g)\n",
                                wi, u[wi], f[wi], u[wi+1], f[wi+1],
                                u[wi+2], f[wi+2], u[wi+3], f[wi+3]);
                        }
                    }
                }
            }
        }
    }

    if (copy_log <= 50)
        LOG("[CMD#%d] CmdCopyBuffer: cb=%p src=0x%llx dst=0x%llx regions=%u\n",
            op, real, (unsigned long long)srcBuf, (unsigned long long)dstBuf, regionCount);
    real_cmd_copy_buffer(real, srcBuf, dstBuf, regionCount, pRegions);
}

/* --- CmdCopyBufferToImage --- */
typedef void (*PFN_vkCmdCopyBufToImg)(void*, uint64_t, uint64_t, uint32_t, uint32_t, const void*);
static PFN_vkCmdCopyBufToImg real_cmd_copy_buf_to_img = NULL;

/* Forward declaration for BC clear fallback */
typedef void (*PFN_vkCmdClearColorImage)(void*, uint64_t, uint32_t, const void*, uint32_t, const void*);
static PFN_vkCmdClearColorImage real_cmd_clear_color;

static void trace_CmdCopyBufferToImage(void* cmdBuf, uint64_t buffer, uint64_t image,
                                         uint32_t imageLayout, uint32_t regionCount,
                                         const void* pRegions) {
    void* real = unwrap(cmdBuf);
    int op = ++g_cmd_op_count;

    /* BC-substituted images: buffer has BC data but image is RGBA8.
     * Can't copy directly. Instead clear to MAGENTA so we can check if
     * geometry is correct (vertex position test). */
    int bc_idx = bc_img_lookup(image);
    if (bc_idx >= 0) {
        static int bc_clear_count = 0;
        bc_clear_count++;
        /* Lazily resolve CmdClearColorImage if not yet captured by GDPA */
        if (!real_cmd_clear_color && thunk_lib)
            real_cmd_clear_color = (PFN_vkCmdClearColorImage)dlsym(thunk_lib, "vkCmdClearColorImage");
        if (real_cmd_clear_color) {
            /* VkClearColorValue: float32[4] = {R,G,B,A} */
            float clearColor[4] = {1.0f, 0.0f, 1.0f, 1.0f}; /* MAGENTA */
            /* VkImageSubresourceRange: aspectMask(4) baseMip(4) levelCount(4) baseLayer(4) layerCount(4) = 20 bytes */
            uint8_t range[20];
            *(uint32_t*)(range + 0) = 1;  /* VK_IMAGE_ASPECT_COLOR_BIT */
            *(uint32_t*)(range + 4) = 0;  /* baseMipLevel */
            *(uint32_t*)(range + 8) = 1;  /* levelCount (VK_REMAINING was -1 but use 1 for safety) */
            *(uint32_t*)(range + 12) = 0; /* baseArrayLayer */
            *(uint32_t*)(range + 16) = 1; /* layerCount */
            /* Use GENERAL layout (7) since we don't know the current layout */
            real_cmd_clear_color(real, image, 7, clearColor, 1, range);
        }
        if (bc_clear_count <= 5) {
            LOG("[CMD#%d] CmdCopyBufToImg: BC img=0x%llx cleared MAGENTA (bc_fmt=%u, #%d)\n",
                op, (unsigned long long)image, g_bc_images[bc_idx].bc_format, bc_clear_count);
        }
        return; /* skip the actual copy */
    }

    LOG("[CMD#%d] CmdCopyBufferToImage: cb=%p buf=0x%llx img=0x%llx layout=%u regions=%u\n",
        op, real, (unsigned long long)buffer, (unsigned long long)image,
        imageLayout, regionCount);
    real_cmd_copy_buf_to_img(real, buffer, image, imageLayout, regionCount, pRegions);
}

/* --- CmdCopyBufferToImage2 (Vulkan 1.3 / KHR) --- */
/* VkCopyBufferToImageInfo2 layout (x86-64):
 *   offset 0:  sType (uint32_t) + pad
 *   offset 8:  pNext (pointer)
 *   offset 16: srcBuffer (uint64_t)
 *   offset 24: dstImage (uint64_t)
 *   offset 32: dstImageLayout (uint32_t)
 *   offset 36: regionCount (uint32_t)
 *   offset 40: pRegions (pointer)
 */
typedef void (*PFN_vkCmdCopyBufToImg2)(void*, const void*);
static PFN_vkCmdCopyBufToImg2 real_cmd_copy_buf_to_img2 = NULL;

static void trace_CmdCopyBufferToImage2(void* cmdBuf, const void* pCopyInfo) {
    void* real = unwrap(cmdBuf);
    int op = ++g_cmd_op_count;

    uint64_t dst_image = 0;
    if (pCopyInfo)
        dst_image = *(const uint64_t*)((const char*)pCopyInfo + 24);

    int bc_idx = bc_img_lookup(dst_image);
    if (bc_idx >= 0) {
        static int bc_skip_count2 = 0;
        bc_skip_count2++;
        if (bc_skip_count2 <= 10) {
            LOG("[CMD#%d] CmdCopyBufferToImage2: SKIP BC img=0x%llx (bc_fmt=%u, #%d)\n",
                op, (unsigned long long)dst_image, g_bc_images[bc_idx].bc_format, bc_skip_count2);
        }
        return; /* skip — BC data can't be copied into RGBA image */
    }

    LOG("[CMD#%d] CmdCopyBufferToImage2: cb=%p img=0x%llx\n",
        op, real, (unsigned long long)dst_image);
    real_cmd_copy_buf_to_img2(real, pCopyInfo);
}

/* real_cmd_clear_color already forward-declared above CmdCopyBufferToImage */
static uint64_t g_last_render_image = 0;

/* --- CmdCopyImageToBuffer --- */
typedef void (*PFN_vkCmdCopyImgToBuf)(void*, uint64_t, uint32_t, uint64_t, uint32_t, const void*);
static PFN_vkCmdCopyImgToBuf real_cmd_copy_img_to_buf = NULL;

/* Diagnostic: inject CmdClearColorImage RED before CopyImageToBuffer
 * to verify the copy pipeline works. If staging reads red, the pipeline
 * works but DXVK renders black. If staging reads zero, pipeline is broken. */
static int g_citb_diag_done = 0;
static void* g_last_render_cb = NULL; /* wrapped CB handle that last called CmdBeginRendering */

/* --- Per-CB render pass tracking ---
 * Track which command buffers are currently inside a render pass.
 * CmdBeginRendering sets the flag, CmdEndRendering clears it.
 * CmdDrawIndexed checks the flag to detect draws outside render pass. */
#define MAX_CB_TRACK 64
static struct {
    void* cb_real;       /* unwrapped CB handle */
    int   in_render_pass;
    uint32_t rp_w, rp_h; /* render area from CmdBeginRendering */
} g_cb_state[MAX_CB_TRACK];
static int g_cb_state_count = 0;

static int cb_state_idx(void* real) {
    for (int i = 0; i < g_cb_state_count; i++)
        if (g_cb_state[i].cb_real == real) return i;
    if (g_cb_state_count < MAX_CB_TRACK) {
        int idx = g_cb_state_count++;
        g_cb_state[idx].cb_real = real;
        g_cb_state[idx].in_render_pass = 0;
        return idx;
    }
    return -1;
}

/* --- Pipeline VIS cache ---
 * Track vertex input state per pipeline for draw-time binding checks. */
#define MAX_PIPE_CACHE 256
static struct {
    uint64_t pipeline;
    uint32_t bindingCount;
    uint32_t strides[8];
    uint32_t bindingSlots[8]; /* actual binding slot numbers */
} g_pipe_vis[MAX_PIPE_CACHE];
static int g_pipe_vis_count = 0;

/* Currently bound pipeline handle (set by CmdBindPipeline) */
static uint64_t g_cur_pipeline = 0;

/* Forward declare EDS function pointer array (defined at file scope before wrapped_GDPA).
 * Used by trace_CmdDrawIndexed to inject dynamic state into secondary CBs. */
#define MAX_DYN_WRAPPERS 64
static PFN_vkVoidFunction dyn_real[MAX_DYN_WRAPPERS];

static void trace_CmdCopyImageToBuffer(void* cmdBuf, uint64_t image, uint32_t imageLayout,
                                         uint64_t buffer, uint32_t regionCount,
                                         const void* pRegions) {
    void* real = unwrap(cmdBuf);
    int op = ++g_cmd_op_count;
    LOG("[CMD#%d] CmdCopyImageToBuffer: cb=%p img=0x%llx layout=%u buf=0x%llx regions=%u\n",
        op, real, (unsigned long long)image, imageLayout,
        (unsigned long long)buffer, regionCount);

    /* DIAGNOSTIC: fill the STAGING BUFFER directly with 0xDEADBEEF via CmdFillBuffer
     * BEFORE the image copy. This tests whether the buffer↔memory mapping works:
     * - If staging reads 0xDEADBEEF → buffer is connected to mapped memory, image copy writes zeros
     * - If staging reads 0x00000000 → buffer↔memory mapping is BROKEN (GPU writes don't reach CPU map) */
    static int citb_count = 0;
    citb_count++;
    if (!g_citb_diag_done) {
        g_citb_diag_done = 1;
        typedef void (*PFN_CmdFillBuf)(void*, uint64_t, uint64_t, uint64_t, uint32_t);
        PFN_CmdFillBuf fn_fill = NULL;
        if (thunk_lib)
            fn_fill = (PFN_CmdFillBuf)dlsym(thunk_lib, "vkCmdFillBuffer");
        if (fn_fill) {
            LOG("[DIAG] CmdFillBuffer 0xDEADBEEF → buf=0x%llx (BEFORE copy, frame %d)\n",
                (unsigned long long)buffer, citb_count);
            /* Fill entire buffer with 0xDEADBEEF — this is a GPU command in the same CB */
            fn_fill(real, buffer, 0, (uint64_t)-1, 0xDEADBEEF);
        } else {
            LOG("[DIAG] Could not resolve vkCmdFillBuffer!\n");
        }
    }

    real_cmd_copy_img_to_buf(real, image, imageLayout, buffer, regionCount, pRegions);
}

/* --- CmdClearColorImage --- */
/* (typedef + declaration moved to forward decl before CmdCopyImageToBuffer) */

static void trace_CmdClearColorImage(void* cmdBuf, uint64_t image, uint32_t layout,
                                      const void* pColor, uint32_t rangeCount,
                                      const void* pRanges) {
    void* real = unwrap(cmdBuf);
    int op = ++g_cmd_op_count;
    LOG("[CMD#%d] CmdClearColorImage: cb=%p img=0x%llx layout=%u ranges=%u\n",
        op, real, (unsigned long long)image, layout, rangeCount);
    real_cmd_clear_color(real, image, layout, pColor, rangeCount, pRanges);
}

/* --- CmdClearDepthStencilImage --- */
typedef void (*PFN_vkCmdClearDSImage)(void*, uint64_t, uint32_t, const void*, uint32_t, const void*);
static PFN_vkCmdClearDSImage real_cmd_clear_ds = NULL;

static void trace_CmdClearDepthStencilImage(void* cmdBuf, uint64_t image, uint32_t layout,
                                              const void* pDepthStencil, uint32_t rangeCount,
                                              const void* pRanges) {
    void* real = unwrap(cmdBuf);
    int op = ++g_cmd_op_count;
    LOG("[CMD#%d] CmdClearDepthStencilImage: cb=%p img=0x%llx layout=%u ranges=%u\n",
        op, real, (unsigned long long)image, layout, rangeCount);
    real_cmd_clear_ds(real, image, layout, pDepthStencil, rangeCount, pRanges);
}

/* --- CmdBeginRendering (Vulkan 1.3 / KHR dynamic rendering) --- */
typedef void (*PFN_vkCmdBeginRendering)(void*, const void*);
static PFN_vkCmdBeginRendering real_cmd_begin_rendering = NULL;

static void trace_CmdBeginRendering(void* cmdBuf, const void* pRenderingInfo) {
    void* real = unwrap(cmdBuf);
    int op = ++g_cmd_op_count;
    /* VkRenderingInfo (x86-64):
     * offset 0:  sType (4)
     * offset 4:  pad (4)
     * offset 8:  pNext (8)
     * offset 16: flags (4)
     * offset 20: renderArea.offset.x (4)
     * offset 24: renderArea.offset.y (4)
     * offset 28: renderArea.extent.width (4)
     * offset 32: renderArea.extent.height (4)
     * offset 36: layerCount (4)
     * offset 40: viewMask (4)
     * offset 44: colorAttachmentCount (4)
     * offset 48: pColorAttachments (8)
     * offset 56: pDepthAttachment (8)
     * offset 64: pStencilAttachment (8) */
    uint32_t colorCount = 0, w = 0, h = 0, flags = 0, layers = 0;
    const void* pDepth = NULL;
    const void* pStencil = NULL;
    if (pRenderingInfo) {
        flags = *(const uint32_t*)((const char*)pRenderingInfo + 16);
        w = *(const uint32_t*)((const char*)pRenderingInfo + 28);
        h = *(const uint32_t*)((const char*)pRenderingInfo + 32);
        layers = *(const uint32_t*)((const char*)pRenderingInfo + 36);
        colorCount = *(const uint32_t*)((const char*)pRenderingInfo + 44);
        pDepth = *(const void**)((const char*)pRenderingInfo + 56);
        pStencil = *(const void**)((const char*)pRenderingInfo + 64);
    }
    /* Extract imageView from first color attachment to trace render target.
     * VkRenderingAttachmentInfo x86-64: offset 16 = imageView (uint64_t) */
    uint64_t att0_view = 0, att0_src_img = 0;
    if (colorCount > 0) {
        const char* pColorAtts = *(const char**)((const char*)pRenderingInfo + 48);
        if (pColorAtts) {
            att0_view = *(const uint64_t*)(pColorAtts + 16);
            att0_src_img = iv_lookup_image(att0_view);
        }
    }
    LOG("[CMD#%d] CmdBeginRendering: cb=%p %ux%u colorAtts=%u view=0x%llx img=0x%llx\n",
        op, real, w, h, colorCount,
        (unsigned long long)att0_view, (unsigned long long)att0_src_img);
    /* Save for diagnostics */
    g_last_render_image = att0_src_img;
    g_last_render_cb = cmdBuf;
    /* Track per-CB render pass state */
    {
        int idx = cb_state_idx(real);
        if (idx >= 0) {
            g_cb_state[idx].in_render_pass = 1;
            g_cb_state[idx].rp_w = w;
            g_cb_state[idx].rp_h = h;
        }
    }
    real_cmd_begin_rendering(real, pRenderingInfo);

    /* GREEN diagnostic removed — render pass confirmed working */
}

/* --- CmdEndRendering + RED clear diagnostic --- */
typedef void (*PFN_vkCmdEndRendering)(void*);
static PFN_vkCmdEndRendering real_cmd_end_rendering = NULL;

static void trace_CmdEndRendering(void* cmdBuf) {
    void* real = unwrap(cmdBuf);
    real_cmd_end_rendering(real);

    /* Lazily resolve CmdClearColorImage if not yet available */
    if (!real_cmd_clear_color && thunk_lib)
        real_cmd_clear_color = (PFN_vkCmdClearColorImage)dlsym(thunk_lib, "vkCmdClearColorImage");

    /* Track per-CB render pass state */
    {
        int idx = cb_state_idx(real);
        if (idx >= 0) g_cb_state[idx].in_render_pass = 0;
    }
    int op = ++g_cmd_op_count;
    LOG("[CMD#%d] CmdEndRendering: cb=%p img=0x%llx\n",
        op, real, (unsigned long long)g_last_render_image);
}

/* ---- EndCommandBuffer ---- */
static VkResult trace_EndCommandBuffer(void* cmdBuf) {
    void* real = unwrap(cmdBuf);
    VkResult res = real_end_cmd_buf(real);
    LOG("[D%d] vkEndCommandBuffer: cmdBuf=%p(real=%p) result=%d\n",
        g_device_count, cmdBuf, real, res);
    return res;
}

/* --- CmdBindPipeline --- */
typedef void (*PFN_vkCmdBindPipeline)(void*, uint32_t, uint64_t);
static PFN_vkCmdBindPipeline real_cmd_bind_pipeline = NULL;

static void trace_CmdBindPipeline(void* cmdBuf, uint32_t bindPoint, uint64_t pipeline) {
    void* real = unwrap(cmdBuf);
    int op = ++g_cmd_op_count;
    LOG("[CMD#%d] CmdBindPipeline: cb=%p bindPoint=%u(%s) pipeline=0x%llx\n",
        op, real, bindPoint,
        bindPoint == 0 ? "GRAPHICS" : bindPoint == 1 ? "COMPUTE" : "RAYTRACE",
        (unsigned long long)pipeline);
    if (bindPoint == 0) {
        g_cur_pipeline = pipeline;
    }
    /* Record for secondary CB replay */
    {
        ReplayCmd* cmd = add_replay_cmd(cmdBuf, RCMD_BIND_PIPELINE);
        if (cmd) { cmd->pipe.bindPoint = bindPoint; cmd->pipe.pipeline = pipeline; }
    }
    real_cmd_bind_pipeline(real, bindPoint, pipeline);
}

/* --- Per-CB vertex buffer tracking for readback --- */
#define MAX_VB_SLOTS 8
typedef struct {
    uint64_t buffer;
    uint64_t offset;
    uint64_t size;
    uint64_t stride;
    int      bound; /* set to 1 when VB2 binds this slot */
} VBSlotInfo;
static VBSlotInfo g_last_vb[MAX_VB_SLOTS];  /* most recently bound VB per slot */
static uint32_t g_last_vb_max = 0; /* highest bound slot + 1 */

/* --- Index buffer tracking for readback --- */
static uint64_t g_last_ib_buf = 0;
static uint64_t g_last_ib_off = 0;
static uint32_t g_last_ib_type = 0; /* 0=UINT16, 1=UINT32 */

/* --- Per-descriptor-set UBO tracking (from vkUpdateDescriptorSets type=6) --- */
#define MAX_LAST_UBO 16
typedef struct {
    uint64_t buffer;
    uint64_t offset;
    uint64_t range;
} LastUboEntry;

/* Map descriptor set handle → UBO bindings */
#define MAX_SET_UBO_TRACK 512
typedef struct {
    uint64_t set_handle;
    LastUboEntry ubos[MAX_LAST_UBO];
    int ubo_count;
} SetUboTrack;
static SetUboTrack g_set_ubo_track[MAX_SET_UBO_TRACK];
static int g_set_ubo_track_count = 0;

/* Currently bound UBOs (updated at CmdBindDescriptorSets from set→UBO map) */
static LastUboEntry g_last_ubo[MAX_LAST_UBO];
static int g_last_ubo_count = 0;

/* Last viewport/scissor values for 576-draw diagnostics */
static float g_last_viewport[6] = {0}; /* x, y, w, h, minD, maxD */
static uint32_t g_last_scissor[4] = {0}; /* x, y, w, h */
static uint32_t g_last_viewport_set = 0;

/* Last push constant data for diagnostics */
static uint8_t g_last_pc_data[256];
static uint32_t g_last_pc_size = 0;
static uint32_t g_last_pc_stages = 0;

/* --- CmdDraw --- */
typedef void (*PFN_vkCmdDraw)(void*, uint32_t, uint32_t, uint32_t, uint32_t);
static PFN_vkCmdDraw real_cmd_draw = NULL;

static void trace_CmdDraw(void* cmdBuf, uint32_t vertexCount, uint32_t instanceCount,
                           uint32_t firstVertex, uint32_t firstInstance) {
    void* real = unwrap(cmdBuf);
    int op = ++g_cmd_op_count;
    LOG("[CMD#%d] CmdDraw: cb=%p verts=%u inst=%u\n",
        op, real, vertexCount, instanceCount);
    /* Record for secondary CB replay */
    {
        ReplayCmd* cmd = add_replay_cmd(cmdBuf, RCMD_DRAW);
        if (cmd) {
            cmd->draw.vertexCount = vertexCount;
            cmd->draw.instanceCount = instanceCount;
            cmd->draw.firstVertex = firstVertex;
            cmd->draw.firstInstance = firstInstance;
        }
    }
    real_cmd_draw(real, vertexCount, instanceCount, firstVertex, firstInstance);
}

/* --- CmdDrawIndexed --- */
typedef void (*PFN_vkCmdDrawIndexed)(void*, uint32_t, uint32_t, uint32_t, int32_t, uint32_t);
static PFN_vkCmdDrawIndexed real_cmd_draw_indexed = NULL;

static int g_vtx_readback_count = 0;

static void trace_CmdDrawIndexed(void* cmdBuf, uint32_t indexCount, uint32_t instanceCount,
                                  uint32_t firstIndex, int32_t vertexOffset,
                                  uint32_t firstInstance) {
    void* real = unwrap(cmdBuf);
    int op = ++g_cmd_op_count;
    static int di_diag_count = 0;
    di_diag_count++;

    LOG("[CMD#%d] CmdDrawIndexed: cb=%p indices=%u firstIdx=%u inst=%u vtxOff=%d\n",
        op, real, indexCount, firstIndex, instanceCount, vertexOffset);

    /* === DIAGNOSTIC: Read back VB, IB, and UBO data at draw time === */
    /* Focus on 3D MESH draws (bCount>=2 pipeline) — these are the exploded ones.
     * HUD draws (bCount=1, stride=24) render correctly. */
    int cur_pipe_bcount = 0;
    for (int p = 0; p < g_pipe_vis_count; p++) {
        if (g_pipe_vis[p].pipeline == g_cur_pipeline) {
            cur_pipe_bcount = g_pipe_vis[p].bindingCount;
            break;
        }
    }
    static int mesh_draw_diag = 0;
    static int hud_draw_diag = 0;
    int do_diag = 0;
    if (cur_pipe_bcount >= 2) {
        /* 3D mesh pipeline — ALWAYS capture first 20 */
        do_diag = (++mesh_draw_diag <= 20);
    } else if (indexCount > 6 && hud_draw_diag < 3) {
        /* HUD non-quad — capture a few for comparison */
        do_diag = 1;
        hud_draw_diag++;
    }
    if (do_diag) {
        /* 1. Index buffer readback */
        if (g_last_ib_buf) {
            void* ib_ptr = lookup_ubo_ptr(g_last_ib_buf, g_last_ib_off);
            if (ib_ptr) {
                uint32_t n = indexCount < 12 ? indexCount : 12;
                if (g_last_ib_type == 0) { /* UINT16 */
                    uint16_t* idx = (uint16_t*)ib_ptr;
                    char buf[256]; int pos = 0;
                    for (uint32_t i = 0; i < n; i++)
                        pos += snprintf(buf + pos, sizeof(buf) - pos, "%u ", idx[firstIndex + i]);
                    LOG("  IB-READBACK(u16, n=%u): %s\n", indexCount, buf);
                } else { /* UINT32 */
                    uint32_t* idx = (uint32_t*)ib_ptr;
                    char buf[256]; int pos = 0;
                    for (uint32_t i = 0; i < n; i++)
                        pos += snprintf(buf + pos, sizeof(buf) - pos, "%u ", idx[firstIndex + i]);
                    LOG("  IB-READBACK(u32, n=%u): %s\n", indexCount, buf);
                }
            } else {
                LOG("  IB-READBACK: FAILED buf=0x%lx off=%lu\n",
                    (unsigned long)g_last_ib_buf, (unsigned long)g_last_ib_off);
            }
        }

        /* 2. Vertex buffer slot 0 readback — read vertices referenced by indices */
        if (g_last_vb[0].bound && g_last_vb[0].buffer) {
            uint64_t stride = g_last_vb[0].stride;
            if (stride == 0) stride = 24; /* fallback to baked stride */
            void* vb_base = lookup_ubo_ptr(g_last_vb[0].buffer, g_last_vb[0].offset);
            if (vb_base) {
                LOG("  VB0: buf=0x%lx off=%lu stride=%lu base=%p vtxOff=%d\n",
                    (unsigned long)g_last_vb[0].buffer, (unsigned long)g_last_vb[0].offset,
                    (unsigned long)stride, vb_base, vertexOffset);
                /* Read first 6 vertices — mesh has stride=56 so position is XYZW at offset 0 */
                uint32_t n = 6;
                for (uint32_t v = 0; v < n; v++) {
                    int32_t vi = vertexOffset + (int32_t)v;
                    float* f = (float*)((uint8_t*)vb_base + vi * stride);
                    /* For mesh (stride=56): pos=f0..f3(XYZW), gap, normal@32, color@40, uv@48 */
                    if (stride >= 56) {
                        uint8_t* base = (uint8_t*)vb_base + vi * stride;
                        float px = *(float*)(base + 0);
                        float py = *(float*)(base + 4);
                        float pz = *(float*)(base + 8);
                        float pw = *(float*)(base + 12);
                        float u = *(float*)(base + 48);
                        float v2 = *(float*)(base + 52);
                        LOG("  VB[%d]: pos=(%.4f,%.4f,%.4f,%.4f) uv=(%.4f,%.4f)\n",
                            vi, px, py, pz, pw, u, v2);
                    } else {
                        LOG("  VB[%d]: f0=%.4f f1=%.4f f2=%.4f f3=%.4f f4=%.4f f5=%.4f\n",
                            vi, f[0], f[1], f[2], f[3], f[4], f[5]);
                    }
                }
            } else {
                LOG("  VB-READBACK: FAILED buf=0x%lx off=%lu\n",
                    (unsigned long)g_last_vb[0].buffer, (unsigned long)g_last_vb[0].offset);
            }
        } else {
            LOG("  VB-READBACK: slot 0 not bound (bound=%d buf=0x%lx)\n",
                g_last_vb[0].bound, (unsigned long)g_last_vb[0].buffer);
        }

        /* 2b. Instance buffer (binding 1) readback — 4x4 transform matrix */
        if (g_last_vb[1].bound && g_last_vb[1].buffer) {
            uint64_t inst_stride = g_last_vb[1].stride;
            if (inst_stride == 0) inst_stride = 64;
            void* inst_base = lookup_ubo_ptr(g_last_vb[1].buffer, g_last_vb[1].offset);
            if (inst_base) {
                LOG("  VB1(inst): buf=0x%lx off=%lu stride=%lu base=%p\n",
                    (unsigned long)g_last_vb[1].buffer, (unsigned long)g_last_vb[1].offset,
                    (unsigned long)inst_stride, inst_base);
                /* Read first 2 instances (4x4 float matrix each = 64 bytes) */
                for (uint32_t inst = 0; inst < 2 && inst < instanceCount; inst++) {
                    float* m = (float*)((uint8_t*)inst_base + inst * inst_stride);
                    LOG("  INST[%u] row0: [%.4f %.4f %.4f %.4f]\n", inst, m[0], m[1], m[2], m[3]);
                    LOG("  INST[%u] row1: [%.4f %.4f %.4f %.4f]\n", inst, m[4], m[5], m[6], m[7]);
                    LOG("  INST[%u] row2: [%.4f %.4f %.4f %.4f]\n", inst, m[8], m[9], m[10], m[11]);
                    LOG("  INST[%u] row3: [%.4f %.4f %.4f %.4f]\n", inst, m[12], m[13], m[14], m[15]);
                    /* Hex dump row3 for NaN analysis */
                    {
                        uint32_t h12, h13, h14, h15;
                        memcpy(&h12, &m[12], 4); memcpy(&h13, &m[13], 4);
                        memcpy(&h14, &m[14], 4); memcpy(&h15, &m[15], 4);
                        LOG("  INST[%u] row3hex: [0x%08x 0x%08x 0x%08x 0x%08x]\n", inst, h12, h13, h14, h15);
                    }
                }
            } else {
                LOG("  VB1(inst): FAILED lookup buf=0x%lx off=%lu\n",
                    (unsigned long)g_last_vb[1].buffer, (unsigned long)g_last_vb[1].offset);
            }
        }

        /* 3. UBO/SSBO readback — dump ALL tracked entries */
        LOG("  UBO count=%d\n", g_last_ubo_count);
        for (int u = 0; u < g_last_ubo_count && u < MAX_LAST_UBO; u++) {
            if (!g_last_ubo[u].buffer) continue;
            void* ubo_ptr = lookup_ubo_ptr(g_last_ubo[u].buffer, g_last_ubo[u].offset);
            if (ubo_ptr) {
                float* m = (float*)ubo_ptr;
                /* Count non-zero floats in the first 64 bytes (16 floats) */
                int nz = 0;
                for (int f = 0; f < 16; f++) if (m[f] != 0.0f) nz++;
                LOG("  UBO[%d] buf=0x%lx off=%lu range=%lu nonzero=%d:\n",
                    u, (unsigned long)g_last_ubo[u].buffer,
                    (unsigned long)g_last_ubo[u].offset,
                    (unsigned long)g_last_ubo[u].range, nz);
                LOG("    [%e %e %e %e]\n", m[0], m[1], m[2], m[3]);
                LOG("    [%e %e %e %e]\n", m[4], m[5], m[6], m[7]);
                LOG("    [%e %e %e %e]\n", m[8], m[9], m[10], m[11]);
                LOG("    [%e %e %e %e]\n", m[12], m[13], m[14], m[15]);
            } else {
                LOG("  UBO[%d] FAILED buf=0x%lx off=%lu\n",
                    u, (unsigned long)g_last_ubo[u].buffer,
                    (unsigned long)g_last_ubo[u].offset);
            }
        }
        if (g_last_ubo_count == 0)
            LOG("  UBO-READBACK: no UBOs tracked\n");

        /* 4. Viewport/Scissor/PushConstants/Pipeline — always for mesh draws */
        {
            LOG("  VIEWPORT: x=%.1f y=%.1f w=%.1f h=%.1f minD=%.3f maxD=%.3f (set=%u)\n",
                g_last_viewport[0], g_last_viewport[1], g_last_viewport[2],
                g_last_viewport[3], g_last_viewport[4], g_last_viewport[5],
                g_last_viewport_set);
            LOG("  SCISSOR: x=%u y=%u w=%u h=%u\n",
                g_last_scissor[0], g_last_scissor[1], g_last_scissor[2], g_last_scissor[3]);
            /* Pipeline vertex input lookup — which pipeline is bound? */
            LOG("  CUR_PIPELINE: 0x%llx\n", (unsigned long long)g_cur_pipeline);
            int found_pipe = 0;
            for (int p = 0; p < g_pipe_vis_count; p++) {
                if (g_pipe_vis[p].pipeline == g_cur_pipeline) {
                    LOG("  PIPE-VIS: bindings=%u", g_pipe_vis[p].bindingCount);
                    for (uint32_t b = 0; b < g_pipe_vis[p].bindingCount && b < 8; b++)
                        LOG(" slot%u:stride%u", g_pipe_vis[p].bindingSlots[b], g_pipe_vis[p].strides[b]);
                    LOG("\n");
                    found_pipe = 1;
                    break;
                }
            }
            if (!found_pipe)
                LOG("  PIPE-VIS: NOT FOUND in cache (%d entries)\n", g_pipe_vis_count);
            /* Compare pipeline baked strides vs DXVK's VB2 strides */
            LOG("  VB2-STRIDES-FROM-DXVK:");
            for (uint32_t s = 0; s < g_last_vb_max && s < 4; s++) {
                if (g_last_vb[s].bound)
                    LOG(" slot%u:stride%lu buf=0x%lx off=%lu",
                        s, (unsigned long)g_last_vb[s].stride,
                        (unsigned long)g_last_vb[s].buffer,
                        (unsigned long)g_last_vb[s].offset);
            }
            LOG("\n");
            /* Push constants data dump */
            if (g_last_pc_size > 0) {
                LOG("  PUSH_CONSTS: stages=0x%x size=%u\n", g_last_pc_stages, g_last_pc_size);
                const uint32_t* u = (const uint32_t*)g_last_pc_data;
                uint32_t nwords = g_last_pc_size / 4;
                if (nwords > 16) nwords = 16;
                for (uint32_t w = 0; w < nwords; w++) {
                    float f; memcpy(&f, &u[w], 4);
                    LOG("    PC[%u]: 0x%08x (%.6g)\n", w, u[w], f);
                }
            }
        }
    }

    /* Record for secondary CB replay */
    {
        ReplayCmd* cmd = add_replay_cmd(cmdBuf, RCMD_DRAW_INDEXED);
        if (cmd) {
            cmd->draw_idx.indexCount = indexCount;
            cmd->draw_idx.instanceCount = instanceCount;
            cmd->draw_idx.firstIndex = firstIndex;
            cmd->draw_idx.vertexOffset = vertexOffset;
            cmd->draw_idx.firstInstance = firstInstance;
        }
    }

    real_cmd_draw_indexed(real, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

/* --- CmdDrawIndirect --- (replaces broken ASM trampoline) */
typedef void (*PFN_vkCmdDrawIndirect)(void*, uint64_t, uint64_t, uint32_t, uint32_t);
static PFN_vkCmdDrawIndirect real_cmd_draw_indirect = NULL;

static void trace_CmdDrawIndirect(void* cmdBuf, uint64_t buffer, uint64_t offset,
                                   uint32_t drawCount, uint32_t stride) {
    void* real = unwrap(cmdBuf);
    int op = ++g_cmd_op_count;
    static int di_log = 0;
    di_log++;
    if (di_log <= 200)
        LOG("[CMD#%d] CmdDrawIndirect: cb=%p buf=0x%lx off=%lu count=%u stride=%u\n",
            op, real, (unsigned long)buffer, (unsigned long)offset, drawCount, stride);

    /* === DIAGNOSTIC: Read indirect buffer, VB data, instance data at draw time === */
    /* Which pipeline is currently bound? */
    int di_pipe_bcount = 0;
    for (int p = 0; p < g_pipe_vis_count; p++) {
        if (g_pipe_vis[p].pipeline == g_cur_pipeline) {
            di_pipe_bcount = g_pipe_vis[p].bindingCount;
            break;
        }
    }
    static int di_diag = 0;
    static int di_mesh_diag = 0;
    int do_di_diag = 0;
    if (di_pipe_bcount >= 2) {
        /* 3D mesh pipeline — always capture first 30 */
        do_di_diag = (++di_mesh_diag <= 30);
        if (do_di_diag)
            LOG("  *** MESH-PIPELINE (bCount=%d) pipe=0x%llx ***\n",
                di_pipe_bcount, (unsigned long long)g_cur_pipeline);
    } else if (++di_diag <= 20) {
        do_di_diag = 1;
    }
    if (do_di_diag) {
        /* 1. Indirect buffer readback — VkDrawIndirectCommand values */
        void* ind_ptr = lookup_ubo_ptr(buffer, offset);
        if (ind_ptr) {
            for (uint32_t d = 0; d < drawCount && d < 8; d++) {
                uint32_t* cmd = (uint32_t*)((uint8_t*)ind_ptr + d * stride);
                LOG("  INDIRECT[%u]: vertexCount=%u instanceCount=%u firstVertex=%u firstInstance=%u\n",
                    d, cmd[0], cmd[1], cmd[2], cmd[3]);
            }
        } else {
            LOG("  INDIRECT: FAILED to read (DEVICE_LOCAL?) buf=0x%lx\n", (unsigned long)buffer);
        }

        /* 2. Vertex buffer slot 0 — read first 3 vertex positions */
        if (g_last_vb[0].bound && g_last_vb[0].buffer) {
            uint64_t vb_stride = g_last_vb[0].stride;
            if (vb_stride == 0) vb_stride = 104;
            void* vb_base = lookup_ubo_ptr(g_last_vb[0].buffer, g_last_vb[0].offset);
            if (vb_base) {
                LOG("  VB0: buf=0x%lx off=%lu stride=%lu\n",
                    (unsigned long)g_last_vb[0].buffer, (unsigned long)g_last_vb[0].offset,
                    (unsigned long)vb_stride);
                for (int v = 0; v < 3; v++) {
                    float* pos = (float*)((uint8_t*)vb_base + v * vb_stride);
                    LOG("    vtx[%d] pos: %.3f %.3f %.3f %.3f\n", v, pos[0], pos[1], pos[2], pos[3]);
                    /* Also dump bytes 16-31 (gap in vertex format) */
                    float* gap = (float*)((uint8_t*)vb_base + v * vb_stride + 16);
                    LOG("    vtx[%d] gap16: %.3f %.3f %.3f %.3f\n", v, gap[0], gap[1], gap[2], gap[3]);
                }
            } else {
                LOG("  VB0: DEVICE_LOCAL (unreadable) buf=0x%lx\n", (unsigned long)g_last_vb[0].buffer);
            }
        }

        /* 3. Instance buffer (binding 1) — read transform matrix */
        if (g_last_vb[1].bound && g_last_vb[1].buffer) {
            uint64_t inst_stride = g_last_vb[1].stride;
            if (inst_stride == 0) inst_stride = 64;
            void* inst_base = lookup_ubo_ptr(g_last_vb[1].buffer, g_last_vb[1].offset);
            if (inst_base) {
                LOG("  VB1(instance): buf=0x%lx off=%lu stride=%lu\n",
                    (unsigned long)g_last_vb[1].buffer, (unsigned long)g_last_vb[1].offset,
                    (unsigned long)inst_stride);
                /* Dump first instance's 4x4 matrix (4 vec4 = 64 bytes) */
                float* m = (float*)inst_base;
                LOG("    inst[0] row0: %.4f %.4f %.4f %.4f\n", m[0], m[1], m[2], m[3]);
                LOG("    inst[0] row1: %.4f %.4f %.4f %.4f\n", m[4], m[5], m[6], m[7]);
                LOG("    inst[0] row2: %.4f %.4f %.4f %.4f\n", m[8], m[9], m[10], m[11]);
                LOG("    inst[0] row3: %.4f %.4f %.4f %.4f\n", m[12], m[13], m[14], m[15]);
            } else {
                LOG("  VB1(instance): DEVICE_LOCAL (unreadable) buf=0x%lx\n",
                    (unsigned long)g_last_vb[1].buffer);
            }
        }

        /* 4. Push constant value dump */
        if (g_last_pc_size > 0) {
            const uint32_t* u = (const uint32_t*)g_last_pc_data;
            uint32_t nwords = g_last_pc_size / 4;
            LOG("  PC: stages=0x%x size=%u:", g_last_pc_stages, g_last_pc_size);
            for (uint32_t w = 0; w < nwords && w < 12; w++)
                LOG(" %08x", u[w]);
            LOG("\n");
            /* Also as floats */
            const float* f = (const float*)g_last_pc_data;
            LOG("  PC(float):");
            for (uint32_t w = 0; w < nwords && w < 12; w++)
                LOG(" %.3f", f[w]);
            LOG("\n");
        }

        /* 5. SSBO/UBO readback from last bound descriptor sets */
        if (g_last_ubo_count > 0) {
            LOG("  DESCRIPTORS: %d tracked UBO/SSBO bindings\n", g_last_ubo_count);
            for (int u = 0; u < g_last_ubo_count && u < 4; u++) {
                uint64_t buf = g_last_ubo[u].buffer;
                uint64_t boff = g_last_ubo[u].offset;
                uint64_t range = g_last_ubo[u].range;
                if (buf == 0) continue;
                void* ptr = lookup_ubo_ptr(buf, boff);
                LOG("  UBO[%d]: buf=0x%lx off=%lu range=%lu ptr=%p\n",
                    u, (unsigned long)buf, (unsigned long)boff, (unsigned long)range, ptr);
                if (ptr && range >= 16) {
                    /* Dump first 16 floats (64 bytes) of each UBO/SSBO */
                    const float* f = (const float*)ptr;
                    LOG("    data(float): %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f\n",
                        f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
                    LOG("    data(float): %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f\n",
                        f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
                    /* Also as hex for first 8 words */
                    const uint32_t* h = (const uint32_t*)ptr;
                    LOG("    data(hex): %08x %08x %08x %08x %08x %08x %08x %08x\n",
                        h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]);
                }
            }
        } else {
            LOG("  DESCRIPTORS: no UBO/SSBO tracked\n");
        }
    }

    real_cmd_draw_indirect(real, buffer, offset, drawCount, stride);
}

/* --- CmdDrawIndexedIndirect --- (replaces broken ASM trampoline) */
typedef void (*PFN_vkCmdDrawIndexedIndirect)(void*, uint64_t, uint64_t, uint32_t, uint32_t);
static PFN_vkCmdDrawIndexedIndirect real_cmd_draw_indexed_indirect = NULL;

static void trace_CmdDrawIndexedIndirect(void* cmdBuf, uint64_t buffer, uint64_t offset,
                                          uint32_t drawCount, uint32_t stride) {
    void* real = unwrap(cmdBuf);
    int op = ++g_cmd_op_count;
    static int dii_log = 0;
    if (++dii_log <= 200)
        LOG("[CMD#%d] CmdDrawIndexedIndirect: cb=%p buf=0x%lx off=%lu count=%u stride=%u\n",
            op, real, (unsigned long)buffer, (unsigned long)offset, drawCount, stride);
    real_cmd_draw_indexed_indirect(real, buffer, offset, drawCount, stride);
}

/* --- CmdDispatch --- */
typedef void (*PFN_vkCmdDispatch)(void*, uint32_t, uint32_t, uint32_t);
static PFN_vkCmdDispatch real_cmd_dispatch = NULL;

static void trace_CmdDispatch(void* cmdBuf, uint32_t gx, uint32_t gy, uint32_t gz) {
    void* real = unwrap(cmdBuf);
    int op = ++g_cmd_op_count;
    LOG("[CMD#%d] CmdDispatch: cb=%p groups=%u,%u,%u\n",
        op, real, gx, gy, gz);
    real_cmd_dispatch(real, gx, gy, gz);
}

/* --- CmdFillBuffer --- */
typedef void (*PFN_vkCmdFillBuffer)(void*, uint64_t, uint64_t, uint64_t, uint32_t);
static PFN_vkCmdFillBuffer real_cmd_fill_buffer = NULL;

static void trace_CmdFillBuffer(void* cmdBuf, uint64_t dstBuf, uint64_t dstOffset,
                                  uint64_t size, uint32_t data) {
    void* real = unwrap(cmdBuf);
    int op = ++g_cmd_op_count;
    LOG("[CMD#%d] CmdFillBuffer: cb=%p buf=0x%llx off=%llu size=%llu data=0x%x\n",
        op, real, (unsigned long long)dstBuf, (unsigned long long)dstOffset,
        (unsigned long long)size, data);
    real_cmd_fill_buffer(real, dstBuf, dstOffset, size, data);
}

/* --- CmdUpdateBuffer --- */
typedef void (*PFN_vkCmdUpdateBuffer)(void*, uint64_t, uint64_t, uint64_t, const void*);
static PFN_vkCmdUpdateBuffer real_cmd_update_buffer = NULL;

static void trace_CmdUpdateBuffer(void* cmdBuf, uint64_t dstBuf, uint64_t dstOffset,
                                    uint64_t dataSize, const void* pData) {
    void* real = unwrap(cmdBuf);
    int op = ++g_cmd_op_count;
    LOG("[CMD#%d] CmdUpdateBuffer: cb=%p buf=0x%llx off=%llu size=%llu\n",
        op, real, (unsigned long long)dstBuf, (unsigned long long)dstOffset,
        (unsigned long long)dataSize);
    real_cmd_update_buffer(real, dstBuf, dstOffset, dataSize, pData);
}

/* --- CmdBindDescriptorSets --- */
typedef void (*PFN_vkCmdBindDescSets)(void*, uint32_t, uint64_t, uint32_t, uint32_t,
                                       const uint64_t*, uint32_t, const uint32_t*);
static PFN_vkCmdBindDescSets real_cmd_bind_desc_sets = NULL;

static void trace_CmdBindDescriptorSets(void* cmdBuf, uint32_t bindPoint, uint64_t layout,
                                          uint32_t firstSet, uint32_t setCount,
                                          const uint64_t* pSets, uint32_t dynOffCount,
                                          const uint32_t* pDynOffs) {
    void* real = unwrap(cmdBuf);
    int op = ++g_cmd_op_count;
    static int ds_log_count = 0;
    ds_log_count++;
    if (ds_log_count <= 2000) {
        LOG("[CMD#%d] CmdBindDescriptorSets: cb=%p first=%u count=%u dynOffs=%u layout=0x%lx\n",
            op, real, firstSet, setCount, dynOffCount, (unsigned long)layout);
        for (uint32_t s = 0; s < setCount && s < 4; s++) {
            LOG("  set[%u]: handle=0x%lx\n", firstSet + s, pSets ? (unsigned long)pSets[s] : 0);
        }
        if (dynOffCount > 0 && pDynOffs) {
            for (uint32_t d = 0; d < dynOffCount && d < 4; d++) {
                LOG("  dynOff[%u]=%u\n", d, pDynOffs[d]);
            }
        }
    }
    /* Update g_last_ubo from per-set tracking when a set with UBOs is bound */
    if (pSets) {
        for (uint32_t s = 0; s < setCount; s++) {
            uint64_t sh = pSets[s];
            for (int si = 0; si < g_set_ubo_track_count; si++) {
                if (g_set_ubo_track[si].set_handle == sh && g_set_ubo_track[si].ubo_count > 0) {
                    for (int u = 0; u < g_set_ubo_track[si].ubo_count && u < MAX_LAST_UBO; u++)
                        g_last_ubo[u] = g_set_ubo_track[si].ubos[u];
                    g_last_ubo_count = g_set_ubo_track[si].ubo_count;
                    break;
                }
            }
        }
    }
    /* Record for secondary CB replay */
    {
        ReplayCmd* cmd = add_replay_cmd(cmdBuf, RCMD_BIND_DESC_SETS);
        if (cmd) {
            cmd->desc.bindPoint = bindPoint;
            cmd->desc.layout = layout;
            cmd->desc.firstSet = firstSet;
            cmd->desc.setCount = setCount < 4 ? setCount : 4;
            for (uint32_t s = 0; s < cmd->desc.setCount; s++)
                cmd->desc.sets[s] = pSets ? pSets[s] : 0;
            cmd->desc.dynOffCount = dynOffCount < 8 ? dynOffCount : 8;
            for (uint32_t d = 0; d < cmd->desc.dynOffCount; d++)
                cmd->desc.dynOffs[d] = pDynOffs ? pDynOffs[d] : 0;
        }
    }
    /* FEX thunk 7th-arg fix: CmdBindDescriptorSets has 8 args.
     * Args 7 (dynOffCount) and 8 (pDynOffs) are on the x86-64 stack.
     * FEX thunks may corrupt stack-passed args (same bug as VB2 pStrides).
     * Force dynOffCount=0 when DXVK sends 0, to ensure 0 reaches the driver.
     * If dynOffCount > 0, pass through and log warning. */
    if (dynOffCount == 0) {
        real_cmd_bind_desc_sets(real, bindPoint, layout, firstSet, setCount, pSets, 0, NULL);
    } else {
        static int dyn_warn = 0;
        if (++dyn_warn <= 10)
            LOG("WARNING: CmdBindDescriptorSets dynOffCount=%u (non-zero, may corrupt through thunk!)\n", dynOffCount);
        real_cmd_bind_desc_sets(real, bindPoint, layout, firstSet, setCount, pSets, dynOffCount, pDynOffs);
    }
}

/* --- CmdSetViewport --- */
typedef void (*PFN_vkCmdSetViewport)(void*, uint32_t, uint32_t, const void*);
static PFN_vkCmdSetViewport real_cmd_set_viewport = NULL;

static void trace_CmdSetViewport(void* cmdBuf, uint32_t first, uint32_t count, const void* pViewports) {
    void* real = unwrap(cmdBuf);
    int op = ++g_cmd_op_count;
    /* VkViewport: x(4)+y(4)+width(4)+height(4)+minDepth(4)+maxDepth(4) = 24 bytes */
    if (count > 0 && pViewports) {
        float w = *(const float*)((const char*)pViewports + 8);
        float h = *(const float*)((const char*)pViewports + 12);
        LOG("[CMD#%d] CmdSetViewport: cb=%p count=%u vp0=%.0fx%.0f\n",
            op, real, count, w, h);
    } else {
        LOG("[CMD#%d] CmdSetViewport: cb=%p count=%u\n", op, real, count);
    }
    real_cmd_set_viewport(real, first, count, pViewports);
}

/* --- CmdSetScissor --- */
typedef void (*PFN_vkCmdSetScissor)(void*, uint32_t, uint32_t, const void*);
static PFN_vkCmdSetScissor real_cmd_set_scissor = NULL;

static void trace_CmdSetScissor(void* cmdBuf, uint32_t first, uint32_t count, const void* pScissors) {
    void* real = unwrap(cmdBuf);
    int op = ++g_cmd_op_count;
    LOG("[CMD#%d] CmdSetScissor: cb=%p count=%u\n", op, real, count);
    real_cmd_set_scissor(real, first, count, pScissors);
}

/* --- CmdBindVertexBuffers --- */
typedef void (*PFN_vkCmdBindVtxBufs)(void*, uint32_t, uint32_t, const uint64_t*, const uint64_t*);
static PFN_vkCmdBindVtxBufs real_cmd_bind_vtx_bufs = NULL;

static void trace_CmdBindVertexBuffers(void* cmdBuf, uint32_t first, uint32_t count,
                                        const uint64_t* pBuffers, const uint64_t* pOffsets) {
    void* real = unwrap(cmdBuf);
    int op = ++g_cmd_op_count;
    LOG("[CMD#%d] CmdBindVertexBuffers: cb=%p first=%u count=%u\n",
        op, real, first, count);
    real_cmd_bind_vtx_bufs(real, first, count, pBuffers, pOffsets);
}

/* --- CmdBindVertexBuffers2 (Vulkan 1.3) --- */
typedef void (*PFN_vkCmdBindVtxBufs2)(void*, uint32_t, uint32_t, const uint64_t*,
                                       const uint64_t*, const uint64_t*, const uint64_t*);
static PFN_vkCmdBindVtxBufs2 real_cmd_bind_vtx_bufs2 = NULL;

static void trace_CmdBindVertexBuffers2(void* cmdBuf, uint32_t first, uint32_t count,
                                         const uint64_t* pBuffers, const uint64_t* pOffsets,
                                         const uint64_t* pSizes, const uint64_t* pStrides) {
    void* real = unwrap(cmdBuf);
    int op = ++g_cmd_op_count;
    static int vb2_log_count = 0;
    vb2_log_count++;
    /* Log ALL VB2 calls that aren't stride=24 single-binding (mesh VB2),
     * plus first 500 of the common stride=24 calls */
    int is_unusual = (first != 0) || (count != 1) ||
        (pStrides && pStrides[0] != 24);
    static int unusual_vb2_count = 0;
    if (is_unusual) unusual_vb2_count++;
    if (vb2_log_count <= 500 || (is_unusual && unusual_vb2_count <= 200)) {
        LOG("[CMD#%d] CmdBindVertexBuffers2: cb=%p first=%u count=%u pSizes=%p pStrides=%p%s\n",
            op, real, first, count, (void*)pSizes, (void*)pStrides,
            is_unusual ? " *** MESH-VB ***" : "");
        for (uint32_t i = 0; i < count && i < 4; i++) {
            LOG("  vb[%u]: buf=0x%lx off=%lu size=%lu stride=%lu\n",
                first + i,
                pBuffers ? (unsigned long)pBuffers[i] : 0,
                pOffsets ? (unsigned long)pOffsets[i] : 0,
                pSizes ? (unsigned long)pSizes[i] : 0,
                pStrides ? (unsigned long)pStrides[i] : 0);
        }
    }
    /* Save VB state for vertex readback at draw time */
    for (uint32_t i = 0; i < count && (first + i) < MAX_VB_SLOTS; i++) {
        g_last_vb[first + i].buffer = pBuffers ? pBuffers[i] : 0;
        g_last_vb[first + i].offset = pOffsets ? pOffsets[i] : 0;
        g_last_vb[first + i].size   = pSizes   ? pSizes[i]   : 0xFFFFFFFFFFFFFFFFULL;
        g_last_vb[first + i].stride = pStrides ? pStrides[i] : 0;
        g_last_vb[first + i].bound = 1;
        if (first + i + 1 > g_last_vb_max) g_last_vb_max = first + i + 1;
    }
    /* Record for secondary CB replay */
    {
        ReplayCmd* cmd = add_replay_cmd(cmdBuf, RCMD_BIND_VB);
        if (cmd) {
            cmd->vb.first = first;
            cmd->vb.count = count < 8 ? count : 8;
            for (uint32_t i = 0; i < cmd->vb.count; i++) {
                cmd->vb.buffers[i] = pBuffers ? pBuffers[i] : 0;
                cmd->vb.offsets[i] = pOffsets ? pOffsets[i] : 0;
                cmd->vb.strides[i] = pStrides ? pStrides[i] : 0;
            }
        }
    }
    /* PURE VB2 PASSTHROUGH TEST — pass all 7 args through to Vortek/Mali.
     * If vertices are still exploded, the problem is NOT in VB2/stride handling. */
    real_cmd_bind_vtx_bufs2(real, first, count, pBuffers, pOffsets, pSizes, pStrides);
    {
        static int vb2_pass_log = 0;
        if (++vb2_pass_log <= 20)
            LOG("  -> VB2 PASSTHROUGH (all 7 args, no downconvert)\n");
    }
}

/* --- CmdBindIndexBuffer --- */
typedef void (*PFN_vkCmdBindIdxBuf)(void*, uint64_t, uint64_t, uint32_t);
static PFN_vkCmdBindIdxBuf real_cmd_bind_idx_buf = NULL;

static void trace_CmdBindIndexBuffer(void* cmdBuf, uint64_t buffer, uint64_t offset, uint32_t indexType) {
    void* real = unwrap(cmdBuf);
    int op = ++g_cmd_op_count;
    g_last_ib_buf = buffer;
    g_last_ib_off = offset;
    g_last_ib_type = indexType;
    LOG("[CMD#%d] CmdBindIndexBuffer: cb=%p buf=0x%llx off=%llu type=%u\n",
        op, real, (unsigned long long)buffer, (unsigned long long)offset, indexType);
    /* Record for secondary CB replay */
    {
        ReplayCmd* cmd = add_replay_cmd(cmdBuf, RCMD_BIND_IB);
        if (cmd) { cmd->ib.buffer = buffer; cmd->ib.offset = offset; cmd->ib.indexType = indexType; }
    }
    real_cmd_bind_idx_buf(real, buffer, offset, indexType);
}

/* --- CmdBindIndexBuffer2KHR (maintenance5) --- */
/* DXVK uses this instead of CmdBindIndexBuffer when maintenance5 is spoofed.
 * This function has 5 args: (cmdBuf, buffer, offset, size, indexType).
 * We MUST intercept it because:
 * 1) It goes through make_unwrap_trampoline (raw x86-64 asm) which FEX can't JIT
 * 2) The real driver may not support maintenance5 (we inject the extension)
 * Strategy: try real vkCmdBindIndexBuffer2KHR first; if unavailable, fall back to VB1. */
typedef void (*PFN_vkCmdBindIdxBuf2)(void*, uint64_t, uint64_t, uint64_t, uint32_t);
static PFN_vkCmdBindIdxBuf2 real_cmd_bind_idx_buf2 = NULL;

static void trace_CmdBindIndexBuffer2KHR(void* cmdBuf, uint64_t buffer,
                                          uint64_t offset, uint64_t size,
                                          uint32_t indexType) {
    void* real = unwrap(cmdBuf);
    int op = ++g_cmd_op_count;
    g_last_ib_buf = buffer;
    g_last_ib_off = offset;
    g_last_ib_type = indexType;
    static int ib2_log_count = 0;
    ib2_log_count++;
    if (ib2_log_count <= 50) {
        LOG("[CMD#%d] CmdBindIndexBuffer2KHR: cb=%p buf=0x%llx off=%llu sz=%llu type=%u\n",
            op, real, (unsigned long long)buffer, (unsigned long long)offset,
            (unsigned long long)size, indexType);
    }
    /* Record for secondary CB replay */
    {
        ReplayCmd* cmd = add_replay_cmd(cmdBuf, RCMD_BIND_IB);
        if (cmd) { cmd->ib.buffer = buffer; cmd->ib.offset = offset; cmd->ib.indexType = indexType; }
    }
    if (real_cmd_bind_idx_buf2) {
        /* Real driver supports it — use it */
        real_cmd_bind_idx_buf2(real, buffer, offset, size, indexType);
    } else if (real_cmd_bind_idx_buf) {
        /* Fall back to CmdBindIndexBuffer (drop size param) */
        if (ib2_log_count <= 5)
            LOG("  IB2->IB1 fallback (maintenance5 not real)\n");
        real_cmd_bind_idx_buf(real, buffer, offset, indexType);
    }
}

/* --- VB/IB injection for CmdExecuteCommands ---
 * DXVK may bind VB/IB on a different CB than the primary that executes
 * secondaries. After vkBeginCommandBuffer, state is undefined.
 * This injects the last known VB/IB onto the primary before replay. */
static void inject_vb_ib_state(void* real_primary) {
    static int inject_log = 0;
    if (g_last_vb[0].bound && g_last_vb[0].buffer && real_cmd_bind_vtx_bufs) {
        uint64_t bufs[MAX_VB_SLOTS];
        uint64_t offs[MAX_VB_SLOTS];
        uint32_t n = g_last_vb_max > 0 ? g_last_vb_max : 1;
        if (n > MAX_VB_SLOTS) n = MAX_VB_SLOTS;
        for (uint32_t s = 0; s < n; s++) {
            bufs[s] = g_last_vb[s].buffer;
            offs[s] = g_last_vb[s].offset;
        }
        real_cmd_bind_vtx_bufs(real_primary, 0, n, bufs, offs);
        if (++inject_log <= 20)
            LOG("  INJECT VB1: slots=%u buf=0x%lx off=%lu stride=%lu(baked)\n",
                n, (unsigned long)bufs[0], (unsigned long)offs[0],
                (unsigned long)g_last_vb[0].stride);
    }
    if (g_last_ib_buf) {
        if (real_cmd_bind_idx_buf2)
            real_cmd_bind_idx_buf2(real_primary, g_last_ib_buf, g_last_ib_off,
                                   0xFFFFFFFFFFFFFFFFULL, g_last_ib_type);
        else if (real_cmd_bind_idx_buf)
            real_cmd_bind_idx_buf(real_primary, g_last_ib_buf, g_last_ib_off,
                                  g_last_ib_type);
        if (inject_log <= 20)
            LOG("  INJECT IB: buf=0x%lx off=%lu type=%u\n",
                (unsigned long)g_last_ib_buf, (unsigned long)g_last_ib_off,
                g_last_ib_type);
    }
}

/* --- CmdPushConstants --- */
typedef void (*PFN_vkCmdPushConsts)(void*, uint64_t, uint32_t, uint32_t, uint32_t, const void*);
static PFN_vkCmdPushConsts real_cmd_push_consts = NULL;

static void trace_CmdPushConstants(void* cmdBuf, uint64_t layout, uint32_t stageFlags,
                                    uint32_t offset, uint32_t size, const void* pValues) {
    void* real = unwrap(cmdBuf);
    int op = ++g_cmd_op_count;
    static int pc_log_count = 0;
    pc_log_count++;
    /* Always save push constant data globally for 576-draw diagnostics */
    if (pValues && size > 0 && size <= sizeof(g_last_pc_data)) {
        memcpy(g_last_pc_data, pValues, size);
        g_last_pc_size = size;
        g_last_pc_stages = stageFlags;
    }
    if (pc_log_count <= 100) {
        LOG("[CMD#%d] CmdPushConstants: cb=%p stages=0x%x off=%u size=%u\n",
            op, real, stageFlags, offset, size);
        /* Dump data as uint32/float for diagnostics */
        if (pValues && size >= 4) {
            const uint32_t* u = (const uint32_t*)pValues;
            uint32_t nwords = size / 4;
            if (nwords > 12) nwords = 12;
            LOG("  data:");
            for (uint32_t w = 0; w < nwords; w++) {
                float f;
                memcpy(&f, &u[w], 4);
                LOG(" [%u]=0x%08x(%.4g)", w, u[w], f);
            }
            LOG("\n");
        }
    }
    /* Record for secondary CB replay */
    {
        ReplayCmd* cmd = add_replay_cmd(cmdBuf, RCMD_PUSH_CONSTS);
        if (cmd) {
            cmd->pc.layout = layout;
            cmd->pc.stageFlags = stageFlags;
            cmd->pc.offset = offset;
            cmd->pc.size = size < MAX_REPLAY_DATA ? size : MAX_REPLAY_DATA;
            if (pValues) memcpy(cmd->pc.data, pValues, cmd->pc.size);
        }
    }
    real_cmd_push_consts(real, layout, stageFlags, offset, size, pValues);
}

/* ===== Secondary CB Replay Function =====
 * Replays recorded commands from a secondary CB directly into the primary CB.
 * Called from wrapper_CmdExecuteCommands instead of real_cmd_exec_cmds.
 * This bypasses Vortek's broken secondary CB state inheritance. */
static void replay_secondary_into_primary(void* real_primary, ReplayCB* rcb) {
    for (int c = 0; c < rcb->cmd_count; c++) {
        ReplayCmd* cmd = &rcb->cmds[c];
        switch (cmd->type) {
        case RCMD_BIND_VB:
            if (real_cmd_bind_vtx_bufs)
                real_cmd_bind_vtx_bufs(real_primary, cmd->vb.first, cmd->vb.count,
                                       cmd->vb.buffers, cmd->vb.offsets);
            break;
        case RCMD_BIND_IB:
            if (real_cmd_bind_idx_buf)
                real_cmd_bind_idx_buf(real_primary, cmd->ib.buffer, cmd->ib.offset,
                                      cmd->ib.indexType);
            break;
        case RCMD_BIND_PIPELINE:
            if (real_cmd_bind_pipeline)
                real_cmd_bind_pipeline(real_primary, cmd->pipe.bindPoint, cmd->pipe.pipeline);
            break;
        case RCMD_BIND_DESC_SETS:
            if (real_cmd_bind_desc_sets)
                real_cmd_bind_desc_sets(real_primary, cmd->desc.bindPoint, cmd->desc.layout,
                                        cmd->desc.firstSet, cmd->desc.setCount, cmd->desc.sets,
                                        cmd->desc.dynOffCount, cmd->desc.dynOffs);
            break;
        case RCMD_PUSH_CONSTS:
            if (real_cmd_push_consts)
                real_cmd_push_consts(real_primary, cmd->pc.layout, cmd->pc.stageFlags,
                                     cmd->pc.offset, cmd->pc.size, cmd->pc.data);
            break;
        case RCMD_DRAW_INDEXED:
            if (real_cmd_draw_indexed)
                real_cmd_draw_indexed(real_primary, cmd->draw_idx.indexCount,
                                      cmd->draw_idx.instanceCount, cmd->draw_idx.firstIndex,
                                      cmd->draw_idx.vertexOffset, cmd->draw_idx.firstInstance);
            break;
        case RCMD_DRAW:
            if (real_cmd_draw)
                real_cmd_draw(real_primary, cmd->draw.vertexCount, cmd->draw.instanceCount,
                              cmd->draw.firstVertex, cmd->draw.firstInstance);
            break;
        /* Extended dynamic state replay */
        case RCMD_EDS_UINT:
            if (cmd->eds_uint.slot >= 0 && cmd->eds_uint.slot < MAX_DYN_WRAPPERS && dyn_real[cmd->eds_uint.slot])
                ((void(*)(void*,uint32_t))dyn_real[cmd->eds_uint.slot])(real_primary, cmd->eds_uint.value);
            break;
        case RCMD_EDS_VIEWPORT:
            if (dyn_real[0])
                ((void(*)(void*,uint32_t,const void*))dyn_real[0])(real_primary, cmd->eds_viewport.count, cmd->eds_viewport.data);
            break;
        case RCMD_EDS_SCISSOR:
            if (dyn_real[1])
                ((void(*)(void*,uint32_t,const void*))dyn_real[1])(real_primary, cmd->eds_scissor.count, cmd->eds_scissor.data);
            break;
        case RCMD_EDS_DEPTHBIAS:
            if (dyn_real[2])
                ((void(*)(void*,float,float,float))dyn_real[2])(real_primary, cmd->eds_depthbias.a, cmd->eds_depthbias.b, cmd->eds_depthbias.c);
            break;
        case RCMD_EDS_BLEND:
            if (dyn_real[3])
                ((void(*)(void*,const float*))dyn_real[3])(real_primary, cmd->eds_blend.vals);
            break;
        case RCMD_EDS_STENCIL2:
            if (cmd->eds_stencil2.slot >= 0 && cmd->eds_stencil2.slot < MAX_DYN_WRAPPERS && dyn_real[cmd->eds_stencil2.slot])
                ((void(*)(void*,uint32_t,uint32_t))dyn_real[cmd->eds_stencil2.slot])(real_primary, cmd->eds_stencil2.face, cmd->eds_stencil2.val);
            break;
        case RCMD_EDS_STENCILOP:
            if (dyn_real[12])
                ((void(*)(void*,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t))dyn_real[12])(
                    real_primary, cmd->eds_stencilop.face, cmd->eds_stencilop.fail,
                    cmd->eds_stencilop.pass, cmd->eds_stencilop.dfail, cmd->eds_stencilop.cmp);
            break;
        }
    }
}

/* --- CmdExecuteCommands state logger (uses g_last_vb/ib/ubo defined above) --- */
static void log_exec_cmds(void* real_primary, uint32_t count, void* const* pSecondary) {
    static int exec_log_count = 0;
    exec_log_count++;
    if (exec_log_count <= 500) {
        LOG("CmdExecuteCommands: primary=%p count=%u\n", real_primary, count);
        if (g_last_vb[0].bound) {
            LOG("  INHERIT VB0: buf=0x%lx off=%lu stride=%lu\n",
                (unsigned long)g_last_vb[0].buffer, (unsigned long)g_last_vb[0].offset,
                (unsigned long)g_last_vb[0].stride);
            /* Read back first vertex to see what data the 576-draw will use */
            uint64_t stride = g_last_vb[0].stride ? g_last_vb[0].stride : 24;
            void* vb_ptr = lookup_ubo_ptr(g_last_vb[0].buffer, g_last_vb[0].offset);
            if (vb_ptr) {
                float* pos = (float*)vb_ptr;
                LOG("  INHERIT VB[0] pos=(%.4f, %.4f) VB[1] pos=(%.4f, %.4f)\n",
                    pos[0], pos[1],
                    *(float*)((uint8_t*)vb_ptr + stride), *(float*)((uint8_t*)vb_ptr + stride + 4));
            }
        }
        if (g_last_ib_buf) {
            LOG("  INHERIT IB: buf=0x%lx off=%lu type=%u\n",
                (unsigned long)g_last_ib_buf, (unsigned long)g_last_ib_off, g_last_ib_type);
        }
        if (g_last_ubo_count > 0) {
            for (int u = 0; u < g_last_ubo_count && u < 4; u++) {
                void* ubo_ptr = lookup_ubo_ptr(g_last_ubo[u].buffer, g_last_ubo[u].offset);
                if (ubo_ptr) {
                    float* m = (float*)ubo_ptr;
                    LOG("  INHERIT UBO[%d]: buf=0x%lx off=%lu first4f=[%e %e %e %e]\n",
                        u, (unsigned long)g_last_ubo[u].buffer,
                        (unsigned long)g_last_ubo[u].offset, m[0], m[1], m[2], m[3]);
                }
            }
        }
        for (uint32_t i = 0; i < count; i++) {
            void* real_s = unwrap((void*)pSecondary[i]);
            LOG("  secondary[%u]: wrapped=%p real=%p\n", i, pSecondary[i], real_s);
        }
    }
}
/* Install the logger (called once, e.g. from first CreateGraphicsPipelines or device init) */
__attribute__((constructor)) static void install_exec_cmds_logger(void) {
    g_exec_cmds_log = log_exec_cmds;
    g_inject_vb_ib = inject_vb_ib_state;
}

/* --- vkCreateGraphicsPipelines --- */
typedef VkResult (*PFN_vkCreateGraphicsPipelines)(void*, uint64_t, uint32_t, const void*, const void*, uint64_t*);
static PFN_vkCreateGraphicsPipelines real_create_gfx_pipelines = NULL;

typedef void (*PFN_vkDestroyShaderModule)(void*, uint64_t, const void*);
static PFN_vkDestroyShaderModule real_destroy_shader_module = NULL;

/* Minimal passthrough vertex shader: copies position (location 0) to gl_Position.
 * Used to test if the complex DXVK-generated mesh shader is the cause of
 * "exploded vertices" — if mesh forms proper shapes with this shader,
 * the issue is in the complex shader or Mali's handling of it. */
static const uint32_t passthrough_vs_spv[] = {
    0x07230203, 0x00010000, 0x00070000, 0x0000000d, 0x00000000, 0x00020011,
    0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0007000f, 0x00000000,
    0x00000002, 0x6e69616d, 0x00000000, 0x00000003, 0x00000004, 0x00040047,
    0x00000003, 0x0000000b, 0x00000000, 0x00040047, 0x00000004, 0x0000001e,
    0x00000000, 0x00020013, 0x00000005, 0x00030021, 0x00000006, 0x00000005,
    0x00030016, 0x00000007, 0x00000020, 0x00040017, 0x00000008, 0x00000007,
    0x00000004, 0x00040020, 0x00000009, 0x00000003, 0x00000008, 0x00040020,
    0x0000000a, 0x00000001, 0x00000008, 0x0004003b, 0x00000009, 0x00000003,
    0x00000003, 0x0004003b, 0x0000000a, 0x00000004, 0x00000001, 0x00050036,
    0x00000005, 0x00000002, 0x00000000, 0x00000006, 0x000200f8, 0x0000000b,
    0x0004003d, 0x00000008, 0x0000000c, 0x00000004, 0x0003003e, 0x00000003,
    0x0000000c, 0x000100fd, 0x00010038,
};
static const uint64_t passthrough_vs_size = 300;

/* =========================================================================
 * SPIR-V ClipDistance/CullDistance stripping for Mali GPUs.
 *
 * Mali (via Vortek) doesn't properly handle shaderClipDistance.
 * DXVK generates shaders with OpCapability ClipDistance and
 * OpDecorate %var BuiltIn ClipDistance. If these are present,
 * pipeline compilation produces wrong vertex positions (explosion).
 *
 * Winlator/Vortek 10.0 strips these from SPIR-V. We do the same.
 *
 * SPIR-V instruction format: word0 = (wordcount << 16) | opcode
 * - OpCapability (17): [opcode, capability]  capability 32=ClipDistance, 33=CullDistance
 * - OpDecorate (71): [opcode, target, decoration, ...]  decoration 11=BuiltIn, builtin 3=ClipDistance, 4=CullDistance
 * - OpMemberDecorate (72): [opcode, structType, member, decoration, ...] same builtins
 *
 * We NOP out (replace with OpNop = 0x00010000) the following:
 * 1. OpCapability ClipDistance (0x00020011, 0x00000020)
 * 2. OpCapability CullDistance (0x00020011, 0x00000021)
 * 3. OpDecorate with BuiltIn ClipDistance/CullDistance
 * 4. OpMemberDecorate with BuiltIn ClipDistance/CullDistance
 * ========================================================================= */
static int strip_clip_distance_spirv(uint32_t* code, uint64_t codeSize) {
    uint64_t nwords = codeSize / 4;
    if (nwords < 5 || code[0] != 0x07230203) return 0; /* not valid SPIR-V */
    int changes = 0;
    uint64_t i = 5; /* skip header (5 words) */
    while (i < nwords) {
        uint32_t instr = code[i];
        uint16_t opcode = instr & 0xFFFF;
        uint16_t wcount = instr >> 16;
        if (wcount == 0 || i + wcount > nwords) break; /* malformed */

        if (opcode == 17 && wcount == 2) { /* OpCapability */
            uint32_t cap = code[i+1];
            if (cap == 32 || cap == 33) { /* ClipDistance or CullDistance */
                LOG("  SPIRV-STRIP: OpCapability %s at word %lu\n",
                    cap == 32 ? "ClipDistance" : "CullDistance", (unsigned long)i);
                code[i]   = 0x00010000; /* OpNop */
                code[i+1] = 0x00010000; /* OpNop */
                changes++;
            }
        }
        else if (opcode == 71 && wcount >= 4) { /* OpDecorate */
            uint32_t decoration = code[i+2];
            if (decoration == 11) { /* BuiltIn */
                uint32_t builtin = code[i+3];
                if (builtin == 3 || builtin == 4) { /* ClipDistance or CullDistance */
                    LOG("  SPIRV-STRIP: OpDecorate BuiltIn %s at word %lu\n",
                        builtin == 3 ? "ClipDistance" : "CullDistance", (unsigned long)i);
                    for (uint16_t w = 0; w < wcount; w++)
                        code[i+w] = 0x00010000; /* OpNop */
                    changes++;
                }
            }
        }
        else if (opcode == 72 && wcount >= 5) { /* OpMemberDecorate */
            uint32_t decoration = code[i+3];
            if (decoration == 11) { /* BuiltIn */
                uint32_t builtin = code[i+4];
                if (builtin == 3 || builtin == 4) { /* ClipDistance or CullDistance */
                    LOG("  SPIRV-STRIP: OpMemberDecorate BuiltIn %s at word %lu\n",
                        builtin == 3 ? "ClipDistance" : "CullDistance", (unsigned long)i);
                    for (uint16_t w = 0; w < wcount; w++)
                        code[i+w] = 0x00010000; /* OpNop */
                    changes++;
                }
            }
        }
        i += wcount;
    }
    return changes;
}

/* =========================================================================
 * Mali SPIR-V Fix: OpConstantComposite → OpSpecConstantComposite
 *
 * Mali's compiler incorrectly constant-folds OpConstantComposite (0x2C)
 * when its operands include specialization constants. DXVK 1.7.3+ generates
 * OpConstantComposite with OpSpecConstantTrue/False members. The Mali
 * compiler constant-folds these to False, causing:
 * - Texture samples to return vec4(0) (black textures)
 * - Vertex position calculations to use wrong values (if spec consts in VS)
 *
 * Fix: Replace opcode 0x2C (OpConstantComposite) with 0x33
 * (OpSpecConstantComposite) for composites that reference spec constants.
 *
 * Also adds optimization barriers: replaces OpSelect using spec constants
 * with the non-folded branch to prevent Mali's aggressive optimizer from
 * breaking live code paths.
 *
 * Reference: Vortek/Winlator "MaliCompositeConstantFixPass"
 * https://leegao.github.io/winlator-internals/2025/08/10/OpCompositeConstant.html
 * ========================================================================= */
static int fix_mali_spirv(uint32_t* code, uint64_t codeSize) {
    uint64_t nwords = codeSize / 4;
    if (nwords < 5 || code[0] != 0x07230203) return 0;
    int changes = 0;

    /* Pass 1: Find all spec constant IDs (OpSpecConstantTrue/False/Op/Composite).
     * These are SPIR-V result IDs that hold specialization-derived values. */
    #define MAX_SPEC_IDS 512
    uint32_t spec_ids[MAX_SPEC_IDS];
    int n_spec = 0;

    uint64_t idx = 5;
    while (idx < nwords) {
        uint32_t instr = code[idx];
        uint16_t op = instr & 0xFFFF;
        uint16_t wc = instr >> 16;
        if (wc == 0 || idx + wc > nwords) break;

        /* OpSpecConstantTrue (0x30), OpSpecConstantFalse (0x31),
         * OpSpecConstant (0x32), OpSpecConstantComposite (0x33),
         * OpSpecConstantOp (0x34) — all produce spec-derived result IDs */
        if (op >= 0x30 && op <= 0x34 && wc >= 3) {
            uint32_t result_id = code[idx + 2];
            if (n_spec < MAX_SPEC_IDS)
                spec_ids[n_spec++] = result_id;
        }
        idx += wc;
    }

    if (n_spec == 0) return 0; /* no spec constants → nothing to fix */

    /* Helper: check if an ID is a spec constant */
    #define IS_SPEC(id) ({ \
        int _found = 0; \
        for (int _j = 0; _j < n_spec; _j++) { \
            if (spec_ids[_j] == (id)) { _found = 1; break; } \
        } _found; })

    /* Pass 2: Convert OpConstantComposite → OpSpecConstantComposite
     * when any operand is a spec constant.
     * Also propagate: the result of such a composite becomes a spec ID too. */
    int changed_pass2 = 1;
    int iter = 0;
    while (changed_pass2 && iter < 5) { /* iterate to propagate transitively */
        changed_pass2 = 0;
        iter++;
        idx = 5;
        while (idx < nwords) {
            uint32_t instr = code[idx];
            uint16_t op = instr & 0xFFFF;
            uint16_t wc = instr >> 16;
            if (wc == 0 || idx + wc > nwords) break;

            if (op == 0x2C && wc >= 4) { /* OpConstantComposite: type(1) result(1) constituents(N) */
                uint32_t result_id = code[idx + 2];
                /* Check if any constituent is a spec constant */
                int has_spec = 0;
                for (uint16_t c = 3; c < wc; c++) {
                    if (IS_SPEC(code[idx + c])) { has_spec = 1; break; }
                }
                if (has_spec) {
                    /* Change opcode from 0x2C to 0x33 (preserve word count) */
                    code[idx] = (code[idx] & 0xFFFF0000) | 0x33;
                    changes++;
                    changed_pass2 = 1;
                    /* The result is now a spec constant too */
                    if (n_spec < MAX_SPEC_IDS)
                        spec_ids[n_spec++] = result_id;
                }
            }
            idx += wc;
        }
    }

    #undef IS_SPEC
    #undef MAX_SPEC_IDS
    return changes;
}

/* =========================================================================
 * Mali SPIR-V Fix: Optimization Barrier (OpBitFieldInsert after OpShiftLeftLogical)
 *
 * Mali's compiler aggressively constant-folds OpShiftLeftLogical when the
 * shift amount is a compile-time constant. This produces incorrect results
 * for vertex position / address calculations, causing "exploded vertices" —
 * scattered triangles with wrong positions but correct colors.
 *
 * Fix: After each qualifying OpShiftLeftLogical, insert a no-op barrier:
 *   %temp = OpShiftLeftLogical %type %base %const_shift
 *   %orig = OpBitFieldInsert   %type %temp %zero %uint_0 %uint_0
 *
 * OpBitFieldInsert(base, insert, offset=0, count=0) replaces 0 bits = no-op,
 * but Mali's compiler cannot optimize through the opcode boundary.
 *
 * IMPORTANT: This pass GROWS the SPIR-V buffer (7 extra words per barrier).
 * Returns newly allocated buffer, or NULL if no changes needed.
 * Caller must free() the returned buffer.
 *
 * Reference: Vortek/Winlator "MaliOptimizationBarrierPass"
 * ========================================================================= */
static uint32_t* add_mali_shift_barriers(const uint32_t* code, uint64_t codeSize,
                                          uint64_t* outSize, int* nBarriersOut) {
    uint64_t nwords = codeSize / 4;
    *outSize = codeSize;
    *nBarriersOut = 0;
    if (nwords < 5 || code[0] != 0x07230203) return NULL;

    uint32_t bound = code[3];

    /* ---- Pass 1: Collect constant IDs and find integer types ---- */
    #define BARRIER_MAX_CONST 2048
    uint32_t const_ids[BARRIER_MAX_CONST];
    int n_const = 0;

    uint32_t uint_type = 0;   /* OpTypeInt 32 0 */
    uint32_t int_type  = 0;   /* OpTypeInt 32 1 */
    uint32_t uint_zero = 0;   /* OpConstant %uint 0 */
    uint32_t int_zero  = 0;   /* OpConstant %int  0 */

    uint64_t idx = 5;
    while (idx < nwords) {
        uint32_t instr = code[idx];
        uint16_t op = instr & 0xFFFF;
        uint16_t wc = instr >> 16;
        if (wc == 0 || idx + wc > nwords) break;

        /* OpTypeInt: [4|21] result width signedness */
        if (op == 21 && wc == 4 && code[idx + 2] == 32) {
            if (code[idx + 3] == 0) uint_type = code[idx + 1];
            else if (code[idx + 3] == 1) int_type = code[idx + 1];
        }
        /* OpConstant: [wc|43] type result value... */
        if (op == 43 && wc >= 4) {
            uint32_t tid = code[idx + 1], rid = code[idx + 2];
            if (n_const < BARRIER_MAX_CONST) const_ids[n_const++] = rid;
            if (wc == 4 && code[idx + 3] == 0) {
                if (tid == uint_type && uint_type) uint_zero = rid;
                else if (tid == int_type && int_type) int_zero = rid;
            }
        }
        /* OpSpecConstant: [wc|50] type result value... */
        if (op == 50 && wc >= 4) {
            if (n_const < BARRIER_MAX_CONST) const_ids[n_const++] = code[idx + 2];
        }
        idx += wc;
    }

    /* Helper: is id a constant? */
    #define BARRIER_IS_CONST(id) ({ int _f=0; \
        for (int _i=0; _i<n_const; _i++) { \
            if (const_ids[_i]==(id)) { _f=1; break; } \
        } _f; })

    /* ---- Pass 2: Count qualifying OpShiftLeftLogical ---- */
    int n_barriers = 0;
    int any_int_shift = 0;
    idx = 5;
    while (idx < nwords) {
        uint32_t instr = code[idx];
        uint16_t op = instr & 0xFFFF;
        uint16_t wc = instr >> 16;
        if (wc == 0 || idx + wc > nwords) break;

        /* OpShiftLeftLogical: [5|196] type result base shift */
        if (op == 196 && wc == 5) {
            uint32_t rtype = code[idx + 1];
            uint32_t shift_id = code[idx + 4];
            if (BARRIER_IS_CONST(shift_id) &&
                (rtype == uint_type || rtype == int_type)) {
                n_barriers++;
                if (rtype == int_type) any_int_shift = 1;
            }
        }
        idx += wc;
    }

    if (n_barriers == 0) {
        return NULL;
    }

    /* ---- Determine extra definitions needed ---- */
    int need_uint_type = (!uint_type);
    int need_uint_zero = (!uint_zero);
    int need_int_zero  = (any_int_shift && !int_zero);
    int extra_words = 0;
    if (need_uint_type) extra_words += 4;
    if (need_uint_zero) extra_words += 4;
    if (need_int_zero)  extra_words += 4;

    /* ---- Allocate output buffer ---- */
    uint64_t out_nwords = nwords + (uint64_t)n_barriers * 7 + extra_words;
    uint32_t* out = (uint32_t*)malloc(out_nwords * 4);
    if (!out) {
        return NULL;
    }

    /* Assign new IDs */
    uint32_t next_id = bound;
    if (need_uint_type) uint_type = next_id++;
    if (need_uint_zero) uint_zero = next_id++;
    if (need_int_zero)  int_zero  = next_id++;
    uint32_t temp_base = next_id;
    next_id += n_barriers;

    /* Copy header */
    memcpy(out, code, 5 * 4);
    out[3] = next_id; /* updated bound */

    /* ---- Pass 3: Copy with barrier insertions ---- */
    uint64_t oidx = 5;
    int bidx = 0;
    int defs_done = 0;

    idx = 5;
    while (idx < nwords) {
        uint32_t instr = code[idx];
        uint16_t op = instr & 0xFFFF;
        uint16_t wc = instr >> 16;
        if (wc == 0 || idx + wc > nwords) break;

        /* Insert extra type/constant defs right before first OpFunction */
        if (op == 54 && !defs_done) {
            defs_done = 1;
            if (need_uint_type) {
                out[oidx++] = (4 << 16) | 21; /* OpTypeInt */
                out[oidx++] = uint_type;
                out[oidx++] = 32;
                out[oidx++] = 0;
            }
            if (need_uint_zero) {
                out[oidx++] = (4 << 16) | 43; /* OpConstant */
                out[oidx++] = uint_type;
                out[oidx++] = uint_zero;
                out[oidx++] = 0;
            }
            if (need_int_zero) {
                out[oidx++] = (4 << 16) | 43; /* OpConstant */
                out[oidx++] = int_type;
                out[oidx++] = int_zero;
                out[oidx++] = 0;
            }
        }

        /* Qualifying OpShiftLeftLogical? */
        if (op == 196 && wc == 5) {
            uint32_t rtype     = code[idx + 1];
            uint32_t result_id = code[idx + 2];
            uint32_t shift_id  = code[idx + 4];

            if (BARRIER_IS_CONST(shift_id) &&
                (rtype == uint_type || rtype == int_type)) {
                uint32_t temp_id = temp_base + bidx;

                /* Copy shift with renamed result → temp_id */
                memcpy(out + oidx, code + idx, wc * 4);
                out[oidx + 2] = temp_id;
                oidx += wc;

                /* Insert OpBitFieldInsert(temp, zero, 0, 0) → original ID */
                uint32_t insert_z = (rtype == int_type) ? int_zero : uint_zero;
                out[oidx++] = (7 << 16) | 201; /* OpBitFieldInsert */
                out[oidx++] = rtype;             /* result type     */
                out[oidx++] = result_id;          /* original ID     */
                out[oidx++] = temp_id;            /* base            */
                out[oidx++] = insert_z;           /* insert = 0      */
                out[oidx++] = uint_zero;          /* offset = 0      */
                out[oidx++] = uint_zero;          /* count  = 0      */

                bidx++;
                idx += wc;
                continue;
            }
        }

        /* Default: copy instruction unchanged */
        memcpy(out + oidx, code + idx, wc * 4);
        oidx += wc;
        idx += wc;
    }

    *outSize = oidx * 4;
    *nBarriersOut = bidx;

    #undef BARRIER_IS_CONST
    #undef BARRIER_MAX_CONST
    return out;
}

/* =========================================================================
 * USCALED/SSCALED Vertex Format Emulation for Mali GPU
 *
 * Mali GPUs lack native support for VK_FORMAT_*_USCALED and VK_FORMAT_*_SSCALED
 * vertex buffer formats. When DXVK uses these formats, Mali misinterprets the
 * vertex data, causing "exploded vertices" (scattered triangles).
 *
 * Fix: Remap USCALED→UINT and SSCALED→SINT in the pipeline VIS, then rewrite
 * the vertex shader SPIR-V to insert OpConvertUToF/OpConvertSToF after loading
 * the affected input variables.
 *
 * Reference: Vortek/Winlator "ScaledFormatEmulationPass"
 * https://leegao.github.io/winlator-internals/2025/06/02/Vortek2.html
 * ========================================================================= */

#define MAX_SCALED_ATTRS 16
typedef struct {
    uint32_t location;
    int components;   /* 1-4 */
    int is_signed;    /* 0=USCALED→UINT, 1=SSCALED→SINT */
} ScaledAttrRemap;

typedef struct {
    int count;
    ScaledAttrRemap attrs[MAX_SCALED_ATTRS];
} ScaledRemapInfo;

/* Returns the UINT/SINT equivalent format for a USCALED/SSCALED format.
 * Returns 0 if the format is not a scaled format.
 * Sets *is_signed (0=unsigned, 1=signed) and *components (1-4) via out params. */
static uint32_t remap_scaled_format(uint32_t fmt, int* is_signed, int* components) {
    switch (fmt) {
        /* R8_USCALED(11)→R8_UINT(13), R8_SSCALED(12)→R8_SINT(14) */
        case 11: *is_signed = 0; *components = 1; return 13;
        case 12: *is_signed = 1; *components = 1; return 14;
        /* R8G8_USCALED(18)→R8G8_UINT(20), R8G8_SSCALED(19)→R8G8_SINT(21) */
        case 18: *is_signed = 0; *components = 2; return 20;
        case 19: *is_signed = 1; *components = 2; return 21;
        /* R8G8B8_USCALED(25)→R8G8B8_UINT(27), R8G8B8_SSCALED(26)→R8G8B8_SINT(28) */
        case 25: *is_signed = 0; *components = 3; return 27;
        case 26: *is_signed = 1; *components = 3; return 28;
        /* B8G8R8_USCALED(32)→B8G8R8_UINT(34), B8G8R8_SSCALED(33)→B8G8R8_SINT(35) */
        case 32: *is_signed = 0; *components = 3; return 34;
        case 33: *is_signed = 1; *components = 3; return 35;
        /* R8G8B8A8_USCALED(39)→R8G8B8A8_UINT(41), R8G8B8A8_SSCALED(40)→R8G8B8A8_SINT(42) */
        case 39: *is_signed = 0; *components = 4; return 41;
        case 40: *is_signed = 1; *components = 4; return 42;
        /* B8G8R8A8_USCALED(46)→B8G8R8A8_UINT(48), B8G8R8A8_SSCALED(47)→B8G8R8A8_SINT(49) */
        case 46: *is_signed = 0; *components = 4; return 48;
        case 47: *is_signed = 1; *components = 4; return 49;
        /* A8B8G8R8_USCALED(53)→A8B8G8R8_UINT(55), A8B8G8R8_SSCALED(54)→A8B8G8R8_SINT(56) */
        case 53: *is_signed = 0; *components = 4; return 55;
        case 54: *is_signed = 1; *components = 4; return 56;
        /* A2R10G10B10_USCALED(60)→A2R10G10B10_UINT(62), A2R10G10B10_SSCALED(61)→A2R10G10B10_SINT(63) */
        case 60: *is_signed = 0; *components = 4; return 62;
        case 61: *is_signed = 1; *components = 4; return 63;
        /* A2B10G10R10_USCALED(67)→A2B10G10R10_UINT(69), A2B10G10R10_SSCALED(68)→A2B10G10R10_SINT(70) */
        case 67: *is_signed = 0; *components = 4; return 69;
        case 68: *is_signed = 1; *components = 4; return 70;
        /* R16_USCALED(72)→R16_UINT(74), R16_SSCALED(73)→R16_SINT(75) */
        case 72: *is_signed = 0; *components = 1; return 74;
        case 73: *is_signed = 1; *components = 1; return 75;
        /* R16G16_USCALED(79)→R16G16_UINT(81), R16G16_SSCALED(80)→R16G16_SINT(82) */
        case 79: *is_signed = 0; *components = 2; return 81;
        case 80: *is_signed = 1; *components = 2; return 82;
        /* R16G16B16_USCALED(86)→R16G16B16_UINT(88), R16G16B16_SSCALED(87)→R16G16B16_SINT(89) */
        case 86: *is_signed = 0; *components = 3; return 88;
        case 87: *is_signed = 1; *components = 3; return 89;
        /* R16G16B16A16_USCALED(93)→R16G16B16A16_UINT(95), R16G16B16A16_SSCALED(94)→R16G16B16A16_SINT(96) */
        case 93: *is_signed = 0; *components = 4; return 95;
        case 94: *is_signed = 1; *components = 4; return 96;
        default: return 0; /* not a scaled format */
    }
}

/* =========================================================================
 * SPIR-V Rewriter: Emulate USCALED/SSCALED by converting Input types
 *
 * For each remapped vertex attribute:
 *   1. Change the Input variable's pointer type from float→int
 *   2. After each OpLoad from that variable, insert OpConvertUToF/OpConvertSToF
 *
 * Returns newly allocated SPIR-V buffer, or NULL if no changes needed.
 * Caller must free() the returned buffer.
 * ========================================================================= */
static uint32_t* emulate_scaled_formats_spirv(const uint32_t* code, uint64_t codeSize,
                                               const ScaledRemapInfo* remap,
                                               uint64_t* outSize) {
    uint64_t nwords = codeSize / 4;
    *outSize = codeSize;
    if (nwords < 5 || code[0] != 0x07230203 || remap->count == 0) return NULL;

    uint32_t bound = code[3];
    uint32_t next_id = bound;

    /* ---- Pass 1: Scan types and variables ---- */
    #define SCALED_MAX_TYPES 128
    #define SCALED_MAX_VARS  64

    uint32_t float32_type = 0;     /* OpTypeFloat 32 */
    uint32_t uint32_type = 0;      /* OpTypeInt 32 0 */
    uint32_t sint32_type = 0;      /* OpTypeInt 32 1 */

    /* Track float vector types: fvec_type[N] = type ID for vec<float,N> (1=scalar) */
    uint32_t fvec_type[5] = {0};   /* index 1-4 */

    /* Track int vector types */
    uint32_t uvec_type[5] = {0};   /* vec<uint,N> */
    uint32_t svec_type[5] = {0};   /* vec<sint,N> */

    /* Track pointer types: map type_id → base_type_id for Input storage class */
    struct { uint32_t ptr_id; uint32_t base_id; } ptr_types[SCALED_MAX_TYPES];
    int n_ptr_types = 0;

    /* Track Input variables: var_id, pointer_type_id */
    struct { uint32_t var_id; uint32_t ptr_type_id; } input_vars[SCALED_MAX_VARS];
    int n_input_vars = 0;

    /* Track Location decorations: var_id → location */
    struct { uint32_t var_id; uint32_t location; } locations[SCALED_MAX_VARS];
    int n_locations = 0;

    uint64_t idx = 5;
    while (idx < nwords) {
        uint32_t instr = code[idx];
        uint16_t op = instr & 0xFFFF;
        uint16_t wc = instr >> 16;
        if (wc == 0 || idx + wc > nwords) break;

        switch (op) {
            case 22: /* OpTypeFloat: [3|22] result width */
                if (wc == 3 && code[idx + 2] == 32)
                    float32_type = code[idx + 1];
                break;
            case 21: /* OpTypeInt: [4|21] result width signedness */
                if (wc == 4 && code[idx + 2] == 32) {
                    if (code[idx + 3] == 0) uint32_type = code[idx + 1];
                    else sint32_type = code[idx + 1];
                }
                break;
            case 23: /* OpTypeVector: [4|23] result component_type count */
                if (wc == 4) {
                    uint32_t comp_type = code[idx + 2];
                    uint32_t count = code[idx + 3];
                    if (count >= 1 && count <= 4) {
                        if (comp_type == float32_type && float32_type)
                            fvec_type[count] = code[idx + 1];
                        /* We'll match int vectors after we know uint/sint type IDs */
                    }
                }
                break;
            case 32: /* OpTypePointer: [4|32] result storage_class base_type */
                if (wc == 4) {
                    uint32_t sc = code[idx + 2];
                    if (sc == 1 && n_ptr_types < SCALED_MAX_TYPES) { /* Input */
                        ptr_types[n_ptr_types].ptr_id = code[idx + 1];
                        ptr_types[n_ptr_types].base_id = code[idx + 3];
                        n_ptr_types++;
                    }
                }
                break;
            case 59: /* OpVariable: [4|59] result_type result storage_class */
                if (wc >= 4 && code[idx + 3] == 1 && n_input_vars < SCALED_MAX_VARS) { /* Input */
                    input_vars[n_input_vars].var_id = code[idx + 2];
                    input_vars[n_input_vars].ptr_type_id = code[idx + 1];
                    n_input_vars++;
                }
                break;
            case 71: /* OpDecorate: [wc|71] target decoration [operands...] */
                if (wc >= 4 && code[idx + 2] == 30 && n_locations < SCALED_MAX_VARS) { /* Location */
                    locations[n_locations].var_id = code[idx + 1];
                    locations[n_locations].location = code[idx + 3];
                    n_locations++;
                }
                break;
        }
        idx += wc;
    }

    /* Also scan for existing int vector types */
    idx = 5;
    while (idx < nwords) {
        uint32_t instr = code[idx];
        uint16_t op = instr & 0xFFFF;
        uint16_t wc = instr >> 16;
        if (wc == 0 || idx + wc > nwords) break;
        if (op == 23 && wc == 4) { /* OpTypeVector */
            uint32_t comp_type = code[idx + 2];
            uint32_t count = code[idx + 3];
            if (count >= 1 && count <= 4) {
                if (comp_type == uint32_type && uint32_type)
                    uvec_type[count] = code[idx + 1];
                if (comp_type == sint32_type && sint32_type)
                    svec_type[count] = code[idx + 1];
            }
        }
        idx += wc;
    }

    if (!float32_type) {
        LOG("  SCALED-SPIRV: no OpTypeFloat 32 found, skipping\n");
        return NULL;
    }

    /* ---- Pass 2: Match variables to remap targets ---- */
    struct {
        uint32_t var_id;
        uint32_t float_type;    /* current float/vec type */
        uint32_t int_type;      /* target int/ivec type (may need creation) */
        uint32_t int_ptr_type;  /* target pointer type (may need creation) */
        int components;
        int is_signed;
        int need_int_type;      /* 1 if we need to create int vector type */
        int need_ptr_type;      /* 1 if we need to create pointer type */
    } remap_vars[MAX_SCALED_ATTRS];
    int n_remap_vars = 0;

    for (int r = 0; r < remap->count; r++) {
        uint32_t target_loc = remap->attrs[r].location;
        int comps = remap->attrs[r].components;
        int is_signed = remap->attrs[r].is_signed;

        /* Find variable at this location */
        uint32_t var_id = 0;
        uint32_t var_ptr_type = 0;
        for (int v = 0; v < n_input_vars; v++) {
            for (int l = 0; l < n_locations; l++) {
                if (locations[l].var_id == input_vars[v].var_id &&
                    locations[l].location == target_loc) {
                    var_id = input_vars[v].var_id;
                    var_ptr_type = input_vars[v].ptr_type_id;
                    break;
                }
            }
            if (var_id) break;
        }
        if (!var_id) {
            LOG("  SCALED-SPIRV: no Input variable at location %u, skipping\n", target_loc);
            continue;
        }

        /* Find the base float type through the pointer */
        uint32_t base_float = 0;
        for (int p = 0; p < n_ptr_types; p++) {
            if (ptr_types[p].ptr_id == var_ptr_type) {
                base_float = ptr_types[p].base_id;
                break;
            }
        }

        /* Determine the integer type we need */
        uint32_t* ivec_arr = is_signed ? svec_type : uvec_type;
        uint32_t int_base = is_signed ? sint32_type : uint32_type;
        uint32_t target_int_type = ivec_arr[comps];
        int need_int_type = 0;
        int need_ptr_type = 1;

        if (!int_base) need_int_type = 1; /* need to create OpTypeInt */
        if (comps > 1 && !target_int_type) need_int_type = 1; /* need to create vector type */
        if (comps == 1 && int_base) target_int_type = int_base;

        /* Check if pointer type already exists */
        uint32_t target_ptr_type = 0;
        if (target_int_type) {
            for (int p = 0; p < n_ptr_types; p++) {
                if (ptr_types[p].base_id == target_int_type) {
                    target_ptr_type = ptr_types[p].ptr_id;
                    need_ptr_type = 0;
                    break;
                }
            }
        }

        remap_vars[n_remap_vars].var_id = var_id;
        remap_vars[n_remap_vars].float_type = base_float;
        remap_vars[n_remap_vars].int_type = target_int_type;
        remap_vars[n_remap_vars].int_ptr_type = target_ptr_type;
        remap_vars[n_remap_vars].components = comps;
        remap_vars[n_remap_vars].is_signed = is_signed;
        remap_vars[n_remap_vars].need_int_type = need_int_type;
        remap_vars[n_remap_vars].need_ptr_type = need_ptr_type;
        n_remap_vars++;

        LOG("  SCALED-SPIRV: loc=%u var=%%%u float_type=%%%u comps=%d %s need_create=%d,%d\n",
            target_loc, var_id, base_float, comps,
            is_signed ? "SINT" : "UINT",
            need_int_type, need_ptr_type);
    }

    if (n_remap_vars == 0) return NULL;

    /* ---- Pass 3: Count OpLoads from remapped variables ---- */
    int n_loads = 0;
    idx = 5;
    while (idx < nwords) {
        uint32_t instr = code[idx];
        uint16_t op = instr & 0xFFFF;
        uint16_t wc = instr >> 16;
        if (wc == 0 || idx + wc > nwords) break;
        if (op == 61 && wc >= 4) { /* OpLoad: [4|61] result_type result pointer */
            uint32_t ptr_id = code[idx + 3];
            for (int v = 0; v < n_remap_vars; v++) {
                if (remap_vars[v].var_id == ptr_id) {
                    n_loads++;
                    break;
                }
            }
        }
        idx += wc;
    }

    LOG("  SCALED-SPIRV: %d vars to remap, %d loads to patch\n", n_remap_vars, n_loads);
    if (n_loads == 0) return NULL;

    /* ---- Allocate new type IDs ---- */
    int new_type_words = 0;
    for (int v = 0; v < n_remap_vars; v++) {
        if (remap_vars[v].need_int_type) {
            uint32_t int_base = remap_vars[v].is_signed ? sint32_type : uint32_type;
            if (!int_base) {
                /* Need to create OpTypeInt 32 0/1: 4 words */
                uint32_t new_int = next_id++;
                if (remap_vars[v].is_signed) sint32_type = new_int;
                else uint32_type = new_int;
                int_base = new_int;
                new_type_words += 4;
            }
            if (remap_vars[v].components > 1) {
                /* Need to create OpTypeVector: 4 words */
                uint32_t new_vec = next_id++;
                if (remap_vars[v].is_signed) svec_type[remap_vars[v].components] = new_vec;
                else uvec_type[remap_vars[v].components] = new_vec;
                remap_vars[v].int_type = new_vec;
                new_type_words += 4;
            } else {
                remap_vars[v].int_type = int_base;
            }
        }
        /* Re-check: the int_type may have been set by a previous var with same component count */
        if (!remap_vars[v].int_type) {
            uint32_t* ivec_arr2 = remap_vars[v].is_signed ? svec_type : uvec_type;
            uint32_t int_base2 = remap_vars[v].is_signed ? sint32_type : uint32_type;
            remap_vars[v].int_type = (remap_vars[v].components > 1) ?
                                      ivec_arr2[remap_vars[v].components] : int_base2;
        }
        if (remap_vars[v].need_ptr_type && remap_vars[v].int_type) {
            /* Need to create OpTypePointer Input: 4 words */
            uint32_t new_ptr = next_id++;
            remap_vars[v].int_ptr_type = new_ptr;
            new_type_words += 4;
            /* Other vars with same int_type can reuse this pointer */
            for (int v2 = v + 1; v2 < n_remap_vars; v2++) {
                if (remap_vars[v2].int_type == remap_vars[v].int_type &&
                    remap_vars[v2].need_ptr_type) {
                    remap_vars[v2].int_ptr_type = new_ptr;
                    remap_vars[v2].need_ptr_type = 0; /* don't create duplicate */
                }
            }
        }
    }

    /* ---- Pass 4: Allocate output and copy with modifications ---- */
    uint64_t out_nwords = nwords + new_type_words + n_loads * 4;
    uint32_t* out = (uint32_t*)malloc(out_nwords * 4);
    if (!out) return NULL;

    uint64_t oidx = 0;
    /* Copy header */
    memcpy(out, code, 5 * 4);
    out[3] = next_id; /* update bound */
    oidx = 5;

    int types_inserted = 0;
    idx = 5;
    while (idx < nwords) {
        uint32_t instr = code[idx];
        uint16_t op = instr & 0xFFFF;
        uint16_t wc = instr >> 16;
        if (wc == 0 || idx + wc > nwords) break;

        /* Insert new types just before the first OpFunction */
        if (op == 54 && !types_inserted) { /* OpFunction */
            types_inserted = 1;
            /* Emit any new OpTypeInt */
            if (uint32_type >= bound) { /* was freshly allocated */
                out[oidx++] = (4 << 16) | 21; /* OpTypeInt */
                out[oidx++] = uint32_type;
                out[oidx++] = 32;
                out[oidx++] = 0; /* unsigned */
            }
            if (sint32_type >= bound) { /* was freshly allocated */
                out[oidx++] = (4 << 16) | 21; /* OpTypeInt */
                out[oidx++] = sint32_type;
                out[oidx++] = 32;
                out[oidx++] = 1; /* signed */
            }
            /* Emit any new OpTypeVector */
            for (int c = 2; c <= 4; c++) {
                if (uvec_type[c] >= bound) {
                    out[oidx++] = (4 << 16) | 23; /* OpTypeVector */
                    out[oidx++] = uvec_type[c];
                    out[oidx++] = uint32_type;
                    out[oidx++] = c;
                }
                if (svec_type[c] >= bound) {
                    out[oidx++] = (4 << 16) | 23; /* OpTypeVector */
                    out[oidx++] = svec_type[c];
                    out[oidx++] = sint32_type;
                    out[oidx++] = c;
                }
            }
            /* Emit any new OpTypePointer Input */
            for (int v = 0; v < n_remap_vars; v++) {
                if (remap_vars[v].need_ptr_type && remap_vars[v].int_ptr_type >= bound) {
                    out[oidx++] = (4 << 16) | 32; /* OpTypePointer */
                    out[oidx++] = remap_vars[v].int_ptr_type;
                    out[oidx++] = 1; /* Input */
                    out[oidx++] = remap_vars[v].int_type;
                    /* Mark as emitted so we don't emit duplicates */
                    uint32_t emitted_ptr = remap_vars[v].int_ptr_type;
                    for (int v2 = v + 1; v2 < n_remap_vars; v2++) {
                        if (remap_vars[v2].int_ptr_type == emitted_ptr)
                            remap_vars[v2].need_ptr_type = 0;
                    }
                    remap_vars[v].need_ptr_type = 0;
                }
            }
        }

        /* Check if this is an OpVariable that needs its type changed */
        int var_match = -1;
        if (op == 59 && wc >= 4 && code[idx + 3] == 1) { /* OpVariable Input */
            uint32_t vid = code[idx + 2];
            for (int v = 0; v < n_remap_vars; v++) {
                if (remap_vars[v].var_id == vid) { var_match = v; break; }
            }
        }

        /* Check if this is an OpLoad from a remapped variable */
        int load_match = -1;
        if (op == 61 && wc >= 4) { /* OpLoad */
            uint32_t ptr_id = code[idx + 3];
            for (int v = 0; v < n_remap_vars; v++) {
                if (remap_vars[v].var_id == ptr_id) { load_match = v; break; }
            }
        }

        if (var_match >= 0 && remap_vars[var_match].int_ptr_type) {
            /* Change result type from float pointer to int pointer */
            memcpy(out + oidx, code + idx, wc * 4);
            out[oidx + 1] = remap_vars[var_match].int_ptr_type;
            oidx += wc;
        } else if (load_match >= 0 && remap_vars[load_match].int_type) {
            /* Rewrite the OpLoad: change result type to int, rename result ID */
            uint32_t orig_result_id = code[idx + 2];
            uint32_t temp_id = next_id++;
            /* Update bound in header */
            out[3] = next_id;

            /* Emit modified OpLoad: result type = int, result = temp_id */
            memcpy(out + oidx, code + idx, wc * 4);
            out[oidx + 1] = remap_vars[load_match].int_type;
            out[oidx + 2] = temp_id;
            oidx += wc;

            /* Insert OpConvertUToF(112) or OpConvertSToF(111):
             * [4|op] result_type result operand */
            uint32_t conv_op = remap_vars[load_match].is_signed ? 111 : 112;
            uint32_t float_type = remap_vars[load_match].float_type;
            /* For scalar float (components==1), use float32_type directly */
            if (remap_vars[load_match].components == 1 && !float_type)
                float_type = float32_type;
            /* For vectors, use the fvec_type */
            if (!float_type && remap_vars[load_match].components > 1)
                float_type = fvec_type[remap_vars[load_match].components];
            if (!float_type) float_type = float32_type; /* fallback */

            out[oidx++] = (4 << 16) | conv_op;
            out[oidx++] = float_type;
            out[oidx++] = orig_result_id;
            out[oidx++] = temp_id;
        } else {
            /* Copy instruction unchanged */
            memcpy(out + oidx, code + idx, wc * 4);
            oidx += wc;
        }
        idx += wc;
    }

    *outSize = oidx * 4;
    LOG("  SCALED-SPIRV: rewrote %d loads, %lu→%lu bytes (new types: %d words)\n",
        n_loads, (unsigned long)codeSize, (unsigned long)(oidx * 4), new_type_words);

    #undef SCALED_MAX_TYPES
    #undef SCALED_MAX_VARS
    return out;
}

/* Convert inline VkShaderModuleCreateInfo (maintenance5) to real VkShaderModule.
 * Vortek's IPC can't serialize pNext chains on shader stages, so we pre-create
 * the modules and patch the stage to use them.
 * Returns number of temp modules created; caller must destroy them after pipeline creation.
 */
#define MAX_TEMP_MODULES 32
static uint32_t fixup_inline_shaders(void* real_device, const void* pCreateInfos,
                                      uint32_t pipe_count, uint64_t* temp_modules,
                                      const ScaledRemapInfo* remap_info) {
    uint32_t n_temp = 0;
    typedef struct { uint32_t sType; uint32_t _pad; void* pNext; } PNBase;

    for (uint32_t i = 0; i < pipe_count; i++) {
        uint8_t* ci = (uint8_t*)pCreateInfos + i * 144;
        uint32_t stageCount = *(uint32_t*)(ci + 20);
        uint8_t* pStages = *(uint8_t**)(ci + 24);
        if (!pStages) continue;

        for (uint32_t s = 0; s < stageCount && s < 6; s++) {
            uint8_t* stage = pStages + s * 48;
            uint64_t* pModule = (uint64_t*)(stage + 24);

            if (*pModule != 0) continue; /* already has a VkShaderModule */

            /* Walk pNext for VkShaderModuleCreateInfo (sType=16)
             * Layout: sType(4)+pad(4)+pNext(8)+flags(4)+pad(4)+codeSize(8)+pCode(8) = 40 bytes */
            PNBase* pn = (PNBase*)(*(void**)(stage + 8));
            while (pn) {
                if (pn->sType == 16) { /* VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO */
                    if (n_temp >= MAX_TEMP_MODULES) {
                        LOG("  WARNING: too many inline shaders (%u)\n", n_temp);
                        break;
                    }
                    /* Create real VkShaderModule from inline data */
                    uint64_t new_module = 0;
                    uint64_t codeSize = *(uint64_t*)((uint8_t*)pn + 24);
                    const uint32_t* pCode = *(const uint32_t**)((uint8_t*)pn + 32);
                    uint32_t stageBit = *(uint32_t*)(stage + 20);
                    /* SPIR-V integrity check before module creation */
                    if (pCode && codeSize >= 20) {
                        uint32_t magic = pCode[0];
                        uint32_t version = pCode[1];
                        uint32_t generator = pCode[2];
                        uint32_t bound = pCode[3];
                        /* XOR checksum of all SPIR-V words */
                        uint32_t cksum = 0;
                        uint64_t nwords_spv = codeSize / 4;
                        for (uint64_t w = 0; w < nwords_spv; w++)
                            cksum ^= pCode[w];
                        /* Last 2 words for truncation detection */
                        uint32_t last0 = nwords_spv > 1 ? pCode[nwords_spv-2] : 0;
                        uint32_t last1 = nwords_spv > 0 ? pCode[nwords_spv-1] : 0;
                        LOG("  SPIRV-CHECK: stage[%u] magic=0x%08x ver=0x%x gen=0x%x bound=%u words=%lu cksum=0x%08x last=[0x%08x,0x%08x]\n",
                            s, magic, version, generator, bound, (unsigned long)nwords_spv,
                            cksum, last0, last1);
                        /* Dump SPIR-V to file for offline analysis */
                        {
                            static int spv_dump_count = 0;
                            if (spv_dump_count < 30) {
                                char spv_fname[128];
                                snprintf(spv_fname, sizeof(spv_fname),
                                         "/tmp/shader_%03d_w%lu_s%x.spv",
                                         spv_dump_count, (unsigned long)nwords_spv, stageBit);
                                FILE* spv_f = fopen(spv_fname, "wb");
                                if (spv_f) {
                                    fwrite(pCode, 1, codeSize, spv_f);
                                    fclose(spv_f);
                                    LOG("  SHADER-DUMP: %s (%lu bytes)\n", spv_fname, (unsigned long)codeSize);
                                }
                                spv_dump_count++;
                            }
                        }
                    }
                    /* SPIR-V instruction census for large shaders
                     * Opcodes: FMul=133 FAdd=129 IAdd=128 Dot=148
                     * MxV=145 VxM=144 MxM=146 ExtInst=12
                     * SHL=196 SHR_L=194 SHR_A=195 BFI=201 */
                    if (pCode && codeSize > 8000) {
                        uint64_t nw = codeSize / 4;
                        int n_shl=0, n_shr_l=0, n_shr_a=0, n_dot=0;
                        int n_mxv=0, n_vxm=0, n_mxm=0, n_fmul=0, n_fadd=0;
                        int n_bfi=0, n_iadd=0, n_ext=0, n_load=0;
                        uint64_t ci3 = 5;
                        while (ci3 < nw) {
                            uint32_t w = pCode[ci3];
                            uint16_t opc = w & 0xFFFF;
                            uint16_t wc3 = w >> 16;
                            if (wc3 == 0 || ci3 + wc3 > nw) break;
                            switch (opc) {
                                case 194: n_shr_l++; break;
                                case 195: n_shr_a++; break;
                                case 196: n_shl++; break;
                                case 148: n_dot++; break;
                                case 145: n_mxv++; break;
                                case 144: n_vxm++; break;
                                case 146: n_mxm++; break;
                                case 133: n_fmul++; break;
                                case 129: n_fadd++; break;
                                case 128: n_iadd++; break;
                                case 201: n_bfi++; break;
                                case 12: n_ext++; break;
                                case 61: n_load++; break;
                            }
                            ci3 += wc3;
                        }
                        LOG("  SPIRV-CENSUS: stage[%u] bit=0x%x %luB: "
                            "FMul=%d FAdd=%d Dot=%d MxV=%d VxM=%d MxM=%d "
                            "SHL=%d SHR_L=%d SHR_A=%d IAdd=%d BFI=%d Ext=%d Load=%d\n",
                            s, stageBit, (unsigned long)codeSize,
                            n_fmul, n_fadd, n_dot, n_mxv, n_vxm, n_mxm,
                            n_shl, n_shr_l, n_shr_a, n_iadd, n_bfi, n_ext, n_load);
                    }
                    /* ==== Apply ALL Mali SPIR-V fixes ==== */
                    uint32_t* mutableCode = NULL;
                    uint64_t effectiveSize = codeSize;
                    uint8_t patched_ci[40];
                    const void* moduleCI = (const void*)pn;
                    if (pCode && codeSize > 20) {
                        mutableCode = (uint32_t*)malloc(codeSize);
                        if (mutableCode) {
                            memcpy(mutableCode, pCode, codeSize);
                            int total_fixes = 0;
                            /* ==== Mali SPIR-V Fixes (re-enabled) ==== */
                            /* Fix 1: Strip ClipDistance/CullDistance (Mali unsupported) */
                            {
                                int n_strip = strip_clip_distance_spirv(mutableCode, codeSize);
                                if (n_strip > 0) {
                                    total_fixes += n_strip;
                                    LOG("  MALI-FIX: stage[%u] stripped %d ClipDistance/CullDistance\n", s, n_strip);
                                }
                            }
                            /* Fix 2: OpConstantComposite→OpSpecConstantComposite */
                            {
                                int n_composite = fix_mali_spirv(mutableCode, effectiveSize);
                                if (n_composite > 0) {
                                    total_fixes += n_composite;
                                    LOG("  MALI-FIX: stage[%u] fixed %d ConstantComposite→Spec\n", s, n_composite);
                                }
                            }
                            /* Fix 3: Shift optimization barriers */
                            {
                                uint64_t barrierSize = 0;
                                int nBarriers = 0;
                                uint32_t* barrierCode = add_mali_shift_barriers(
                                    mutableCode, effectiveSize, &barrierSize, &nBarriers);
                                if (barrierCode) {
                                    free(mutableCode);
                                    mutableCode = barrierCode;
                                    effectiveSize = barrierSize;
                                    total_fixes += nBarriers;
                                    LOG("  MALI-FIX: stage[%u] added %d shift barriers (%lu→%lu bytes)\n",
                                        s, nBarriers, (unsigned long)codeSize, (unsigned long)barrierSize);
                                }
                            }
                            /* Fix 5: USCALED/SSCALED format emulation (vertex shaders only) */
                            if (stageBit == 1 && remap_info && remap_info[i].count > 0) {
                                uint64_t scaledSize = 0;
                                uint32_t* scaledCode = emulate_scaled_formats_spirv(
                                    mutableCode, effectiveSize, &remap_info[i], &scaledSize);
                                if (scaledCode) {
                                    free(mutableCode);
                                    mutableCode = scaledCode;
                                    effectiveSize = scaledSize;
                                    total_fixes++;
                                    LOG("  MALI-FIX: stage[%u] SCALED format emulation (%lu→%lu bytes)\n",
                                        s, (unsigned long)codeSize, (unsigned long)scaledSize);
                                }
                            }
                            /* Fix 4: spirv-opt pre-optimized shader replacement.
                             * Mali miscompiles large vertex shaders with OpFunctionCall.
                             * We pre-optimize with spirv-opt (inline + DCE + CCP) and
                             * load the result from /tmp/spirvopt_<cksum>.spv.
                             * This eliminates function calls and simplifies control flow. */
                            if (stageBit == 1 && codeSize >= 8000) {
                                /* Compute XOR checksum of ORIGINAL (pre-fix) code */
                                uint32_t orig_cksum = 0;
                                uint64_t nw_orig = codeSize / 4;
                                for (uint64_t w2 = 0; w2 < nw_orig; w2++)
                                    orig_cksum ^= pCode[w2];
                                char opt_path[128];
                                snprintf(opt_path, sizeof(opt_path),
                                         "/tmp/spirvopt_%08x.spv", orig_cksum);
                                FILE* opt_f = fopen(opt_path, "rb");
                                if (opt_f) {
                                    fseek(opt_f, 0, SEEK_END);
                                    long opt_fsize = ftell(opt_f);
                                    fseek(opt_f, 0, SEEK_SET);
                                    if (opt_fsize > 20 && opt_fsize < 200000) {
                                        uint32_t* opt_code = (uint32_t*)malloc(opt_fsize);
                                        if (opt_code) {
                                            size_t rd = fread(opt_code, 1, opt_fsize, opt_f);
                                            if ((long)rd == opt_fsize && opt_code[0] == 0x07230203) {
                                                free(mutableCode);
                                                mutableCode = opt_code;
                                                effectiveSize = opt_fsize;
                                                total_fixes++;
                                                LOG("  MALI-FIX: stage[%u] SPIRV-OPT replaced cksum=0x%08x (%lu→%lu bytes)\n",
                                                    s, orig_cksum, (unsigned long)codeSize, (unsigned long)opt_fsize);
                                            } else {
                                                free(opt_code);
                                                LOG("  MALI-FIX: stage[%u] SPIRV-OPT file corrupt: %s\n", s, opt_path);
                                            }
                                        }
                                    }
                                    fclose(opt_f);
                                } else {
                                    LOG("  MALI-FIX: stage[%u] no spirv-opt file: %s (cksum=0x%08x)\n",
                                        s, opt_path, orig_cksum);
                                }
                            }
                            if (total_fixes > 0) {
                                memcpy(patched_ci, (const uint8_t*)pn, 40);
                                *(const uint32_t**)(patched_ci + 32) = mutableCode;
                                *(uint64_t*)(patched_ci + 24) = effectiveSize;
                                moduleCI = (const void*)patched_ci;
                            }
                        }
                    }
                    VkResult r;
                    r = real_create_shader_module(real_device, moduleCI, NULL, &new_module);
                    if (mutableCode) free(mutableCode);
                    if (r == 0 && new_module) {
                        *pModule = new_module;
                        temp_modules[n_temp++] = new_module;
                        LOG("  inline->module: stage[%u] bit=0x%x codeSize=%lu module=0x%lx\n",
                            s, stageBit, (unsigned long)codeSize, (unsigned long)new_module);

                        /* CRITICAL: Strip VkShaderModuleCreateInfo (sType=16)
                         * from this stage's pNext chain. Now that we have a real
                         * VkShaderModule, leaving the inline SPIR-V in pNext is
                         * dangerous: FEX thunks for CreateGraphicsPipelines may
                         * try to marshal the pCode pointer across IPC, but the
                         * thunks don't know about maintenance5 inline shaders.
                         * This can corrupt pipeline creation for large shaders
                         * (65KB mesh shader vs 2.4KB font shader). */
                        {
                            void** ppStageNext = (void**)(stage + 8);
                            PNBase* prev2 = NULL;
                            PNBase* cur2 = (PNBase*)*ppStageNext;
                            while (cur2) {
                                if (cur2->sType == 16) {
                                    /* Unlink VkShaderModuleCreateInfo */
                                    if (prev2) prev2->pNext = cur2->pNext;
                                    else *ppStageNext = cur2->pNext;
                                    LOG("  stripped VkShaderModuleCreateInfo from stage[%u] pNext\n", s);
                                    break;
                                }
                                prev2 = cur2;
                                cur2 = (PNBase*)cur2->pNext;
                            }
                        }
                    } else {
                        LOG("  WARNING: failed to create module from inline SPIR-V: %d\n", r);
                    }
                    break;
                }
                pn = (PNBase*)pn->pNext;
            }
        }

        /* Strip VkPipelineCreateFlags2CreateInfoKHR (sType=1000470005) from pipe pNext.
         * Vortek doesn't know this maintenance5 struct and may choke on it. */
        {
            void** ppNext = (void**)(ci + 8);
            PNBase* prev = NULL;
            PNBase* pn = (PNBase*)*ppNext;
            while (pn) {
                if (pn->sType == 1000470005) {
                    LOG("  stripped PipelineCreateFlags2 from pNext\n");
                    if (prev) prev->pNext = pn->pNext;
                    else *ppNext = pn->pNext;
                    break;
                }
                prev = pn;
                pn = (PNBase*)pn->pNext;
            }
        }
    }
    return n_temp;
}

static VkResult trace_CreateGraphicsPipelines(void* device, uint64_t cache, uint32_t count,
                                               const void* pCreateInfos, const void* pAllocator,
                                               uint64_t* pPipelines) {
    void* real = unwrap(device);

    /* Log + patch each pipeline create info */
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* ci = (const uint8_t*)pCreateInfos + i * 144;
        uint32_t stageCount = *(uint32_t*)(ci + 20);
        void* pVertexInputState = *(void**)(ci + 32);
        void* pInputAssemblyState = *(void**)(ci + 40);
        void* pColorBlendState = *(void**)(ci + 88);
        uint64_t renderPass = *(uint64_t*)(ci + 112);
        uint64_t layout = *(uint64_t*)(ci + 104);

        LOG("[D%d] GfxPipe[%u]: stages=%u renderPass=0x%lx layout=0x%lx\n",
            g_device_count, i, stageCount, (unsigned long)renderPass, (unsigned long)layout);

        /* Log vertex input state — key for vertex debugging */
        if (pVertexInputState) {
            /* VkPipelineVertexInputStateCreateInfo LP64:
             * 0:sType 8:pNext 16:flags(4) 20:bindingCount(4) 24:pBindings(8)
             * 32:attributeCount(4) 36:pad 40:pAttributes(8) = 48 */
            uint32_t bindingCount = *(uint32_t*)((uint8_t*)pVertexInputState + 20);
            uint32_t attrCount = *(uint32_t*)((uint8_t*)pVertexInputState + 32);
            LOG("  vertexInput: bindings=%u attrs=%u\n", bindingCount, attrCount);
            /* Log first few bindings */
            if (bindingCount > 0) {
                uint8_t* pBindings = *(uint8_t**)((uint8_t*)pVertexInputState + 24);
                for (uint32_t b = 0; b < bindingCount && b < 4; b++) {
                    /* VkVertexInputBindingDescription: binding(4)+stride(4)+inputRate(4)=12 */
                    uint32_t binding = *(uint32_t*)(pBindings + b*12);
                    uint32_t stride = *(uint32_t*)(pBindings + b*12 + 4);
                    uint32_t rate = *(uint32_t*)(pBindings + b*12 + 8);
                    LOG("    binding[%u]: slot=%u stride=%u rate=%u\n", b, binding, stride, rate);
                }
            }
            /* Log ALL attributes (critical for stride bake debugging) */
            if (attrCount > 0) {
                uint8_t* pAttrs = *(uint8_t**)((uint8_t*)pVertexInputState + 40);
                for (uint32_t a = 0; a < attrCount && a < 16; a++) {
                    /* VkVertexInputAttributeDescription: location(4)+binding(4)+format(4)+offset(4)=16 */
                    uint32_t loc = *(uint32_t*)(pAttrs + a*16);
                    uint32_t bind = *(uint32_t*)(pAttrs + a*16 + 4);
                    uint32_t fmt = *(uint32_t*)(pAttrs + a*16 + 8);
                    uint32_t off = *(uint32_t*)(pAttrs + a*16 + 12);
                    LOG("    attr[%u]: loc=%u bind=%u fmt=%u off=%u\n", a, loc, bind, fmt, off);
                }
            }
        } else {
            LOG("  vertexInput: NULL (vertex pulling?)\n");
        }
        /* Log input assembly topology */
        if (pInputAssemblyState) {
            uint32_t topology = *(uint32_t*)((uint8_t*)pInputAssemblyState + 16);
            LOG("  topology=%u\n", topology); /* 0=POINT_LIST 1=LINE_LIST 2=LINE_STRIP 3=TRIANGLE_LIST 4=TRIANGLE_STRIP 5=TRIANGLE_FAN */
        }
        /* Log dynamic states and strip DYN_STRIDE.
         * FEX thunks can't marshal 7th arg (pStrides) of VB2 from x86-64 stack
         * to ARM64 register x6. We bake strides into pipeline VIS and remove
         * DYN_STRIDE so the driver uses the baked value instead of VB2. */
        {
            void* pDynState = *(void**)(ci + 96);
            if (pDynState) {
                uint32_t* pDynCount = (uint32_t*)((uint8_t*)pDynState + 20);
                uint32_t dynCount = *pDynCount;
                uint32_t* dynStates = *(uint32_t**)((uint8_t*)pDynState + 24);
                LOG("  dynamicStates(%u):", dynCount);
                for (uint32_t d = 0; d < dynCount && d < 20; d++)
                    LOG(" %u", dynStates[d]);
                LOG("\n");
                /* PURE VB2 PASSTHROUGH TEST — keep DYN_STRIDE, don't strip anything.
                 * VB2 passes strides through to Vortek/Mali directly. */
                int has_dyn_stride = 0;
                for (uint32_t d = 0; d < dynCount; d++) {
                    if (dynStates[d] == 1000267005) has_dyn_stride = 1;
                }
                if (has_dyn_stride)
                    LOG("  -> KEEPING DYN_STRIDE (VB2 passthrough test)\n");
            } else {
                LOG("  dynamicStates: NULL\n");
            }
        }

        /* PURE VB2 PASSTHROUGH TEST — don't bake strides, leave pipeline VIS as-is.
         * With DYN_STRIDE kept and VB2 passing strides through, let the driver handle it. */
        if (pVertexInputState) {
            uint32_t bCount = *(uint32_t*)((uint8_t*)pVertexInputState + 20);
            uint8_t* pBind = *(uint8_t**)((uint8_t*)pVertexInputState + 24);
            if (pBind) {
                for (uint32_t b = 0; b < bCount; b++) {
                    uint32_t slot = *(uint32_t*)(pBind + b*12);
                    uint32_t stride = *(uint32_t*)(pBind + b*12 + 4);
                    LOG("  -> PASSTHROUGH stride: binding %u: slot=%u stride=%u (NOT baking)\n",
                        b, slot, stride);
                }
            }
        }
        /* Log shader stages */
        {
            uint8_t* pStages = *(uint8_t**)(ci + 24);
            if (pStages) {
                for (uint32_t s = 0; s < stageCount && s < 6; s++) {
                    uint32_t stageBit = *(uint32_t*)(pStages + s*48 + 20);
                    uint64_t module = *(uint64_t*)(pStages + s*48 + 24);
                    void* pNext = *(void**)(pStages + s*48 + 8);
                    LOG("  stage[%u]: bit=0x%x module=0x%lx pNext=%p\n",
                        s, stageBit, (unsigned long)module, pNext);
                }
            }
        }

        /* Patch: Mali-G720 doesn't support logicOp — force disable */
        if (pColorBlendState) {
            uint32_t* logicOpEnable = (uint32_t*)((uint8_t*)pColorBlendState + 20);
            if (*logicOpEnable) {
                LOG("  -> PATCHING logicOpEnable=0 (Mali unsupported)\n");
                *logicOpEnable = 0;
                *(uint32_t*)((uint8_t*)pColorBlendState + 24) = 0;
            }
        }
    }

    /* VIS experiments removed — pipeline VIS passes through unmodified */

    /* POST-MODIFICATION VERIFICATION: dump raw binding/attr bytes going to thunk */
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* ci = (const uint8_t*)pCreateInfos + i * 144;
        void* pVIS = *(void**)(ci + 32);
        if (!pVIS) continue;
        uint32_t bCount = *(uint32_t*)((uint8_t*)pVIS + 20);
        uint8_t* pBind = *(uint8_t**)((uint8_t*)pVIS + 24);
        uint32_t aCount = *(uint32_t*)((uint8_t*)pVIS + 32);
        uint8_t* pAttr = *(uint8_t**)((uint8_t*)pVIS + 40);
        if (1) { /* log ALL pipelines now */
            LOG("  FINAL-VIS[%u]: bCount=%u aCount=%u\n", i, bCount, aCount);
            for (uint32_t b = 0; b < bCount && b < 4; b++) {
                uint32_t slot = *(uint32_t*)(pBind + b*12);
                uint32_t stride = *(uint32_t*)(pBind + b*12 + 4);
                uint32_t rate = *(uint32_t*)(pBind + b*12 + 8);
                LOG("    bind[%u]: slot=%u stride=%u rate=%u (raw: %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x)\n",
                    b, slot, stride, rate,
                    pBind[b*12+0], pBind[b*12+1], pBind[b*12+2], pBind[b*12+3],
                    pBind[b*12+4], pBind[b*12+5], pBind[b*12+6], pBind[b*12+7],
                    pBind[b*12+8], pBind[b*12+9], pBind[b*12+10], pBind[b*12+11]);
            }
            for (uint32_t a = 0; a < aCount && a < 8; a++) {
                uint32_t loc = *(uint32_t*)(pAttr + a*16);
                uint32_t abind = *(uint32_t*)(pAttr + a*16 + 4);
                uint32_t fmt = *(uint32_t*)(pAttr + a*16 + 8);
                uint32_t off = *(uint32_t*)(pAttr + a*16 + 12);
                /* Human-readable format names for debugging */
                const char* fmtName = "?";
                switch(fmt) {
                    case 37: fmtName="R8G8B8A8_UNORM"; break;
                    case 38: fmtName="R8G8B8A8_SNORM"; break;
                    case 41: fmtName="R8G8B8A8_UINT"; break;
                    case 42: fmtName="R8G8B8A8_USCALED"; break;
                    case 43: fmtName="R8G8B8A8_SSCALED"; break;
                    case 44: fmtName="R8G8B8A8_SINT"; break;
                    case 64: fmtName="A2B10G10R10_UNORM"; break;
                    case 70: fmtName="R16_SFLOAT"; break;
                    case 77: fmtName="R16G16_SNORM"; break;
                    case 83: fmtName="R16G16_SFLOAT"; break;
                    case 91: fmtName="R16G16B16A16_UNORM"; break;
                    case 95: fmtName="R16G16B16A16_UINT"; break;
                    case 97: fmtName="R16G16B16A16_SFLOAT"; break;
                    case 98: fmtName="R32_UINT"; break;
                    case 99: fmtName="R32_SINT"; break;
                    case 100: fmtName="R32_SFLOAT"; break;
                    case 103: fmtName="R32G32_SFLOAT"; break;
                    case 106: fmtName="R32G32B32_SFLOAT"; break;
                    case 109: fmtName="R32G32B32A32_SFLOAT"; break;
                }
                LOG("    attr[%u]: loc=%u bind=%u fmt=%u(%s) off=%u\n",
                    a, loc, abind, fmt, fmtName, off);
            }
        }
    }

    /* ==== USCALED/SSCALED format remap ==== */
    ScaledRemapInfo* scaled_remap = NULL;
    if (count > 0) {
        scaled_remap = (ScaledRemapInfo*)calloc(count, sizeof(ScaledRemapInfo));
        for (uint32_t i = 0; i < count && scaled_remap; i++) {
            const uint8_t* ci = (const uint8_t*)pCreateInfos + i * 144;
            void* pVIS = *(void**)(ci + 32);
            if (!pVIS) continue;
            uint32_t aCount = *(uint32_t*)((uint8_t*)pVIS + 32);
            uint8_t* pAttr = *(uint8_t**)((uint8_t*)pVIS + 40);
            if (!pAttr) continue;
            for (uint32_t a = 0; a < aCount && a < 16; a++) {
                uint32_t loc = *(uint32_t*)(pAttr + a*16);
                uint32_t* pFmt = (uint32_t*)(pAttr + a*16 + 8);
                uint32_t fmt = *pFmt;
                int is_signed = 0, components = 0;
                uint32_t new_fmt = remap_scaled_format(fmt, &is_signed, &components);
                if (new_fmt) {
                    LOG("  SCALED-REMAP: pipe[%u] attr[%u] loc=%u fmt=%u→%u (%s, %d comps)\n",
                        i, a, loc, fmt, new_fmt,
                        is_signed ? "SSCALED→SINT" : "USCALED→UINT", components);
                    *pFmt = new_fmt; /* remap format in-place */
                    if (scaled_remap[i].count < MAX_SCALED_ATTRS) {
                        int idx = scaled_remap[i].count++;
                        scaled_remap[i].attrs[idx].location = loc;
                        scaled_remap[i].attrs[idx].components = components;
                        scaled_remap[i].attrs[idx].is_signed = is_signed;
                    }
                }
            }
        }
    }

    /* Convert inline shaders to real VkShaderModule objects.
     * FEX thunks can't marshal inline SPIR-V pNext across IPC. */
    uint64_t temp_modules[MAX_TEMP_MODULES];
    uint32_t n_temp = 0;
    if (real_create_shader_module) {
        n_temp = fixup_inline_shaders(real, pCreateInfos, count, temp_modules, scaled_remap);
        if (n_temp > 0) LOG("  created %u temp shader modules\n", n_temp);
    }
    free(scaled_remap);

    VkResult res = real_create_gfx_pipelines(real, cache, count, pCreateInfos, pAllocator, pPipelines);
    LOG("[D%d] vkCreateGraphicsPipelines: dev=%p count=%u result=%d\n",
        g_device_count, real, count, res);
    if (res != 0) {
        LOG("[D%d] *** CreateGraphicsPipelines FAILED: count=%u result=%d ***\n",
            g_device_count, count, res);
    }

    /* Cache pipeline VIS info for draw-time binding mismatch checks */
    if (res == 0 && pPipelines) {
        for (uint32_t i = 0; i < count; i++) {
            if (g_pipe_vis_count >= MAX_PIPE_CACHE) break;
            const uint8_t* ci = (const uint8_t*)pCreateInfos + i * 144;
            void* pVIS = *(void**)(ci + 32);
            if (!pVIS) continue;
            uint32_t bCount = *(uint32_t*)((uint8_t*)pVIS + 20);
            uint8_t* pBind = *(uint8_t**)((uint8_t*)pVIS + 24);
            int idx = g_pipe_vis_count++;
            g_pipe_vis[idx].pipeline = pPipelines[i];
            g_pipe_vis[idx].bindingCount = bCount;
            for (uint32_t b = 0; b < bCount && b < 8; b++) {
                g_pipe_vis[idx].bindingSlots[b] = *(uint32_t*)(pBind + b*12);
                g_pipe_vis[idx].strides[b] = *(uint32_t*)(pBind + b*12 + 4);
            }
            LOG("  PIPE-CACHE[%d]: pipe=0x%llx bindings=%u",
                idx, (unsigned long long)pPipelines[i], bCount);
            for (uint32_t b = 0; b < bCount && b < 8; b++)
                LOG(" slot%u:stride%u", g_pipe_vis[idx].bindingSlots[b], g_pipe_vis[idx].strides[b]);
            LOG("\n");
        }
    }

    /* Destroy temporary shader modules */
    if (n_temp > 0 && real_destroy_shader_module) {
        for (uint32_t i = 0; i < n_temp; i++) {
            real_destroy_shader_module(real, temp_modules[i], NULL);
        }
        LOG("  destroyed %u temp shader modules\n", n_temp);
    }

    return res;
}

/* --- vkCreateComputePipelines --- */
typedef VkResult (*PFN_vkCreateComputePipelines)(void*, uint64_t, uint32_t, const void*, const void*, uint64_t*);
static PFN_vkCreateComputePipelines real_create_comp_pipelines = NULL;

static VkResult trace_CreateComputePipelines(void* device, uint64_t cache, uint32_t count,
                                              const void* pCreateInfos, const void* pAllocator,
                                              uint64_t* pPipelines) {
    void* real = unwrap(device);
    typedef struct { uint32_t sType; uint32_t _pad; void* pNext; } PNBase;

    /* VkComputePipelineCreateInfo (LP64):
     *   0: sType(4)+pad(4)  8: pNext(8)  16: flags(4)+pad(4)
     *  24: stage(48 = VkPipelineShaderStageCreateInfo)  72: layout(8)  80: basePipeHandle(8)  88: basePipeIndex(4)
     * Total ~ 96 bytes
     */
    uint64_t temp_modules[MAX_TEMP_MODULES];
    uint32_t n_temp = 0;

    for (uint32_t i = 0; i < count; i++) {
        uint8_t* ci = (uint8_t*)pCreateInfos + i * 96;
        /* stage is embedded at offset 24, module at stage+24 = ci+48 */
        uint64_t* pModule = (uint64_t*)(ci + 24 + 24);

        if (*pModule == 0 && real_create_shader_module && n_temp < MAX_TEMP_MODULES) {
            /* Walk stage pNext for inline VkShaderModuleCreateInfo */
            PNBase* pn = (PNBase*)(*(void**)(ci + 24 + 8));
            while (pn) {
                if (pn->sType == 16) {
                    uint64_t new_module = 0;
                    VkResult r = real_create_shader_module(real, (const void*)pn, NULL, &new_module);
                    if (r == 0 && new_module) {
                        *pModule = new_module;
                        temp_modules[n_temp++] = new_module;
                        LOG("[D%d] CompPipe[%u]: inline->module 0x%lx\n",
                            g_device_count, i, (unsigned long)new_module);
                    }
                    break;
                }
                pn = (PNBase*)pn->pNext;
            }
        }

        /* Strip VkPipelineCreateFlags2CreateInfoKHR from pipe pNext */
        {
            void** ppNext = (void**)(ci + 8);
            PNBase* prev = NULL;
            PNBase* pn = (PNBase*)*ppNext;
            while (pn) {
                if (pn->sType == 1000470005) {
                    if (prev) prev->pNext = pn->pNext;
                    else *ppNext = pn->pNext;
                    break;
                }
                prev = pn;
                pn = (PNBase*)pn->pNext;
            }
        }
    }

    VkResult res = real_create_comp_pipelines(real, cache, count, pCreateInfos, pAllocator, pPipelines);
    LOG("[D%d] vkCreateComputePipelines: dev=%p count=%u result=%d\n",
        g_device_count, real, count, res);
    if (res != 0) {
        LOG("[D%d] *** CreateComputePipelines FAILED: count=%u result=%d ***\n",
            g_device_count, count, res);
    }

    /* Destroy temporary modules */
    if (n_temp > 0 && real_destroy_shader_module) {
        for (uint32_t i = 0; i < n_temp; i++)
            real_destroy_shader_module(real, temp_modules[i], NULL);
    }

    return res;
}

/* ==== Forward declarations for memory requirements (defined later) ==== */
typedef void (*PFN_vkGetBufMemReqs)(void*, uint64_t, void*);
static PFN_vkGetBufMemReqs real_get_buf_mem_reqs;

typedef void (*PFN_vkGetImgMemReqs)(void*, uint64_t, void*);
static PFN_vkGetImgMemReqs real_get_img_mem_reqs;

/* ==== Null Descriptor Guard ====
 *
 * When nullDescriptor=1 is spoofed, DXVK writes VK_NULL_HANDLE into descriptor
 * sets for unused bindings. Vortek's vt_handle_vkUpdateDescriptorSets crashes
 * when VkObject_fromId(0) returns NULL. We intercept UpdateDescriptorSets and
 * replace NULL handles with real dummy resources.
 */

/* Dummy resource handles — created lazily on first null encounter */
static uint64_t g_dummy_sampler = 0;
static uint64_t g_dummy_image_view = 0;
static uint64_t g_dummy_buffer = 0;
static uint64_t g_dummy_buffer_view = 0;
static uint64_t g_dummy_image = 0;
static uint64_t g_dummy_memory = 0;
static int g_dummies_init = 0;

typedef VkResult (*PFN_vkCreateBufferView)(void*, const void*, const void*, uint64_t*);
static PFN_vkCreateBufferView real_create_buffer_view = NULL;

/* Uses existing fn ptrs: real_create_buffer, real_create_image, real_create_sampler,
 * real_create_image_view, real_alloc_memory, real_bind_img_mem, real_bind_buf_mem,
 * real_get_img_mem_reqs, real_get_buf_mem_reqs */

/* Resolve a device-level function pointer if still NULL */
static PFN_vkVoidFunction resolve_dev_fn(void* real_device, const char* name) {
    PFN_vkVoidFunction fn = NULL;
    if (real_gipa && saved_instance)
        fn = real_gipa(saved_instance, name);
    if (!fn && thunk_lib)
        fn = (PFN_vkVoidFunction)dlsym(thunk_lib, name);
    if (!fn && real_gdpa && real_device)
        fn = real_gdpa(real_device, name);
    return fn;
}

static void create_dummy_resources(void* real_device) {
    if (g_dummies_init) return;
    g_dummies_init = 1;

    LOG("Creating dummy resources for null descriptors\n");

    /* Resolve any fn ptrs that GDPA hasn't captured yet */
    if (!real_get_buf_mem_reqs)
        real_get_buf_mem_reqs = (PFN_vkGetBufMemReqs)resolve_dev_fn(real_device, "vkGetBufferMemoryRequirements");
    if (!real_get_img_mem_reqs)
        real_get_img_mem_reqs = (PFN_vkGetImgMemReqs)resolve_dev_fn(real_device, "vkGetImageMemoryRequirements");
    if (!real_create_image_view)
        real_create_image_view = (PFN_vkCreateImageView)resolve_dev_fn(real_device, "vkCreateImageView");
    if (!real_create_buffer_view)
        real_create_buffer_view = (PFN_vkCreateBufferView)resolve_dev_fn(real_device, "vkCreateBufferView");
    if (!real_bind_img_mem)
        real_bind_img_mem = (PFN_vkBindImageMemory)resolve_dev_fn(real_device, "vkBindImageMemory");
    if (!real_bind_buf_mem)
        real_bind_buf_mem = (PFN_vkBindBufferMemory)resolve_dev_fn(real_device, "vkBindBufferMemory");
    if (!real_bind_buf_mem2)
        real_bind_buf_mem2 = (PFN_vkBindBufferMemory2)resolve_dev_fn(real_device, "vkBindBufferMemory2");
    if (!real_alloc_memory)
        real_alloc_memory = (PFN_vkAllocateMemory)resolve_dev_fn(real_device, "vkAllocateMemory");
    if (!real_map_memory)
        real_map_memory = (PFN_vkMapMemory)resolve_dev_fn(real_device, "vkMapMemory");
    if (!real_create_sampler)
        real_create_sampler = (PFN_vkCreateSampler)resolve_dev_fn(real_device, "vkCreateSampler");
    if (!real_create_buffer)
        real_create_buffer = (PFN_vkCreateBuffer)resolve_dev_fn(real_device, "vkCreateBuffer");
    if (!real_create_image)
        real_create_image = (PFN_vkCreateImage)resolve_dev_fn(real_device, "vkCreateImage");
    LOG("  resolved: sampler=%p buf=%p img=%p imgView=%p bufView=%p alloc=%p bindBuf=%p bindImg=%p getBufReqs=%p getImgReqs=%p\n",
        (void*)real_create_sampler, (void*)real_create_buffer, (void*)real_create_image,
        (void*)real_create_image_view, (void*)real_create_buffer_view,
        (void*)real_alloc_memory, (void*)real_bind_buf_mem, (void*)real_bind_img_mem,
        (void*)real_get_buf_mem_reqs, (void*)real_get_img_mem_reqs);

    /* Dummy sampler (minimal) */
    if (real_create_sampler) {
        /* VkSamplerCreateInfo: sType=31, minimal config */
        uint8_t sci[80];
        memset(sci, 0, sizeof(sci));
        *(uint32_t*)sci = 31; /* VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO */
        /* All filter/address modes default to 0 = NEAREST/REPEAT */
        *(float*)(sci + 40) = 1.0f; /* maxAnisotropy */
        *(float*)(sci + 52) = 1000.0f; /* maxLod */
        VkResult r = real_create_sampler(real_device, sci, NULL, &g_dummy_sampler);
        LOG("  dummy sampler: %s (0x%lx)\n", r == 0 ? "OK" : "FAIL", (unsigned long)g_dummy_sampler);
    }

    /* Dummy buffer (16 bytes) */
    if (real_create_buffer) {
        /* VkBufferCreateInfo: sType=12 */
        uint8_t bci[56];
        memset(bci, 0, sizeof(bci));
        *(uint32_t*)bci = 12; /* VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO */
        *(uint64_t*)(bci + 24) = 256; /* size */
        *(uint32_t*)(bci + 32) = 0x1FF; /* usage: all transfer+vertex+index+uniform+storage+indirect */
        VkResult r = real_create_buffer(real_device, bci, NULL, &g_dummy_buffer);
        LOG("  dummy buffer: %s (0x%lx)\n", r == 0 ? "OK" : "FAIL", (unsigned long)g_dummy_buffer);

        /* Allocate and bind memory for the dummy buffer */
        if (r == 0 && real_get_buf_mem_reqs && real_alloc_memory && real_bind_buf_mem) {
            uint8_t memReqs[24]; /* VkMemoryRequirements: size(8)+align(8)+memTypeBits(4) */
            real_get_buf_mem_reqs(real_device, g_dummy_buffer, memReqs);
            uint64_t memSize = *(uint64_t*)memReqs;
            uint32_t memBits = *(uint32_t*)(memReqs + 16);

            /* Find first valid memory type */
            uint32_t memType = 0;
            for (uint32_t i = 0; i < 32; i++) {
                if (memBits & (1u << i)) { memType = i; break; }
            }

            /* VkMemoryAllocateInfo LP64:
             * sType(4)+pad(4)+pNext(8)+allocationSize(8)+memoryTypeIndex(4) = 28, pad to 32 */
            uint8_t mai2[32];
            memset(mai2, 0, sizeof(mai2));
            *(uint32_t*)(mai2 + 0) = 5; /* sType */
            *(uint64_t*)(mai2 + 16) = memSize; /* allocationSize */
            *(uint32_t*)(mai2 + 24) = memType; /* memoryTypeIndex */

            r = real_alloc_memory(real_device, mai2, NULL, &g_dummy_memory);
            if (r == 0) {
                real_bind_buf_mem(real_device, g_dummy_buffer, g_dummy_memory, 0);
                LOG("  dummy buffer memory bound OK (size=%lu type=%u)\n", (unsigned long)memSize, memType);
            }
        }
    }

    /* Dummy image (1x1 R8G8B8A8) */
    if (real_create_image) {
        /* VkImageCreateInfo: 88 bytes on LP64 */
        uint8_t ici[96];
        memset(ici, 0, sizeof(ici));
        *(uint32_t*)ici = 14; /* VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO = 14 */
        *(uint32_t*)(ici + 16) = 0; /* flags */
        *(uint32_t*)(ici + 20) = 1; /* imageType = VK_IMAGE_TYPE_2D */
        *(uint32_t*)(ici + 24) = 37; /* format = VK_FORMAT_R8G8B8A8_UNORM */
        *(uint32_t*)(ici + 28) = 1; /* width */
        *(uint32_t*)(ici + 32) = 1; /* height */
        *(uint32_t*)(ici + 36) = 1; /* depth */
        *(uint32_t*)(ici + 40) = 1; /* mipLevels */
        *(uint32_t*)(ici + 44) = 1; /* arrayLayers */
        *(uint32_t*)(ici + 48) = 1; /* samples = VK_SAMPLE_COUNT_1_BIT */
        *(uint32_t*)(ici + 52) = 0; /* tiling = VK_IMAGE_TILING_OPTIMAL */
        *(uint32_t*)(ici + 56) = 0x6; /* usage = TRANSFER_DST | SAMPLED */
        VkResult r = real_create_image(real_device, ici, NULL, &g_dummy_image);
        LOG("  dummy image: %s (0x%lx)\n", r == 0 ? "OK" : "FAIL", (unsigned long)g_dummy_image);

        /* Bind memory for dummy image */
        if (r == 0 && real_get_img_mem_reqs && real_alloc_memory && real_bind_img_mem) {
            uint8_t memReqs[24];
            real_get_img_mem_reqs(real_device, g_dummy_image, memReqs);
            uint64_t memSize = *(uint64_t*)memReqs;
            uint32_t memBits = *(uint32_t*)(memReqs + 16);
            uint32_t memType = 0;
            for (uint32_t i = 0; i < 32; i++) {
                if (memBits & (1u << i)) { memType = i; break; }
            }
            uint64_t imgMem = 0;
            uint8_t mai2[32];
            memset(mai2, 0, sizeof(mai2));
            *(uint32_t*)mai2 = 5;
            *(uint64_t*)(mai2 + 16) = memSize;
            *(uint32_t*)(mai2 + 24) = memType;
            r = real_alloc_memory(real_device, mai2, NULL, &imgMem);
            if (r == 0) {
                real_bind_img_mem(real_device, g_dummy_image, imgMem, 0);
                LOG("  dummy image memory bound OK (size=%lu type=%u)\n", (unsigned long)memSize, memType);
            } else {
                LOG("  dummy image memory alloc FAILED: %d\n", r);
            }
        }

        /* Dummy image view
         * VkImageViewCreateInfo LP64 layout:
         *   0: sType(4) 4:pad 8:pNext(8) 16:flags(4) 20:pad
         *  24: image(8) 32:viewType(4) 36:format(4)
         *  40: components(4x4=16)  56: subresourceRange(4+4x4=20)
         *  total = 76, padded to 80 */
        if (g_dummy_image && real_create_image_view) {
            uint8_t ivci[80];
            memset(ivci, 0, sizeof(ivci));
            *(uint32_t*)(ivci + 0)  = 15; /* VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO */
            *(uint64_t*)(ivci + 24) = g_dummy_image; /* image */
            *(uint32_t*)(ivci + 32) = 1;  /* viewType = VK_IMAGE_VIEW_TYPE_2D */
            *(uint32_t*)(ivci + 36) = 37; /* format = VK_FORMAT_R8G8B8A8_UNORM */
            /* componentMapping at 40: all 0 = IDENTITY */
            /* subresourceRange at 56: */
            *(uint32_t*)(ivci + 56) = 1;  /* aspectMask = VK_IMAGE_ASPECT_COLOR_BIT */
            *(uint32_t*)(ivci + 60) = 0;  /* baseMipLevel */
            *(uint32_t*)(ivci + 64) = 1;  /* levelCount */
            *(uint32_t*)(ivci + 68) = 0;  /* baseArrayLayer */
            *(uint32_t*)(ivci + 72) = 1;  /* layerCount */
            LOG("  creating imageView: image=0x%lx\n", (unsigned long)g_dummy_image);
            r = real_create_image_view(real_device, ivci, NULL, &g_dummy_image_view);
            LOG("  dummy imageView: %s (0x%lx)\n", r == 0 ? "OK" : "FAIL", (unsigned long)g_dummy_image_view);
        }
    }

    /* Dummy buffer view
     * VkBufferViewCreateInfo LP64 layout:
     *   0: sType(4) 4:pad 8:pNext(8) 16:flags(4) 20:pad
     *  24: buffer(8) 32:format(4) 36:pad 40:offset(8) 48:range(8)
     *  total = 56 */
    if (g_dummy_buffer && real_create_buffer_view) {
        uint8_t bvci[56];
        memset(bvci, 0, sizeof(bvci));
        *(uint32_t*)(bvci + 0)  = 13; /* VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO */
        *(uint64_t*)(bvci + 24) = g_dummy_buffer; /* buffer */
        *(uint32_t*)(bvci + 32) = 37; /* format = R8G8B8A8_UNORM */
        *(uint64_t*)(bvci + 40) = 0;  /* offset */
        *(uint64_t*)(bvci + 48) = 256; /* range */
        LOG("  creating bufferView: buf=0x%lx\n", (unsigned long)g_dummy_buffer);
        VkResult r = real_create_buffer_view(real_device, bvci, NULL, &g_dummy_buffer_view);
        LOG("  dummy bufferView: %s (0x%lx)\n", r == 0 ? "OK" : "FAIL", (unsigned long)g_dummy_buffer_view);
    }
}

/* vkUpdateDescriptorSets interceptor */
typedef void (*PFN_vkUpdateDescriptorSets)(void*, uint32_t, const void*, uint32_t, const void*);
static PFN_vkUpdateDescriptorSets real_update_desc_sets = NULL;

/* VkWriteDescriptorSet layout (LP64):
 *   0: sType(4)+pad(4)  8: pNext(8)  16: dstSet(8)  24: dstBinding(4)
 *  28: dstArrayElement(4)  32: descriptorCount(4)  36: descriptorType(4)
 *  40: pImageInfo(8)  48: pBufferInfo(8)  56: pTexelBufferView(8)
 *  total = 64 bytes
 *
 * VkDescriptorImageInfo: sampler(8)+imageView(8)+imageLayout(4)+pad(4) = 24
 * VkDescriptorBufferInfo: buffer(8)+offset(8)+range(8) = 24
 */
#define WRITE_DESC_SET_SIZE 64

/* Check if a descriptor write entry has any NULL handles that we can't fix.
 * Returns 1 if the write is safe to send to Vortek, 0 if it must be skipped. */
static int fix_or_check_write(uint8_t* ws) {
    uint32_t count = *(uint32_t*)(ws + 32);
    uint32_t type = *(uint32_t*)(ws + 36);

    /* Types that use pImageInfo: SAMPLER(0), COMBINED_IMAGE_SAMPLER(1),
     * SAMPLED_IMAGE(2), STORAGE_IMAGE(3), INPUT_ATTACHMENT(10) */
    if (type <= 3 || type == 10) {
        uint8_t* pImageInfo = *(uint8_t**)(ws + 40);
        if (!pImageInfo) return 1;
        for (uint32_t d = 0; d < count; d++) {
            uint64_t* sampler = (uint64_t*)(pImageInfo + d * 24 + 0);
            uint64_t* imageView = (uint64_t*)(pImageInfo + d * 24 + 8);

            /* Fix NULL sampler */
            if ((type == 0 || type == 1) && *sampler == 0) {
                if (g_dummy_sampler) *sampler = g_dummy_sampler;
                else return 0; /* can't fix */
            }
            /* Fix NULL imageView */
            if (type != 0 && *imageView == 0) {
                if (g_dummy_image_view) {
                    *imageView = g_dummy_image_view;
                    uint32_t* layout = (uint32_t*)(pImageInfo + d * 24 + 16);
                    if (*layout == 0) *layout = 1; /* VK_IMAGE_LAYOUT_GENERAL */
                } else {
                    return 0; /* can't fix — skip entire write */
                }
            }
        }
        return 1;
    }
    /* Types that use pBufferInfo: UNIFORM_BUFFER(6), STORAGE_BUFFER(7),
     * UNIFORM_BUFFER_DYNAMIC(8), STORAGE_BUFFER_DYNAMIC(9) */
    if (type >= 6 && type <= 9) {
        uint8_t* pBufferInfo = *(uint8_t**)(ws + 48);
        if (!pBufferInfo) return 1;
        for (uint32_t d = 0; d < count; d++) {
            uint64_t* buffer = (uint64_t*)(pBufferInfo + d * 24);
            if (*buffer == 0) {
                if (g_dummy_buffer) {
                    *buffer = g_dummy_buffer;
                    uint64_t* range = (uint64_t*)(pBufferInfo + d * 24 + 16);
                    if (*range == 0) *range = 256;
                } else {
                    return 0;
                }
            }
        }
        return 1;
    }
    /* Types that use pTexelBufferView: UNIFORM_TEXEL_BUFFER(4), STORAGE_TEXEL_BUFFER(5) */
    if (type == 4 || type == 5) {
        uint64_t* pTexelViews = *(uint64_t**)(ws + 56);
        if (!pTexelViews) return 1;
        for (uint32_t d = 0; d < count; d++) {
            if (pTexelViews[d] == 0) {
                if (g_dummy_buffer_view)
                    pTexelViews[d] = g_dummy_buffer_view;
                else
                    return 0;
            }
        }
        return 1;
    }
    return 1; /* unknown type — pass through */
}

/* ==== Descriptor Update Template Tracking ====
 *
 * DXVK uses vkUpdateDescriptorSetWithTemplate for performance.
 * We must track each template's entry layout so we can scan the raw pData
 * blob for NULL handles and replace them with dummy resources.
 *
 * VkDescriptorUpdateTemplateEntry LP64 layout:
 *   0: dstBinding(4)  4: dstArrayElement(4)  8: descriptorCount(4)
 *  12: descriptorType(4)  16: offset(8)  24: stride(8)   total=32
 *
 * VkDescriptorUpdateTemplateCreateInfo LP64:
 *   0:sType 8:pNext 16:flags(4) 20:entryCount(4) 24:pEntries(8)
 *  32:templateType(4) ... */

typedef struct {
    uint32_t dstBinding;
    uint32_t descriptorCount;
    uint32_t descriptorType;
    uint64_t offset;
    uint64_t stride;
} TemplateEntryCompact;

typedef struct {
    uint64_t templateHandle;
    uint32_t entryCount;
    TemplateEntryCompact* entries;
} TrackedTemplate;

#define MAX_TRACKED_TEMPLATES 256
static TrackedTemplate g_templates[MAX_TRACKED_TEMPLATES];
static int g_template_count = 0;

typedef VkResult (*PFN_vkCreateDescUpdateTemplate)(void*, const void*, const void*, uint64_t*);
static PFN_vkCreateDescUpdateTemplate real_create_desc_update_template = NULL;

static VkResult null_guard_CreateDescriptorUpdateTemplate(void* device, const void* pCreateInfo,
                                                           const void* pAllocator, uint64_t* pTemplate) {
    void* real = unwrap(device);
    VkResult res = real_create_desc_update_template(real, pCreateInfo, pAllocator, pTemplate);
    if (res != 0 || !pTemplate || !*pTemplate || !pCreateInfo) return res;

    /* Parse VkDescriptorUpdateTemplateCreateInfo to save entry layout */
    const uint8_t* ci = (const uint8_t*)pCreateInfo;
    uint32_t entryCount = *(const uint32_t*)(ci + 20);
    const uint8_t* pEntries = *(const uint8_t* const*)(ci + 24);

    if (entryCount > 0 && pEntries && g_template_count < MAX_TRACKED_TEMPLATES) {
        TrackedTemplate* t = &g_templates[g_template_count];
        t->templateHandle = *pTemplate;
        t->entryCount = entryCount;
        t->entries = (TemplateEntryCompact*)malloc(entryCount * sizeof(TemplateEntryCompact));
        if (t->entries) {
            for (uint32_t i = 0; i < entryCount; i++) {
                const uint8_t* e = pEntries + i * 32;
                t->entries[i].dstBinding = *(const uint32_t*)(e + 0);
                t->entries[i].descriptorCount = *(const uint32_t*)(e + 8);
                t->entries[i].descriptorType = *(const uint32_t*)(e + 12);
                t->entries[i].offset = *(const uint64_t*)(e + 16);
                t->entries[i].stride = *(const uint64_t*)(e + 24);
            }
            g_template_count++;
            LOG("DescUpdateTemplate: handle=0x%lx entries=%u (tracked #%d)\n",
                (unsigned long)*pTemplate, entryCount, g_template_count);
        }
    }
    return res;
}

static TrackedTemplate* find_template(uint64_t handle) {
    for (int i = 0; i < g_template_count; i++) {
        if (g_templates[i].templateHandle == handle)
            return &g_templates[i];
    }
    return NULL;
}

typedef void (*PFN_vkUpdateDescSetWithTemplate)(void*, uint64_t, uint64_t, const void*);
static PFN_vkUpdateDescSetWithTemplate real_update_desc_set_with_template = NULL;

static void null_guard_UpdateDescriptorSetWithTemplate(void* device, uint64_t descriptorSet,
                                                        uint64_t descriptorUpdateTemplate,
                                                        const void* pData) {
    void* real = unwrap(device);
    if (!g_dummies_init) create_dummy_resources(real);

    TrackedTemplate* tmpl = find_template(descriptorUpdateTemplate);
    /* Log first few template updates to diagnose UBO bindings */
    static int tmpl_log_count = 0;
    tmpl_log_count++;
    if (tmpl && pData && tmpl_log_count <= 50) {
        LOG("DescTemplUpdate: set=0x%lx tmpl=0x%lx entries=%u\n",
            (unsigned long)descriptorSet, (unsigned long)descriptorUpdateTemplate, tmpl->entryCount);
        for (uint32_t e = 0; e < tmpl->entryCount && e < 8; e++) {
            uint32_t type = tmpl->entries[e].descriptorType;
            uint64_t off = tmpl->entries[e].offset;
            uint64_t stride = tmpl->entries[e].stride;
            uint32_t count = tmpl->entries[e].descriptorCount;
            uint32_t dstBind = tmpl->entries[e].dstBinding;
            /* For UBO/SSBO types, dump buffer handle+offset+range */
            if (type >= 6 && type <= 9) {
                uint8_t* p = (uint8_t*)pData + off;
                uint64_t buf = *(uint64_t*)(p + 0);
                uint64_t boff = *(uint64_t*)(p + 8);
                uint64_t range = *(uint64_t*)(p + 16);
                LOG("  entry[%u] bind=%u type=%u count=%u: buf=0x%lx off=%lu range=%lu\n",
                    e, dstBind, type, count, (unsigned long)buf, (unsigned long)boff, (unsigned long)range);
            } else {
                LOG("  entry[%u] bind=%u type=%u count=%u off=%lu stride=%lu\n",
                    e, dstBind, type, count, (unsigned long)off, (unsigned long)stride);
            }
        }
    }
    /* Track UBO/SSBO buffer+offset per descriptor set for readback at draw time.
     * Also track SSBO[0] for staging copy readback. */
    if (tmpl && pData) {
        /* Populate g_set_ubo_track for UBO(6) AND SSBO(7) from template updates */
        for (uint32_t e = 0; e < tmpl->entryCount; e++) {
            uint32_t type = tmpl->entries[e].descriptorType;
            uint32_t dstBind = tmpl->entries[e].dstBinding;
            if ((type == 6 || type == 7) && dstBind < MAX_LAST_UBO) {
                uint64_t off = tmpl->entries[e].offset;
                uint8_t* p = (uint8_t*)pData + off;
                uint64_t buf = *(uint64_t*)(p + 0);
                uint64_t boff = *(uint64_t*)(p + 8);
                uint64_t range = *(uint64_t*)(p + 16);
                if (buf != 0) {
                    int idx = -1;
                    for (int si = 0; si < g_set_ubo_track_count; si++) {
                        if (g_set_ubo_track[si].set_handle == descriptorSet) {
                            idx = si; break;
                        }
                    }
                    if (idx < 0 && g_set_ubo_track_count < MAX_SET_UBO_TRACK) {
                        idx = g_set_ubo_track_count++;
                        g_set_ubo_track[idx].set_handle = descriptorSet;
                        g_set_ubo_track[idx].ubo_count = 0;
                    }
                    if (idx >= 0) {
                        g_set_ubo_track[idx].ubos[dstBind].buffer = buf;
                        g_set_ubo_track[idx].ubos[dstBind].offset = boff;
                        g_set_ubo_track[idx].ubos[dstBind].range = range;
                        if ((int)(dstBind + 1) > g_set_ubo_track[idx].ubo_count)
                            g_set_ubo_track[idx].ubo_count = dstBind + 1;
                    }
                }
            }
        }
        for (uint32_t e = 0; e < tmpl->entryCount; e++) {
            uint32_t type = tmpl->entries[e].descriptorType;
            if (type == 7) { /* STORAGE_BUFFER */
                uint64_t off = tmpl->entries[e].offset;
                uint8_t* p = (uint8_t*)pData + off;
                uint64_t buf = *(uint64_t*)(p + 0);
                uint64_t boff = *(uint64_t*)(p + 8);
                uint64_t range = *(uint64_t*)(p + 16);
                if (range > 2000 && range < 10000 && buf != 0) {
                    g_ssbo0_buf = buf;
                    g_ssbo0_off = boff;
                    g_ssbo0_range = range;
                }
            }
        }
    }
    /* Read back UBO data from template updates (per-draw transforms) */
    if (tmpl && pData && tmpl_log_count >= 100 && tmpl_log_count <= 120) {
        for (uint32_t e = 0; e < tmpl->entryCount; e++) {
            uint32_t type = tmpl->entries[e].descriptorType;
            if (type == 6 || type == 7) { /* UNIFORM_BUFFER or STORAGE_BUFFER */
                uint64_t off = tmpl->entries[e].offset;
                uint8_t* p = (uint8_t*)pData + off;
                uint64_t buf = *(uint64_t*)(p + 0);
                uint64_t boff = *(uint64_t*)(p + 8);
                uint64_t range = *(uint64_t*)(p + 16);
                void* ubo_ptr = lookup_ubo_ptr(buf, boff);
                LOG("TMPL-UBO[%u]: type=%u buf=0x%lx off=%lu range=%lu ptr=%p (bufs=%d maps=%d)\n",
                    e, type, (unsigned long)buf, (unsigned long)boff,
                    (unsigned long)range, ubo_ptr, g_buf_mem_count, g_map_ptr_count);
                if (!ubo_ptr) {
                    /* Diagnose WHY lookup failed */
                    uint64_t found_mem = 0;
                    for (int ii = g_buf_mem_count - 1; ii >= 0; ii--) {
                        if (g_buf_mem[ii].buffer == buf) {
                            found_mem = g_buf_mem[ii].memory;
                            LOG("  DIAG: buf found in g_buf_mem[%d] mem=0x%lx memOff=%lu\n",
                                ii, (unsigned long)found_mem, (unsigned long)g_buf_mem[ii].memOffset);
                            break;
                        }
                    }
                    if (!found_mem) {
                        LOG("  DIAG: buf 0x%lx NOT FOUND in g_buf_mem (searched %d entries)\n",
                            (unsigned long)buf, g_buf_mem_count);
                    } else {
                        int map_found = 0;
                        for (int ii = g_map_ptr_count - 1; ii >= 0; ii--) {
                            if (g_map_ptrs[ii].memory == found_mem) {
                                map_found = 1;
                                LOG("  DIAG: mem 0x%lx found in g_map_ptrs[%d] ptr=%p mapOff=%lu\n",
                                    (unsigned long)found_mem, ii,
                                    g_map_ptrs[ii].pointer, (unsigned long)g_map_ptrs[ii].mapOffset);
                                break;
                            }
                        }
                        if (!map_found) {
                            LOG("  DIAG: mem 0x%lx NOT MAPPED (searched %d map entries) -> DEVICE_LOCAL only?\n",
                                (unsigned long)found_mem, g_map_ptr_count);
                        }
                    }
                }
                /* FULL-SCAN removed — this SSBO is the text/glyph table, not mesh transform.
                 * Mesh UBO readback now happens at CmdDrawIndexed time via g_last_ubo. */
            }
        }
    }
    if (tmpl && pData) {
        uint8_t* data = (uint8_t*)pData; /* mutable cast — we fix NULLs in-place */
        for (uint32_t e = 0; e < tmpl->entryCount; e++) {
            uint32_t type = tmpl->entries[e].descriptorType;
            uint64_t off = tmpl->entries[e].offset;
            uint64_t stride = tmpl->entries[e].stride;
            uint32_t count = tmpl->entries[e].descriptorCount;

            for (uint32_t d = 0; d < count; d++) {
                uint8_t* p = data + off + d * stride;

                /* Image types: sampler(8)+imageView(8)+imageLayout(4) at p */
                if (type <= 3 || type == 10) {
                    uint64_t* sampler = (uint64_t*)(p + 0);
                    uint64_t* imageView = (uint64_t*)(p + 8);
                    if ((type == 0 || type == 1) && *sampler == 0 && g_dummy_sampler)
                        *sampler = g_dummy_sampler;
                    if (type != 0 && *imageView == 0 && g_dummy_image_view) {
                        *imageView = g_dummy_image_view;
                        uint32_t* layout = (uint32_t*)(p + 16);
                        if (*layout == 0) *layout = 1;
                    }
                }
                /* Buffer types: buffer(8)+offset(8)+range(8) at p */
                else if (type >= 6 && type <= 9) {
                    uint64_t* buffer = (uint64_t*)(p + 0);
                    if (*buffer == 0 && g_dummy_buffer) {
                        *buffer = g_dummy_buffer;
                        uint64_t* range = (uint64_t*)(p + 16);
                        if (*range == 0) *range = 256;
                    }
                }
                /* Texel buffer: VkBufferView (uint64_t) at p */
                else if (type == 4 || type == 5) {
                    uint64_t* view = (uint64_t*)p;
                    if (*view == 0 && g_dummy_buffer_view)
                        *view = g_dummy_buffer_view;
                }
            }
        }
    }

    real_update_desc_set_with_template(real, descriptorSet, descriptorUpdateTemplate, pData);
}

static int g_null_guard_logged = 0;
static int g_uds_log_count = 0;

static void null_guard_UpdateDescriptorSets(void* device, uint32_t writeCount,
                                             const void* pWrites,
                                             uint32_t copyCount, const void* pCopies) {
    void* real = unwrap(device);

    /* Lazily init dummy resources on first call */
    if (!g_dummies_init) create_dummy_resources(real);

    /* Track UBO entries for ALL UDS calls; only LOG first N */
    g_uds_log_count++;
    if (pWrites) {
        for (uint32_t w = 0; w < writeCount; w++) {
            const uint8_t* ws = (const uint8_t*)pWrites + w * WRITE_DESC_SET_SIZE;
            uint64_t dstSet = *(const uint64_t*)(ws + 16);
            uint32_t dstBinding = *(const uint32_t*)(ws + 24);
            uint32_t count = *(const uint32_t*)(ws + 32);
            uint32_t type = *(const uint32_t*)(ws + 36);

            /* Track type=6 UBO entries PER DESCRIPTOR SET — always, for readback */
            if (type >= 6 && type <= 9) {
                const uint8_t* pBufInfo = *(const uint8_t* const*)(ws + 48);
                if (pBufInfo) {
                    uint64_t buf = *(const uint64_t*)(pBufInfo + 0);
                    uint64_t boff = *(const uint64_t*)(pBufInfo + 8);
                    uint64_t range = *(const uint64_t*)(pBufInfo + 16);
                    if (g_uds_log_count <= 200) {
                        LOG("UDS[%u]: set=0x%lx bind=%u type=%u buf=0x%lx off=%lu range=%lu\n",
                            g_uds_log_count, (unsigned long)dstSet, dstBinding, type,
                            (unsigned long)buf, (unsigned long)boff, (unsigned long)range);
                    }
                    if ((type == 6 || type == 7) && dstBinding < MAX_LAST_UBO) {
                        int idx = -1;
                        for (int si = 0; si < g_set_ubo_track_count; si++) {
                            if (g_set_ubo_track[si].set_handle == dstSet) {
                                idx = si; break;
                            }
                        }
                        if (idx < 0 && g_set_ubo_track_count < MAX_SET_UBO_TRACK) {
                            idx = g_set_ubo_track_count++;
                            g_set_ubo_track[idx].set_handle = dstSet;
                            g_set_ubo_track[idx].ubo_count = 0;
                        }
                        if (idx >= 0) {
                            g_set_ubo_track[idx].ubos[dstBinding].buffer = buf;
                            g_set_ubo_track[idx].ubos[dstBinding].offset = boff;
                            g_set_ubo_track[idx].ubos[dstBinding].range = range;
                            if ((int)(dstBinding + 1) > g_set_ubo_track[idx].ubo_count)
                                g_set_ubo_track[idx].ubo_count = dstBinding + 1;
                        }
                    }
                }
            } else if (g_uds_log_count <= 50) {
                LOG("UDS[%u]: set=0x%lx bind=%u type=%u count=%u\n",
                    g_uds_log_count, (unsigned long)dstSet, dstBinding, type, count);
            }
        }
    }

    /* Build filtered writes array: fix NULL handles or skip unfixable writes */
    uint8_t filtered[64 * WRITE_DESC_SET_SIZE]; /* stack buffer for up to 64 writes */
    uint8_t* heap_buf = NULL;
    uint8_t* out = filtered;

    if (writeCount > 64) {
        heap_buf = (uint8_t*)malloc(writeCount * WRITE_DESC_SET_SIZE);
        out = heap_buf ? heap_buf : filtered;
        if (!heap_buf) writeCount = 64; /* safety cap */
    }

    uint32_t kept = 0;
    uint32_t skipped = 0;
    for (uint32_t w = 0; w < writeCount; w++) {
        uint8_t* ws = (uint8_t*)pWrites + w * WRITE_DESC_SET_SIZE;
        /* Make a mutable copy so we can patch in-place */
        memcpy(out + kept * WRITE_DESC_SET_SIZE, ws, WRITE_DESC_SET_SIZE);
        if (fix_or_check_write(out + kept * WRITE_DESC_SET_SIZE)) {
            kept++;
        } else {
            skipped++;
        }
    }

    if (skipped > 0 && !g_null_guard_logged) {
        LOG("null_guard: skipped %u/%u descriptor writes with unfixable NULL handles\n",
            skipped, writeCount);
        g_null_guard_logged = 1;
    }

    if (kept > 0)
        real_update_desc_sets(real, kept, out, copyCount, pCopies);

    if (heap_buf) free(heap_buf);
}

/* --- vkCreateRenderPass / vkCreateRenderPass2 --- */
typedef VkResult (*PFN_vkCreateRenderPass)(void*, const void*, const void*, uint64_t*);
static PFN_vkCreateRenderPass real_create_render_pass = NULL;

static VkResult trace_CreateRenderPass(void* device, const void* pCreateInfo,
                                        const void* pAllocator, uint64_t* pRenderPass) {
    void* real = unwrap(device);
    VkResult res = real_create_render_pass(real, pCreateInfo, pAllocator, pRenderPass);
    LOG("[D%d] vkCreateRenderPass: dev=%p result=%d rp=0x%llx\n",
        g_device_count, real, res, pRenderPass ? (unsigned long long)*pRenderPass : 0);
    return res;
}

typedef VkResult (*PFN_vkCreateRenderPass2)(void*, const void*, const void*, uint64_t*);
static PFN_vkCreateRenderPass2 real_create_render_pass2 = NULL;

static VkResult trace_CreateRenderPass2(void* device, const void* pCreateInfo,
                                         const void* pAllocator, uint64_t* pRenderPass) {
    void* real = unwrap(device);
    VkResult res = real_create_render_pass2(real, pCreateInfo, pAllocator, pRenderPass);
    LOG("[D%d] vkCreateRenderPass2: dev=%p result=%d rp=0x%llx\n",
        g_device_count, real, res, pRenderPass ? (unsigned long long)*pRenderPass : 0);
    return res;
}

/* --- vkAllocateDescriptorSets --- */
typedef VkResult (*PFN_vkAllocDescSets)(void*, const void*, uint64_t*);
static PFN_vkAllocDescSets real_alloc_desc_sets = NULL;

static VkResult trace_AllocateDescriptorSets(void* device, const void* pAllocInfo,
                                              uint64_t* pDescSets) {
    void* real = unwrap(device);
    /* VkDescriptorSetAllocateInfo: sType(4)+pad(4)+pNext(8)+descriptorPool(8)+descriptorSetCount(4) */
    uint32_t count = 0;
    if (pAllocInfo)
        count = *(const uint32_t*)((const char*)pAllocInfo + 24);
    VkResult res = real_alloc_desc_sets(real, pAllocInfo, pDescSets);
    LOG("[D%d] vkAllocateDescriptorSets: dev=%p count=%u result=%d\n",
        g_device_count, real, count, res);
    return res;
}

/* --- vkCreateDescriptorPool --- */
typedef VkResult (*PFN_vkCreateDescPool)(void*, const void*, const void*, uint64_t*);
static PFN_vkCreateDescPool real_create_desc_pool = NULL;

static VkResult trace_CreateDescriptorPool(void* device, const void* pCreateInfo,
                                            const void* pAllocator, uint64_t* pPool) {
    void* real = unwrap(device);
    VkResult res = real_create_desc_pool(real, pCreateInfo, pAllocator, pPool);
    LOG("[D%d] vkCreateDescriptorPool: dev=%p result=%d pool=0x%llx\n",
        g_device_count, real, res, pPool ? (unsigned long long)*pPool : 0);
    return res;
}

/* ==== Memory requirements patching ====
 *
 * When we add a virtual DEVICE_LOCAL-only type (g_added_type_index >= 0),
 * we must patch memoryTypeBits in all memory requirements queries so DXVK
 * knows it can use our virtual type for allocations.
 *
 * VkMemoryRequirements layout (x86-64):
 *   offset 0:  size (uint64_t)
 *   offset 8:  alignment (uint64_t)
 *   offset 16: memoryTypeBits (uint32_t)
 *
 * VkMemoryRequirements2 wraps it at offset 16 (after sType+pNext).
 */

static void wrapped_GetBufferMemoryRequirements(void* device, uint64_t buffer, void* pReqs) {
    void* real = unwrap(device);
    real_get_buf_mem_reqs(real, buffer, pReqs);
    if (pReqs) {
        uint32_t* bits = (uint32_t*)((uint8_t*)pReqs + 16);
        uint32_t orig = *bits;
        if (g_added_type_index >= 0)
            *bits |= (1u << g_added_type_index);
        LOG("GetBufMemReqs: bits=0x%x -> 0x%x (added_idx=%d)\n", orig, *bits, g_added_type_index);
    }
}

static void wrapped_GetImageMemoryRequirements(void* device, uint64_t image, void* pReqs) {
    void* real = unwrap(device);
    real_get_img_mem_reqs(real, image, pReqs);
    if (pReqs && g_added_type_index >= 0) {
        uint32_t* bits = (uint32_t*)((uint8_t*)pReqs + 16);
        *bits |= (1u << g_added_type_index);
    }
}

typedef void (*PFN_vkGetBufMemReqs2)(void*, const void*, void*);
static PFN_vkGetBufMemReqs2 real_get_buf_mem_reqs2 = NULL;

static void wrapped_GetBufferMemoryRequirements2(void* device, const void* pInfo, void* pReqs) {
    void* real = unwrap(device);
    real_get_buf_mem_reqs2(real, pInfo, pReqs);
    if (pReqs) {
        uint32_t* bits = (uint32_t*)((uint8_t*)pReqs + 32);
        uint32_t orig = *bits;
        if (g_added_type_index >= 0)
            *bits |= (1u << g_added_type_index);
        LOG("GetBufMemReqs2: bits=0x%x -> 0x%x (added_idx=%d)\n", orig, *bits, g_added_type_index);
    }
}

typedef void (*PFN_vkGetImgMemReqs2)(void*, const void*, void*);
static PFN_vkGetImgMemReqs2 real_get_img_mem_reqs2 = NULL;

static void wrapped_GetImageMemoryRequirements2(void* device, const void* pInfo, void* pReqs) {
    void* real = unwrap(device);
    real_get_img_mem_reqs2(real, pInfo, pReqs);
    if (pReqs && g_added_type_index >= 0) {
        uint32_t* bits = (uint32_t*)((uint8_t*)pReqs + 32);
        *bits |= (1u << g_added_type_index);
    }
}

/* Vulkan 1.3: vkGetDeviceBufferMemoryRequirements / vkGetDeviceImageMemoryRequirements
 * Same output as GetXxxMemoryRequirements2 (VkMemoryRequirements2, bits at offset 32).
 * DXVK 2.7+ uses these for initial type mask probes — must patch here too. */

typedef void (*PFN_vkGetDevBufMemReqs)(void*, const void*, void*);
static PFN_vkGetDevBufMemReqs real_get_dev_buf_mem_reqs = NULL;

static void wrapped_GetDeviceBufferMemoryRequirements(void* device, const void* pInfo, void* pReqs) {
    void* real = unwrap(device);
    real_get_dev_buf_mem_reqs(real, pInfo, pReqs);
    if (pReqs) {
        uint32_t* bits = (uint32_t*)((uint8_t*)pReqs + 32);
        uint32_t orig = *bits;
        if (g_added_type_index >= 0)
            *bits |= (1u << g_added_type_index);
        LOG("GetDevBufMemReqs: bits=0x%x -> 0x%x (added_idx=%d)\n", orig, *bits, g_added_type_index);
    }
}

typedef void (*PFN_vkGetDevImgMemReqs)(void*, const void*, void*);
static PFN_vkGetDevImgMemReqs real_get_dev_img_mem_reqs = NULL;

static void wrapped_GetDeviceImageMemoryRequirements(void* device, const void* pInfo, void* pReqs) {
    void* real = unwrap(device);
    real_get_dev_img_mem_reqs(real, pInfo, pReqs);
    if (pReqs) {
        uint32_t* bits = (uint32_t*)((uint8_t*)pReqs + 32);
        uint32_t orig = *bits;
        if (g_added_type_index >= 0)
            *bits |= (1u << g_added_type_index);
        LOG("GetDevImgMemReqs: bits=0x%x -> 0x%x (added_idx=%d)\n", orig, *bits, g_added_type_index);
    }
}

/* ==== Extended Dynamic State (EDS) C wrappers ====
 *
 * These replace raw x86-64 asm trampolines (make_unwrap_trampoline) for all
 * extended dynamic state functions that DXVK uses. FEX JIT may not handle
 * the asm trampolines correctly (mmap RWX, indirect jmp rax), so we use
 * proper C functions that go through FEX's normal JIT path.
 */
/* dyn_real[] and MAX_DYN_WRAPPERS defined earlier (forward declaration for trace_CmdDrawIndexed) */
static int dyn_wrapper_count = 0;

/* === EDS wrappers with secondary CB replay recording ===
 * Each wrapper: unwrap CB, record into replay if secondary, call real driver.
 * This ensures all dynamic state is captured for replay_secondary_into_primary. */

/* Helper: record a uint32_t EDS command for secondary CBs */
static void record_eds_uint(void* cb, int slot, uint32_t v) {
    ReplayCmd* cmd = add_replay_cmd(cb, RCMD_EDS_UINT);
    if (cmd) { cmd->eds_uint.slot = slot; cmd->eds_uint.value = v; }
}
static void record_eds_stencil2(void* cb, int slot, uint32_t face, uint32_t val) {
    ReplayCmd* cmd = add_replay_cmd(cb, RCMD_EDS_STENCIL2);
    if (cmd) { cmd->eds_stencil2.slot = slot; cmd->eds_stencil2.face = face; cmd->eds_stencil2.val = val; }
}

/* Slot 0: vkCmdSetViewportWithCount — log + global save + replay record */
static void unwrap3_0(void* cb, uint32_t n, const void* p) {
    static int vp_log = 0;
    vp_log++;
    if (p && n >= 1) {
        const float* vp = (const float*)p;
        g_last_viewport[0] = vp[0]; g_last_viewport[1] = vp[1];
        g_last_viewport[2] = vp[2]; g_last_viewport[3] = vp[3];
        g_last_viewport[4] = vp[4]; g_last_viewport[5] = vp[5];
        g_last_viewport_set = 1;
        if (vp_log <= 50) {
            LOG("VIEWPORT: n=%u x=%.1f y=%.1f w=%.1f h=%.1f minD=%.3f maxD=%.3f\n",
                n, vp[0], vp[1], vp[2], vp[3], vp[4], vp[5]);
        }
        /* Record for secondary CB replay */
        ReplayCmd* cmd = add_replay_cmd(cb, RCMD_EDS_VIEWPORT);
        if (cmd) { cmd->eds_viewport.slot = 0; cmd->eds_viewport.count = 1;
                    memcpy(cmd->eds_viewport.data, vp, 6 * sizeof(float)); }
    }
    ((void(*)(void*,uint32_t,const void*))dyn_real[0])(unwrap(cb), n, p);
}
/* Slot 1: vkCmdSetScissorWithCount — global save + replay record */
static void unwrap3_1(void* cb, uint32_t n, const void* p) {
    if (p && n >= 1) {
        const uint32_t* sc = (const uint32_t*)p;
        g_last_scissor[0] = sc[0]; g_last_scissor[1] = sc[1];
        g_last_scissor[2] = sc[2]; g_last_scissor[3] = sc[3];
        /* Record for secondary CB replay */
        ReplayCmd* cmd = add_replay_cmd(cb, RCMD_EDS_SCISSOR);
        if (cmd) { cmd->eds_scissor.slot = 1; cmd->eds_scissor.count = 1;
                    memcpy(cmd->eds_scissor.data, sc, 4 * sizeof(uint32_t)); }
    }
    ((void(*)(void*,uint32_t,const void*))dyn_real[1])(unwrap(cb), n, p);
}
/* Slot 2: vkCmdSetDepthBias */
static void unwrap_depthbias_2(void* cb, float a, float b, float c) {
    ReplayCmd* cmd = add_replay_cmd(cb, RCMD_EDS_DEPTHBIAS);
    if (cmd) { cmd->eds_depthbias.a = a; cmd->eds_depthbias.b = b; cmd->eds_depthbias.c = c; }
    ((void(*)(void*,float,float,float))dyn_real[2])(unwrap(cb), a, b, c);
}
/* Slot 3: vkCmdSetBlendConstants */
static void unwrap_blend_3(void* cb, const float* p) {
    ReplayCmd* cmd = add_replay_cmd(cb, RCMD_EDS_BLEND);
    if (cmd && p) { memcpy(cmd->eds_blend.vals, p, 4 * sizeof(float)); }
    ((void(*)(void*,const float*))dyn_real[3])(unwrap(cb), p);
}
/* Slots 4-11,13-14: uint32_t EDS (cull, frontFace, depth*, stencilTest, rasterDiscard, depthBias, topology, primRestart) */
static void unwrap2_4(void* cb, uint32_t v) { record_eds_uint(cb, 4, v); ((void(*)(void*,uint32_t))dyn_real[4])(unwrap(cb), v); }
static void unwrap2_5(void* cb, uint32_t v) { record_eds_uint(cb, 5, v); ((void(*)(void*,uint32_t))dyn_real[5])(unwrap(cb), v); }
static void unwrap2_6(void* cb, uint32_t v) { record_eds_uint(cb, 6, v); ((void(*)(void*,uint32_t))dyn_real[6])(unwrap(cb), v); }
static void unwrap2_7(void* cb, uint32_t v) { record_eds_uint(cb, 7, v); ((void(*)(void*,uint32_t))dyn_real[7])(unwrap(cb), v); }
static void unwrap2_8(void* cb, uint32_t v) { record_eds_uint(cb, 8, v); ((void(*)(void*,uint32_t))dyn_real[8])(unwrap(cb), v); }
static void unwrap2_9(void* cb, uint32_t v) { record_eds_uint(cb, 9, v); ((void(*)(void*,uint32_t))dyn_real[9])(unwrap(cb), v); }
static void unwrap2_10(void* cb, uint32_t v) { record_eds_uint(cb, 10, v); ((void(*)(void*,uint32_t))dyn_real[10])(unwrap(cb), v); }
static void unwrap2_11(void* cb, uint32_t v) { record_eds_uint(cb, 11, v); ((void(*)(void*,uint32_t))dyn_real[11])(unwrap(cb), v); }
static void unwrap2_13(void* cb, uint32_t v) { record_eds_uint(cb, 13, v); ((void(*)(void*,uint32_t))dyn_real[13])(unwrap(cb), v); }
static void unwrap2_14(void* cb, uint32_t v) { record_eds_uint(cb, 14, v); ((void(*)(void*,uint32_t))dyn_real[14])(unwrap(cb), v); }
/* Slot 12: vkCmdSetStencilOp (6 args) */
static void unwrap6_12(void* cb, uint32_t face, uint32_t fail, uint32_t pass, uint32_t dfail, uint32_t cmp) {
    ReplayCmd* cmd = add_replay_cmd(cb, RCMD_EDS_STENCILOP);
    if (cmd) { cmd->eds_stencilop.face = face; cmd->eds_stencilop.fail = fail;
               cmd->eds_stencilop.pass = pass; cmd->eds_stencilop.dfail = dfail; cmd->eds_stencilop.cmp = cmp; }
    ((void(*)(void*,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t))dyn_real[12])(unwrap(cb), face, fail, pass, dfail, cmp);
}
/* Slots 15-17: stencil compare/write mask, reference */
static void unwrap_stencil2_15(void* cb, uint32_t face, uint32_t val) {
    record_eds_stencil2(cb, 15, face, val);
    ((void(*)(void*,uint32_t,uint32_t))dyn_real[15])(unwrap(cb), face, val);
}
static void unwrap_stencil2_16(void* cb, uint32_t face, uint32_t val) {
    record_eds_stencil2(cb, 16, face, val);
    ((void(*)(void*,uint32_t,uint32_t))dyn_real[16])(unwrap(cb), face, val);
}
static void unwrap_stencil2_17(void* cb, uint32_t face, uint32_t val) {
    record_eds_stencil2(cb, 17, face, val);
    ((void(*)(void*,uint32_t,uint32_t))dyn_real[17])(unwrap(cb), face, val);
}

static const struct { const char* name; int slot; PFN_vkVoidFunction wrapper; } eds_table[] = {
    {"vkCmdSetViewportWithCount",            0,  (PFN_vkVoidFunction)unwrap3_0},
    {"vkCmdSetViewportWithCountEXT",         0,  (PFN_vkVoidFunction)unwrap3_0},
    {"vkCmdSetScissorWithCount",             1,  (PFN_vkVoidFunction)unwrap3_1},
    {"vkCmdSetScissorWithCountEXT",          1,  (PFN_vkVoidFunction)unwrap3_1},
    {"vkCmdSetCullMode",                     4,  (PFN_vkVoidFunction)unwrap2_4},
    {"vkCmdSetCullModeEXT",                  4,  (PFN_vkVoidFunction)unwrap2_4},
    {"vkCmdSetFrontFace",                    5,  (PFN_vkVoidFunction)unwrap2_5},
    {"vkCmdSetFrontFaceEXT",                 5,  (PFN_vkVoidFunction)unwrap2_5},
    {"vkCmdSetDepthTestEnable",              6,  (PFN_vkVoidFunction)unwrap2_6},
    {"vkCmdSetDepthTestEnableEXT",           6,  (PFN_vkVoidFunction)unwrap2_6},
    {"vkCmdSetDepthWriteEnable",             7,  (PFN_vkVoidFunction)unwrap2_7},
    {"vkCmdSetDepthWriteEnableEXT",          7,  (PFN_vkVoidFunction)unwrap2_7},
    {"vkCmdSetDepthCompareOp",               8,  (PFN_vkVoidFunction)unwrap2_8},
    {"vkCmdSetDepthCompareOpEXT",            8,  (PFN_vkVoidFunction)unwrap2_8},
    {"vkCmdSetStencilTestEnable",            9,  (PFN_vkVoidFunction)unwrap2_9},
    {"vkCmdSetStencilTestEnableEXT",         9,  (PFN_vkVoidFunction)unwrap2_9},
    {"vkCmdSetStencilOp",                    12, (PFN_vkVoidFunction)unwrap6_12},
    {"vkCmdSetStencilOpEXT",                 12, (PFN_vkVoidFunction)unwrap6_12},
    {"vkCmdSetRasterizerDiscardEnable",      10, (PFN_vkVoidFunction)unwrap2_10},
    {"vkCmdSetRasterizerDiscardEnableEXT",   10, (PFN_vkVoidFunction)unwrap2_10},
    {"vkCmdSetDepthBiasEnable",              11, (PFN_vkVoidFunction)unwrap2_11},
    {"vkCmdSetDepthBiasEnableEXT",           11, (PFN_vkVoidFunction)unwrap2_11},
    {"vkCmdSetPrimitiveTopology",            13, (PFN_vkVoidFunction)unwrap2_13},
    {"vkCmdSetPrimitiveTopologyEXT",         13, (PFN_vkVoidFunction)unwrap2_13},
    {"vkCmdSetPrimitiveRestartEnable",       14, (PFN_vkVoidFunction)unwrap2_14},
    {"vkCmdSetPrimitiveRestartEnableEXT",    14, (PFN_vkVoidFunction)unwrap2_14},
    {"vkCmdSetDepthBias",                    2,  (PFN_vkVoidFunction)unwrap_depthbias_2},
    {"vkCmdSetBlendConstants",               3,  (PFN_vkVoidFunction)unwrap_blend_3},
    {"vkCmdSetStencilCompareMask",           15, (PFN_vkVoidFunction)unwrap_stencil2_15},
    {"vkCmdSetStencilWriteMask",             16, (PFN_vkVoidFunction)unwrap_stencil2_16},
    {"vkCmdSetStencilReference",             17, (PFN_vkVoidFunction)unwrap_stencil2_17},
    {NULL, 0, NULL}
};

/* ==== vkGetDeviceProcAddr: GIPA + thunk GDPA fallback + unwrap trampolines ==== */

static PFN_vkVoidFunction wrapped_GDPA(void* device, const char* pName) {
    if (!pName) return NULL;

    /* Try GIPA first (works for instance-level + some device-level) */
    PFN_vkVoidFunction fn = NULL;
    if (real_gipa && saved_instance)
        fn = real_gipa(saved_instance, pName);
    if (!fn && thunk_lib)
        fn = (PFN_vkVoidFunction)dlsym(thunk_lib, pName);

    /* Fallback: use thunk's real GDPA with unwrapped device handle.
     * The thunk's GIPA doesn't return device-level functions like
     * vkBeginCommandBuffer, vkEndCommandBuffer, etc. The thunk's GDPA
     * needs the real (unwrapped) device handle — passing the wrapper
     * would crash it. */
    if (!fn && real_gdpa && device) {
        void* real_dev = unwrap(device);
        if (real_dev) {
            fn = real_gdpa(real_dev, pName);
            if (fn) LOG("GDPA fallback: %s -> %p (via thunk GDPA)\n", pName, (void*)fn);
        }
    }

    /* CmdPipelineBarrier2 → v1 converter: bypass FEX thunk marshaling of
     * VkDependencyInfo by converting to proven-working v1 barrier call. */
    if (strcmp(pName, "vkCmdPipelineBarrier2") == 0 ||
        strcmp(pName, "vkCmdPipelineBarrier2KHR") == 0) {
        real_cmd_pipeline_barrier2 = (PFN_vkCmdPipelineBarrier2)fn;
        /* Also resolve v1 barrier function for the converter */
        if (!real_cmd_pipeline_barrier_v1) {
            PFN_vkVoidFunction v1fn = NULL;
            if (real_gipa && saved_instance)
                v1fn = real_gipa(saved_instance, "vkCmdPipelineBarrier");
            if (!v1fn && thunk_lib)
                v1fn = (PFN_vkVoidFunction)dlsym(thunk_lib, "vkCmdPipelineBarrier");
            if (!v1fn && real_gdpa && device) {
                void* rd = unwrap(device);
                if (rd) v1fn = real_gdpa(rd, "vkCmdPipelineBarrier");
            }
            real_cmd_pipeline_barrier_v1 = (PFN_vkCmdPipelineBarrierV1)v1fn;
            LOG("GDPA: resolved vkCmdPipelineBarrier v1 -> %p\n", (void*)v1fn);
        }
        LOG("GDPA: %s -> converter (v2->v1, real_v2=%p, real_v1=%p)\n",
            pName, (void*)fn, (void*)real_cmd_pipeline_barrier_v1);
        return (PFN_vkVoidFunction)converter_CmdPipelineBarrier2;
    }

    /* Resolve vkGetDeviceFaultInfoEXT for GPU fault diagnostics */
    if (strcmp(pName, "vkGetDeviceFaultInfoEXT") == 0) {
        if (fn) {
            real_get_device_fault_info = (PFN_vkGetDeviceFaultInfoEXT)fn;
            LOG("GDPA: vkGetDeviceFaultInfoEXT -> %p (resolved for fault diagnostics)\n", (void*)fn);
        }
        return fn ? make_unwrap_trampoline(fn) : NULL;
    }

    /* Block extensions that crash through thunks.
     * Wine checks if vkMapMemory2KHR is non-NULL and uses placed mapping
     * for ALL mappings. Vortek/thunks don't support placed mapping properly.
     * Returning NULL forces Wine to use standard vkMapMemory. */
    if (strcmp(pName, "vkMapMemory2KHR") == 0 ||
        strcmp(pName, "vkUnmapMemory2KHR") == 0) {
        LOG("GDPA: %s -> NULL (blocked: placed memory not supported)\n", pName);
        return NULL;
    }

    /* vkQueueSubmit2: pass-through with handle unwrapping.
     * Vortek natively supports QueueSubmit2 — just unwrap queue + cmdBuf handles. */
    if (strcmp(pName, "vkQueueSubmit2KHR") == 0 ||
        strcmp(pName, "vkQueueSubmit2") == 0) {
        if (fn) {
            real_queue_submit2 = (PFN_vkQueueSubmit2)fn;
            LOG("GDPA: %s -> unwrap wrapper (real=%p)\n", pName, (void*)fn);
            return (PFN_vkVoidFunction)wrapper_QueueSubmit2;
        }
    }

    /* Self-reference */
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0)
        return (PFN_vkVoidFunction)wrapped_GDPA;

    if (!fn) {
        LOG("GDPA: %s -> NULL (unresolved by GIPA+dlsym+GDPA)\n", pName);
        return NULL;
    }

    /* C wrappers for functions needing multi-handle processing */
    if (strcmp(pName, "vkDestroyDevice") == 0) {
        real_destroy_device = (PFN_vkDestroyDevice)fn;
        return (PFN_vkVoidFunction)wrapper_DestroyDevice;
    }
    if (strcmp(pName, "vkGetDeviceQueue") == 0) {
        real_get_device_queue = (PFN_vkGetDeviceQueue)fn;
        return (PFN_vkVoidFunction)wrapper_GetDeviceQueue;
    }
    if (strcmp(pName, "vkGetDeviceQueue2") == 0) {
        real_get_device_queue2 = (PFN_vkGetDeviceQueue2)fn;
        return (PFN_vkVoidFunction)wrapper_GetDeviceQueue2;
    }
    if (strcmp(pName, "vkAllocateCommandBuffers") == 0) {
        real_alloc_cmdbufs = (PFN_vkAllocCmdBufs)fn;
        return (PFN_vkVoidFunction)wrapper_AllocateCommandBuffers;
    }
    if (strcmp(pName, "vkFreeCommandBuffers") == 0) {
        real_free_cmdbufs = (PFN_vkFreeCmdBufs)fn;
        return (PFN_vkVoidFunction)wrapper_FreeCommandBuffers;
    }
    if (strcmp(pName, "vkQueueSubmit") == 0) {
        real_queue_submit = (PFN_vkQueueSubmit)fn;
        return (PFN_vkVoidFunction)wrapper_QueueSubmit;
    }
    if (strcmp(pName, "vkQueueWaitIdle") == 0) {
        real_queue_wait_idle = (PFN_vkQueueWaitIdle)fn;
        return (PFN_vkVoidFunction)wrapper_QueueWaitIdle;
    }
    if (strcmp(pName, "vkCmdExecuteCommands") == 0) {
        real_cmd_exec_cmds = (PFN_vkCmdExecCmds)fn;
        return (PFN_vkVoidFunction)wrapper_CmdExecuteCommands;
    }

    /* Memory requirements patching: add virtual DEVICE_LOCAL type bit */
    if (strcmp(pName, "vkGetBufferMemoryRequirements") == 0) {
        real_get_buf_mem_reqs = (PFN_vkGetBufMemReqs)fn;
        return (PFN_vkVoidFunction)wrapped_GetBufferMemoryRequirements;
    }
    if (strcmp(pName, "vkGetImageMemoryRequirements") == 0) {
        real_get_img_mem_reqs = (PFN_vkGetImgMemReqs)fn;
        return (PFN_vkVoidFunction)wrapped_GetImageMemoryRequirements;
    }
    if (strcmp(pName, "vkGetBufferMemoryRequirements2") == 0 ||
        strcmp(pName, "vkGetBufferMemoryRequirements2KHR") == 0) {
        real_get_buf_mem_reqs2 = (PFN_vkGetBufMemReqs2)fn;
        return (PFN_vkVoidFunction)wrapped_GetBufferMemoryRequirements2;
    }
    if (strcmp(pName, "vkGetImageMemoryRequirements2") == 0 ||
        strcmp(pName, "vkGetImageMemoryRequirements2KHR") == 0) {
        real_get_img_mem_reqs2 = (PFN_vkGetImgMemReqs2)fn;
        return (PFN_vkVoidFunction)wrapped_GetImageMemoryRequirements2;
    }
    /* Vulkan 1.3: vkGetDeviceBufferMemoryRequirements — DXVK 2.7+ uses this for
     * initial type mask probe. Must patch memoryTypeBits here too.
     * Only hook core function (not KHR) to avoid dispatch issues. */
    if (strcmp(pName, "vkGetDeviceBufferMemoryRequirements") == 0) {
        if (fn) {
            real_get_dev_buf_mem_reqs = (PFN_vkGetDevBufMemReqs)fn;
            LOG("GDPA: %s -> wrapped (real=%p)\n", pName, (void*)fn);
            return (PFN_vkVoidFunction)wrapped_GetDeviceBufferMemoryRequirements;
        }
    }
    if (strcmp(pName, "vkGetDeviceImageMemoryRequirements") == 0) {
        if (fn) {
            real_get_dev_img_mem_reqs = (PFN_vkGetDevImgMemReqs)fn;
            LOG("GDPA: %s -> wrapped (real=%p)\n", pName, (void*)fn);
            return (PFN_vkVoidFunction)wrapped_GetDeviceImageMemoryRequirements;
        }
    }

    /* Trace wrappers: log VkResult for key init-time functions.
     * These help diagnose which Vulkan call fails during the
     * second D3D11 device (feat 11_1) initialization. */
    if (strcmp(pName, "vkCreateCommandPool") == 0) {
        real_create_cmd_pool = (PFN_vkCreateCommandPool)fn;
        return (PFN_vkVoidFunction)trace_CreateCommandPool;
    }
    if (strcmp(pName, "vkAllocateMemory") == 0) {
        real_alloc_memory = (PFN_vkAllocateMemory)fn;
        return (PFN_vkVoidFunction)trace_AllocateMemory;
    }
    if (strcmp(pName, "vkCreateBuffer") == 0) {
        real_create_buffer = (PFN_vkCreateBuffer)fn;
        return (PFN_vkVoidFunction)trace_CreateBuffer;
    }
    if (strcmp(pName, "vkCreateImage") == 0) {
        real_create_image = (PFN_vkCreateImage)fn;
        return (PFN_vkVoidFunction)trace_CreateImage;
    }
    if (strcmp(pName, "vkCreateFence") == 0) {
        real_create_fence = (PFN_vkCreateFence)fn;
        return (PFN_vkVoidFunction)trace_CreateFence;
    }
    if (strcmp(pName, "vkCreateSemaphore") == 0) {
        real_create_semaphore = (PFN_vkCreateSemaphoreICD)fn;
        return (PFN_vkVoidFunction)trace_CreateSemaphore;
    }
    if (strcmp(pName, "vkMapMemory") == 0) {
        real_map_memory = (PFN_vkMapMemory)fn;
        return (PFN_vkVoidFunction)trace_MapMemory;
    }
    if (strcmp(pName, "vkUnmapMemory") == 0) {
        real_unmap_memory = (PFN_vkUnmapMemory)fn;
        return (PFN_vkVoidFunction)trace_UnmapMemory;
    }
    /* Capture Invalidate/Flush for cache coherence fix */
    if (strcmp(pName, "vkInvalidateMappedMemoryRanges") == 0) {
        real_invalidate_mapped = (PFN_vkInvalidateMappedMemoryRanges)fn;
        return make_unwrap_trampoline(fn);
    }
    if (strcmp(pName, "vkFlushMappedMemoryRanges") == 0) {
        real_flush_mapped = (PFN_vkFlushMappedMemoryRanges)fn;
        return make_unwrap_trampoline(fn);
    }
    if (strcmp(pName, "vkBindBufferMemory") == 0) {
        real_bind_buf_mem = (PFN_vkBindBufferMemory)fn;
        return (PFN_vkVoidFunction)trace_BindBufferMemory;
    }
    if (strcmp(pName, "vkBindBufferMemory2") == 0 ||
        strcmp(pName, "vkBindBufferMemory2KHR") == 0) {
        real_bind_buf_mem2 = (PFN_vkBindBufferMemory2)fn;
        LOG("GDPA: %s -> trace_BindBufferMemory2 (real=%p)\n", pName, (void*)fn);
        return (PFN_vkVoidFunction)trace_BindBufferMemory2;
    }
    if (strcmp(pName, "vkBindImageMemory") == 0) {
        real_bind_img_mem = (PFN_vkBindImageMemory)fn;
        return (PFN_vkVoidFunction)trace_BindImageMemory;
    }
    if (strcmp(pName, "vkCreateDescriptorSetLayout") == 0) {
        real_create_dsl = (PFN_vkCreateDescSetLayout)fn;
        return (PFN_vkVoidFunction)trace_CreateDescriptorSetLayout;
    }
    if (strcmp(pName, "vkCreatePipelineLayout") == 0) {
        real_create_pl = (PFN_vkCreatePipelineLayout)fn;
        return (PFN_vkVoidFunction)trace_CreatePipelineLayout;
    }
    if (strcmp(pName, "vkBeginCommandBuffer") == 0) {
        real_begin_cmd_buf = (PFN_vkBeginCmdBuf)fn;
        return (PFN_vkVoidFunction)trace_BeginCommandBuffer;
    }
    if (strcmp(pName, "vkEndCommandBuffer") == 0) {
        real_end_cmd_buf = (PFN_vkEndCmdBuf)fn;
        return (PFN_vkVoidFunction)trace_EndCommandBuffer;
    }
    if (strcmp(pName, "vkCreateImageView") == 0) {
        real_create_image_view = (PFN_vkCreateImageView)fn;
        return (PFN_vkVoidFunction)trace_CreateImageView;
    }
    if (strcmp(pName, "vkCreateSampler") == 0) {
        real_create_sampler = (PFN_vkCreateSampler)fn;
        return (PFN_vkVoidFunction)trace_CreateSampler;
    }
    if (strcmp(pName, "vkCreateShaderModule") == 0) {
        real_create_shader_module = (PFN_vkCreateShaderModule)fn;
        return (PFN_vkVoidFunction)trace_CreateShaderModule;
    }
    if (strcmp(pName, "vkDestroyShaderModule") == 0) {
        real_destroy_shader_module = (PFN_vkDestroyShaderModule)fn;
        return fn; /* pass through, no wrapper needed */
    }
    if (strcmp(pName, "vkCreateGraphicsPipelines") == 0) {
        real_create_gfx_pipelines = (PFN_vkCreateGraphicsPipelines)fn;
        return (PFN_vkVoidFunction)trace_CreateGraphicsPipelines;
    }
    if (strcmp(pName, "vkCreateComputePipelines") == 0) {
        real_create_comp_pipelines = (PFN_vkCreateComputePipelines)fn;
        return (PFN_vkVoidFunction)trace_CreateComputePipelines;
    }
    if (strcmp(pName, "vkCreateRenderPass") == 0) {
        real_create_render_pass = (PFN_vkCreateRenderPass)fn;
        return (PFN_vkVoidFunction)trace_CreateRenderPass;
    }
    if (strcmp(pName, "vkCreateRenderPass2") == 0 ||
        strcmp(pName, "vkCreateRenderPass2KHR") == 0) {
        real_create_render_pass2 = (PFN_vkCreateRenderPass2)fn;
        return (PFN_vkVoidFunction)trace_CreateRenderPass2;
    }
    if (strcmp(pName, "vkAllocateDescriptorSets") == 0) {
        real_alloc_desc_sets = (PFN_vkAllocDescSets)fn;
        return (PFN_vkVoidFunction)trace_AllocateDescriptorSets;
    }
    if (strcmp(pName, "vkCreateDescriptorPool") == 0) {
        real_create_desc_pool = (PFN_vkCreateDescPool)fn;
        return (PFN_vkVoidFunction)trace_CreateDescriptorPool;
    }
    if (strcmp(pName, "vkUpdateDescriptorSets") == 0) {
        real_update_desc_sets = (PFN_vkUpdateDescriptorSets)fn;
        LOG("GDPA: vkUpdateDescriptorSets -> null_guard wrapper (real=%p)\n", (void*)fn);
        return (PFN_vkVoidFunction)null_guard_UpdateDescriptorSets;
    }
    if (strcmp(pName, "vkUpdateDescriptorSetWithTemplate") == 0 ||
        strcmp(pName, "vkUpdateDescriptorSetWithTemplateKHR") == 0) {
        real_update_desc_set_with_template = (PFN_vkUpdateDescSetWithTemplate)fn;
        LOG("GDPA: %s -> null_guard template wrapper (real=%p)\n", pName, (void*)fn);
        return (PFN_vkVoidFunction)null_guard_UpdateDescriptorSetWithTemplate;
    }
    if (strcmp(pName, "vkCreateDescriptorUpdateTemplate") == 0 ||
        strcmp(pName, "vkCreateDescriptorUpdateTemplateKHR") == 0) {
        real_create_desc_update_template = (PFN_vkCreateDescUpdateTemplate)fn;
        LOG("GDPA: %s -> template tracker (real=%p)\n", pName, (void*)fn);
        return (PFN_vkVoidFunction)null_guard_CreateDescriptorUpdateTemplate;
    }
    if (strcmp(pName, "vkCreateBufferView") == 0) {
        real_create_buffer_view = (PFN_vkCreateBufferView)fn;
        LOG("GDPA: vkCreateBufferView -> %p (captured for dummy resources)\n", (void*)fn);
        return fn ? make_unwrap_trampoline(fn) : NULL;
    }

    /* Cmd* tracing: log command buffer recording operations
     * Note: CmdPipelineBarrier2 is handled above by the v2→v1 converter */
    if (strcmp(pName, "vkCmdCopyBuffer") == 0) {
        real_cmd_copy_buffer = (PFN_vkCmdCopyBuffer)fn;
        return (PFN_vkVoidFunction)trace_CmdCopyBuffer;
    }
    if (strcmp(pName, "vkCmdCopyBufferToImage") == 0) {
        real_cmd_copy_buf_to_img = (PFN_vkCmdCopyBufToImg)fn;
        return (PFN_vkVoidFunction)trace_CmdCopyBufferToImage;
    }
    /* DISABLED: CopyBufferToImage2 intercept — investigating if it kills HUD rendering.
     * BC data flows through as garbage for now. */
    /*
    if (strcmp(pName, "vkCmdCopyBufferToImage2") == 0 ||
        strcmp(pName, "vkCmdCopyBufferToImage2KHR") == 0) {
        real_cmd_copy_buf_to_img2 = (PFN_vkCmdCopyBufToImg2)fn;
        return (PFN_vkVoidFunction)trace_CmdCopyBufferToImage2;
    }
    */
    if (strcmp(pName, "vkCmdCopyImageToBuffer") == 0) {
        real_cmd_copy_img_to_buf = (PFN_vkCmdCopyImgToBuf)fn;
        return (PFN_vkVoidFunction)trace_CmdCopyImageToBuffer;
    }
    if (strcmp(pName, "vkCmdClearColorImage") == 0) {
        real_cmd_clear_color = (PFN_vkCmdClearColorImage)fn;
        return (PFN_vkVoidFunction)trace_CmdClearColorImage;
    }
    if (strcmp(pName, "vkCmdClearDepthStencilImage") == 0) {
        real_cmd_clear_ds = (PFN_vkCmdClearDSImage)fn;
        return (PFN_vkVoidFunction)trace_CmdClearDepthStencilImage;
    }
    if (strcmp(pName, "vkCmdBeginRendering") == 0 ||
        strcmp(pName, "vkCmdBeginRenderingKHR") == 0) {
        real_cmd_begin_rendering = (PFN_vkCmdBeginRendering)fn;
        return (PFN_vkVoidFunction)trace_CmdBeginRendering;
    }
    if (strcmp(pName, "vkCmdEndRendering") == 0 ||
        strcmp(pName, "vkCmdEndRenderingKHR") == 0) {
        real_cmd_end_rendering = (PFN_vkCmdEndRendering)fn;
        return (PFN_vkVoidFunction)trace_CmdEndRendering;
    }
    if (strcmp(pName, "vkCmdBindPipeline") == 0) {
        real_cmd_bind_pipeline = (PFN_vkCmdBindPipeline)fn;
        return (PFN_vkVoidFunction)trace_CmdBindPipeline;
    }
    if (strcmp(pName, "vkCmdDraw") == 0) {
        real_cmd_draw = (PFN_vkCmdDraw)fn;
        return (PFN_vkVoidFunction)trace_CmdDraw;
    }
    if (strcmp(pName, "vkCmdDrawIndexed") == 0) {
        real_cmd_draw_indexed = (PFN_vkCmdDrawIndexed)fn;
        return (PFN_vkVoidFunction)trace_CmdDrawIndexed;
    }
    if (strcmp(pName, "vkCmdDrawIndirect") == 0) {
        real_cmd_draw_indirect = (PFN_vkCmdDrawIndirect)fn;
        LOG("GDPA: vkCmdDrawIndirect -> proper wrapper (real=%p)\n", (void*)fn);
        return (PFN_vkVoidFunction)trace_CmdDrawIndirect;
    }
    if (strcmp(pName, "vkCmdDrawIndexedIndirect") == 0) {
        real_cmd_draw_indexed_indirect = (PFN_vkCmdDrawIndexedIndirect)fn;
        LOG("GDPA: vkCmdDrawIndexedIndirect -> proper wrapper (real=%p)\n", (void*)fn);
        return (PFN_vkVoidFunction)trace_CmdDrawIndexedIndirect;
    }
    if (strcmp(pName, "vkCmdDispatch") == 0) {
        real_cmd_dispatch = (PFN_vkCmdDispatch)fn;
        return (PFN_vkVoidFunction)trace_CmdDispatch;
    }
    if (strcmp(pName, "vkCmdFillBuffer") == 0) {
        real_cmd_fill_buffer = (PFN_vkCmdFillBuffer)fn;
        return (PFN_vkVoidFunction)trace_CmdFillBuffer;
    }
    if (strcmp(pName, "vkCmdUpdateBuffer") == 0) {
        real_cmd_update_buffer = (PFN_vkCmdUpdateBuffer)fn;
        return (PFN_vkVoidFunction)trace_CmdUpdateBuffer;
    }
    if (strcmp(pName, "vkCmdBindDescriptorSets") == 0) {
        real_cmd_bind_desc_sets = (PFN_vkCmdBindDescSets)fn;
        return (PFN_vkVoidFunction)trace_CmdBindDescriptorSets;
    }
    if (strcmp(pName, "vkCmdSetViewport") == 0) {
        real_cmd_set_viewport = (PFN_vkCmdSetViewport)fn;
        return (PFN_vkVoidFunction)trace_CmdSetViewport;
    }
    if (strcmp(pName, "vkCmdSetScissor") == 0) {
        real_cmd_set_scissor = (PFN_vkCmdSetScissor)fn;
        return (PFN_vkVoidFunction)trace_CmdSetScissor;
    }
    if (strcmp(pName, "vkCmdBindVertexBuffers") == 0) {
        real_cmd_bind_vtx_bufs = (PFN_vkCmdBindVtxBufs)fn;
        return (PFN_vkVoidFunction)trace_CmdBindVertexBuffers;
    }
    if (strcmp(pName, "vkCmdBindVertexBuffers2") == 0 ||
        strcmp(pName, "vkCmdBindVertexBuffers2EXT") == 0) {
        real_cmd_bind_vtx_bufs2 = (PFN_vkCmdBindVtxBufs2)fn;
        LOG("GDPA: %s -> trace_CmdBindVertexBuffers2 (real=%p)\n", pName, (void*)fn);
        /* Also fetch VB1 for downconvert path (DXVK may never request VB1 directly) */
        if (!real_cmd_bind_vtx_bufs && real_gdpa && device) {
            void* real_dev = unwrap(device);
            if (real_dev) {
                PFN_vkVoidFunction vb1 = real_gdpa(real_dev, "vkCmdBindVertexBuffers");
                if (vb1) {
                    real_cmd_bind_vtx_bufs = (PFN_vkCmdBindVtxBufs)vb1;
                    LOG("GDPA: proactively fetched VB1=%p for downconvert\n", (void*)vb1);
                }
            }
        }
        return (PFN_vkVoidFunction)trace_CmdBindVertexBuffers2;
    }
    if (strcmp(pName, "vkCmdBindIndexBuffer") == 0) {
        real_cmd_bind_idx_buf = (PFN_vkCmdBindIdxBuf)fn;
        return (PFN_vkVoidFunction)trace_CmdBindIndexBuffer;
    }
    if (strcmp(pName, "vkCmdBindIndexBuffer2KHR") == 0 ||
        strcmp(pName, "vkCmdBindIndexBuffer2") == 0) {
        /* Try to get the real function; fn may be NULL if driver doesn't support maintenance5 */
        real_cmd_bind_idx_buf2 = fn ? (PFN_vkCmdBindIdxBuf2)fn : NULL;
        LOG("GDPA: %s -> trace_CmdBindIndexBuffer2KHR (real=%p, fallback_IB1=%p)\n",
            pName, (void*)fn, (void*)real_cmd_bind_idx_buf);
        /* Also ensure we have IB1 for fallback */
        if (!real_cmd_bind_idx_buf && real_gdpa && device) {
            void* real_dev = unwrap(device);
            if (real_dev) {
                PFN_vkVoidFunction ib1 = real_gdpa(real_dev, "vkCmdBindIndexBuffer");
                if (ib1) real_cmd_bind_idx_buf = (PFN_vkCmdBindIdxBuf)ib1;
            }
        }
        return (PFN_vkVoidFunction)trace_CmdBindIndexBuffer2KHR;
    }
    if (strcmp(pName, "vkCmdPushConstants") == 0) {
        real_cmd_push_consts = (PFN_vkCmdPushConsts)fn;
        return (PFN_vkVoidFunction)trace_CmdPushConstants;
    }

    /* EDS C wrapper lookup — definitions are at file scope above wrapped_GDPA */
    for (int i = 0; eds_table[i].name; i++) {
        if (strcmp(pName, eds_table[i].name) == 0) {
            int slot = eds_table[i].slot;
            if (!dyn_real[slot]) {
                dyn_real[slot] = fn;
                static int eds_log = 0;
                if (++eds_log <= 30)
                    LOG("GDPA: %s -> C unwrap slot %d (real=%p)\n", pName, slot, (void*)fn);
            }
            return eds_table[i].wrapper;
        }
    }

    /* Fallback: use asm trampoline for remaining unknown functions.
     * WARNING: FEX JIT may not handle these correctly (mmap RWX, indirect jmp). */
    {
        static int tramp_log = 0;
        if (++tramp_log <= 50)
            LOG("GDPA: %s -> ASM TRAMPOLINE (real=%p) [WARNING: may break under FEX]\n", pName, (void*)fn);
    }
    return make_unwrap_trampoline(fn);
}

/* ==== Extension enumeration logging ====
 *
 * Log what extensions Vortek actually reports, so we can compare with
 * what the native Mali driver advertises and identify gaps.
 */

typedef VkResult (*PFN_vkEnumDevExtProps)(void*, const char*, uint32_t*, void*);
static PFN_vkEnumDevExtProps real_enum_dev_ext_props = NULL;
static int ext_logged = 0;

/* VkExtensionProperties: extensionName[256] + specVersion(uint32_t) = 260 bytes */
#define VK_EXT_PROPS_SIZE 260

/* Extensions to HIDE from DXVK — forces fallback to proven Vulkan 1.0/1.1 codepaths.
 *
 * VK_KHR_synchronization2: CmdPipelineBarrier2 + QueueSubmit2 — suspected thunk marshaling bugs
 * VK_KHR_dynamic_rendering: CmdBeginRendering — suspected thunk marshaling bugs
 */
static const char* g_hidden_extensions[] = {
    "VK_KHR_synchronization2",
    "VK_KHR_dynamic_rendering",
    NULL
};
#define NUM_HIDDEN_EXTENSIONS 2

/* Extensions to INJECT — advertise even though Vortek doesn't report them.
 *
 * VK_EXT_robustness2: DXVK unconditionally requires robustBufferAccess2.
 *   Mali-G720 doesn't advertise this extension, but newer DXVK hard-requires
 *   the feature. We inject the extension and spoof the features in GetFeatures2.
 *   robustBufferAccess2 is a safety guarantee (OOB reads return 0, OOB writes
 *   are discarded) — Mali GPUs generally handle this gracefully anyway.
 */
static const char* g_injected_extensions[] = {
    "VK_EXT_robustness2",
    "VK_KHR_maintenance5",
    "VK_KHR_pipeline_library",
    NULL
};
#define NUM_INJECTED_EXTENSIONS 3

static int is_hidden_extension(const char* name) {
    for (int i = 0; g_hidden_extensions[i]; i++) {
        if (strcmp(name, g_hidden_extensions[i]) == 0)
            return 1;
    }
    return 0;
}

static VkResult wrapped_EnumerateDeviceExtensionProperties(
        void* physDev, const char* pLayerName,
        uint32_t* pCount, void* pProps) {
    if (!pProps) {
        /* Count-only: get real count, subtract hidden, add injected */
        VkResult res = real_enum_dev_ext_props(physDev, pLayerName, pCount, NULL);
        if (res == 0 && pCount) {
            int adjusted = (int)*pCount - NUM_HIDDEN_EXTENSIONS + NUM_INJECTED_EXTENSIONS;
            *pCount = (uint32_t)(adjusted > 0 ? adjusted : 0);
        }
        return res;
    }

    /* Fill query: enumerate into our OWN buffer (with padding) to avoid
     * FEX thunk overwriting past the caller's allocation and corrupting
     * the glibc heap. The thunk may use a larger stride than 260 bytes
     * per VkExtensionProperties on the ARM64 host side. */
    uint32_t max_count = *pCount + NUM_HIDDEN_EXTENSIONS + 3;
    size_t buf_size = (size_t)max_count * VK_EXT_PROPS_SIZE + 4096;
    uint8_t* tmp = (uint8_t*)malloc(buf_size);
    if (!tmp) return -1; /* VK_ERROR_OUT_OF_HOST_MEMORY */
    memset(tmp, 0, buf_size);

    uint32_t tmp_count = max_count;
    VkResult res = real_enum_dev_ext_props(physDev, pLayerName, &tmp_count, tmp);
    if (res != 0) {
        free(tmp);
        return res;
    }

    /* Filter from tmp → caller's pProps (hide extensions) */
    uint32_t dst = 0;
    uint32_t limit = *pCount;
    for (uint32_t src = 0; src < tmp_count && dst < limit; src++) {
        const char* name = (const char*)(tmp + src * VK_EXT_PROPS_SIZE);
        if (is_hidden_extension(name)) {
            LOG("EXT FILTER: hiding [%s]\n", name);
            continue;
        }
        memcpy((char*)pProps + dst * VK_EXT_PROPS_SIZE,
               tmp + src * VK_EXT_PROPS_SIZE,
               VK_EXT_PROPS_SIZE);
        dst++;
    }

    /* Inject extensions that Vortek doesn't report but DXVK requires */
    for (int i = 0; g_injected_extensions[i] && dst < limit; i++) {
        uint8_t* entry = (uint8_t*)pProps + dst * VK_EXT_PROPS_SIZE;
        memset(entry, 0, VK_EXT_PROPS_SIZE);
        /* extensionName at offset 0 (char[256]), specVersion at offset 256 (uint32_t) */
        strncpy((char*)entry, g_injected_extensions[i], 255);
        *(uint32_t*)(entry + 256) = 1; /* spec_version = 1 */
        LOG("EXT FILTER: injected [%s]\n", g_injected_extensions[i]);
        dst++;
    }

    *pCount = dst;
    LOG("EXT FILTER: %u -> %u (buf=%u)\n", tmp_count, dst, max_count);

    free(tmp);
    return 0;
}

/* ==== ICD entry points ==== */

__attribute__((visibility("default")))
uint32_t vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pVersion) {
    ensure_init();
    LOG("NegotiateVersion: %u\n", *pVersion);
    if (*pVersion > 5) *pVersion = 5;
    return 0;
}

__attribute__((visibility("default")))
PFN_vkVoidFunction vk_icdGetInstanceProcAddr(void *instance, const char *pName) {
    ensure_init();
    if (!real_gipa || !pName) return NULL;

    if (strcmp(pName, "vkCreateInstance") == 0) {
        real_create_instance = (PFN_vkCreateInstance)real_gipa(instance, pName);
        return (PFN_vkVoidFunction)wrapped_CreateInstance;
    }
    if (strcmp(pName, "vkDestroyInstance") == 0) {
        real_destroy_instance = (PFN_vkDestroyInstance)real_gipa(instance, pName);
        return (PFN_vkVoidFunction)wrapped_DestroyInstance;
    }
    if (strcmp(pName, "vkCreateDevice") == 0) {
        real_create_device = (PFN_vkCreateDevice)real_gipa(instance, pName);
        LOG("GIPA: vkCreateDevice -> %p\n", (void*)real_create_device);
        return (PFN_vkVoidFunction)wrapped_CreateDevice;
    }
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0) {
        LOG("GIPA: vkGetDeviceProcAddr -> wrapped_GDPA\n");
        return (PFN_vkVoidFunction)wrapped_GDPA;
    }
    if (strcmp(pName, "vkGetPhysicalDeviceFormatProperties") == 0) {
        real_get_format_props = (PFN_vkGetPhysDeviceFormatProps)real_gipa(instance, pName);
        LOG("GIPA: vkGetPhysicalDeviceFormatProperties -> BC format wrapper\n");
        return real_get_format_props ? (PFN_vkVoidFunction)wrapped_GetPhysicalDeviceFormatProperties : NULL;
    }
    if (strcmp(pName, "vkGetPhysicalDeviceFormatProperties2") == 0 ||
        strcmp(pName, "vkGetPhysicalDeviceFormatProperties2KHR") == 0) {
        PFN_vkGetPhysDeviceFormatProps2 fn2 = (PFN_vkGetPhysDeviceFormatProps2)real_gipa(instance, pName);
        if (fn2) real_get_format_props2 = fn2;
        LOG("GIPA: %s -> BC format wrapper (thunk=%p)\n", pName, (void*)fn2);
        return real_get_format_props2 ? (PFN_vkVoidFunction)wrapped_GetPhysicalDeviceFormatProperties2 : NULL;
    }
    if (strcmp(pName, "vkGetPhysicalDeviceMemoryProperties") == 0) {
        real_get_mem_props = (PFN_vkGetPhysDeviceMemProps)real_gipa(instance, pName);
        LOG("GIPA: vkGetPhysicalDeviceMemoryProperties -> heap-split wrapper\n");
        return (PFN_vkVoidFunction)wrapped_GetPhysicalDeviceMemoryProperties;
    }
    if (strcmp(pName, "vkGetPhysicalDeviceMemoryProperties2") == 0 ||
        strcmp(pName, "vkGetPhysicalDeviceMemoryProperties2KHR") == 0) {
        PFN_vkGetPhysDeviceMemProps2 fn2 = (PFN_vkGetPhysDeviceMemProps2)real_gipa(instance, pName);
        if (fn2) real_get_mem_props2 = fn2;  /* don't clobber valid ptr with NULL */
        LOG("GIPA: %s -> heap-split wrapper (thunk=%p, stored=%p)\n",
            pName, (void*)fn2, (void*)real_get_mem_props2);
        return real_get_mem_props2 ? (PFN_vkVoidFunction)wrapped_GetPhysicalDeviceMemoryProperties2 : NULL;
    }
    if (strcmp(pName, "vkGetPhysicalDeviceProperties") == 0) {
        real_get_phys_dev_props = (PFN_vkGetPhysDeviceProps)real_gipa(instance, pName);
        LOG("GIPA: vkGetPhysicalDeviceProperties -> apiVersion cap wrapper\n");
        return real_get_phys_dev_props ? (PFN_vkVoidFunction)wrapped_GetPhysicalDeviceProperties : NULL;
    }
    if (strcmp(pName, "vkGetPhysicalDeviceProperties2") == 0 ||
        strcmp(pName, "vkGetPhysicalDeviceProperties2KHR") == 0) {
        PFN_vkGetPhysDeviceProps2 fn2 = (PFN_vkGetPhysDeviceProps2)real_gipa(instance, pName);
        if (fn2) real_get_phys_dev_props2 = fn2;
        LOG("GIPA: %s -> apiVersion cap wrapper (thunk=%p)\n", pName, (void*)fn2);
        return real_get_phys_dev_props2 ? (PFN_vkVoidFunction)wrapped_GetPhysicalDeviceProperties2 : NULL;
    }
    if (strcmp(pName, "vkEnumerateDeviceExtensionProperties") == 0) {
        real_enum_dev_ext_props = (PFN_vkEnumDevExtProps)real_gipa(instance, pName);
        LOG("GIPA: vkEnumerateDeviceExtensionProperties -> no-op wrapper\n");
        return real_enum_dev_ext_props ? (PFN_vkVoidFunction)wrapped_EnumerateDeviceExtensionProperties : NULL;
    }
    if (strcmp(pName, "vkGetPhysicalDeviceFeatures2") == 0 ||
        strcmp(pName, "vkGetPhysicalDeviceFeatures2KHR") == 0) {
        PFN_vkGetPhysDeviceFeatures2 fn2 = (PFN_vkGetPhysDeviceFeatures2)real_gipa(instance, pName);
        if (fn2) real_get_features2 = fn2;  /* don't clobber valid ptr with NULL */
        LOG("GIPA: %s -> diagnostic wrapper (thunk=%p, stored=%p)\n",
            pName, (void*)fn2, (void*)real_get_features2);
        return real_get_features2 ? (PFN_vkVoidFunction)wrapped_GetPhysicalDeviceFeatures2 : NULL;
    }

    return real_gipa(instance, pName);
}

__attribute__((visibility("default")))
void* vk_icdGetPhysicalDeviceProcAddr(void *instance, const char *pName) {
    if (!pName) return NULL;

    /* Return our wrappers for physical device functions we intercept.
     * The loader uses GPDPA as primary dispatch for phys-dev functions.
     * Returning NULL here would skip our GIPA wrappers. */
    if (strcmp(pName, "vkGetPhysicalDeviceFormatProperties") == 0)
        return (void*)vk_icdGetInstanceProcAddr(instance, pName);
    if (strcmp(pName, "vkGetPhysicalDeviceFormatProperties2") == 0 ||
        strcmp(pName, "vkGetPhysicalDeviceFormatProperties2KHR") == 0)
        return (void*)vk_icdGetInstanceProcAddr(instance, pName);
    if (strcmp(pName, "vkGetPhysicalDeviceMemoryProperties") == 0 ||
        strcmp(pName, "vkGetPhysicalDeviceMemoryProperties2") == 0 ||
        strcmp(pName, "vkGetPhysicalDeviceMemoryProperties2KHR") == 0)
        return (void*)vk_icdGetInstanceProcAddr(instance, pName);
    if (strcmp(pName, "vkGetPhysicalDeviceProperties") == 0 ||
        strcmp(pName, "vkGetPhysicalDeviceProperties2") == 0 ||
        strcmp(pName, "vkGetPhysicalDeviceProperties2KHR") == 0)
        return (void*)vk_icdGetInstanceProcAddr(instance, pName);
    if (strcmp(pName, "vkGetPhysicalDeviceFeatures2") == 0 ||
        strcmp(pName, "vkGetPhysicalDeviceFeatures2KHR") == 0)
        return (void*)vk_icdGetInstanceProcAddr(instance, pName);
    if (strcmp(pName, "vkEnumerateDeviceExtensionProperties") == 0)
        return (void*)vk_icdGetInstanceProcAddr(instance, pName);

    return NULL;
}
