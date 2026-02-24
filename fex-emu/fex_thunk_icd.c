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
#define MAP_BYTE_LIMIT  (512ULL * 1024 * 1024)  /* 512 MiB — no artificial limit */
#define ALLOC_BYTE_CAP  (512ULL * 1024 * 1024)  /* 512 MiB — no artificial limit */

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
             * Add a pure DEVICE_LOCAL type so DXVK can allocate textures
             * from the big heap without the HOST_VISIBLE flag.
             * DXVK prefers non-HOST_VISIBLE types for device images. */
            uint32_t newIdx = *pTypeCount;
            uint32_t origFlags = *(uint32_t*)(p + 4 + first_hv_type * 8);
            uint32_t newFlags = origFlags & ~(0x02 | 0x04 | 0x08);
            *(uint32_t*)(p + 4 + newIdx * 8) = newFlags;
            *(uint32_t*)(p + 4 + newIdx * 8 + 4) = h;  /* original big heap */
            (*pTypeCount)++;

            g_added_type_index = (int)newIdx;
            g_remap_to_type = first_hv_type;

            LOG("HeapSplit: added type[%u] flags=0x%x -> heap %u (%lluMB) [DEVICE_LOCAL only]\n",
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

    /* Serialize queue operations — shared device means shared queue */
    pthread_mutex_lock(&queue_mutex);
    VkResult res = real_queue_submit(real_queue, submitCount, tmp, fence);
    pthread_mutex_unlock(&queue_mutex);
    if (res != 0)
        LOG("[D%d] vkQueueSubmit #%d FAILED: %d\n", g_device_count, sn, res);

    return res;
}

/* ---- vkCmdExecuteCommands: unwrap primary + secondary cmdBufs ---- */

typedef void (*PFN_vkCmdExecCmds)(void*, uint32_t, void* const*);
static PFN_vkCmdExecCmds real_cmd_exec_cmds = NULL;

static void wrapper_CmdExecuteCommands(void* cmdBuf, uint32_t count,
                                       void* const* pSecondary) {
    void* real_cmd = unwrap(cmdBuf);
    void** real_sec = (void**)alloca(count * sizeof(void*));
    for (uint32_t i = 0; i < count; i++)
        real_sec[i] = unwrap((void*)pSecondary[i]);
    real_cmd_exec_cmds(real_cmd, count, real_sec);
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

/* Types 0 and 1 are HOST_VISIBLE (staging heap). Check if a type is HOST_VISIBLE.
 * Our virtual type (g_added_type_index) is DEVICE_LOCAL only — not HOST_VISIBLE. */
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
     * At ~215MB staging, Mali's internal mmap fails and kills CreateImage.
     * By capping at 210MB we leave ~5-10MB VA headroom for images/metadata.
     * DXVK handles -1 by retrying smaller chunk sizes (16→8→4→2→1 MB). */
    if (is_staging_type(real_type) && g_staging_alloc_total + alloc_size > ALLOC_BYTE_CAP) {
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
    VkResult res = real_create_image(real, pCreateInfo, pAllocator, pImage);
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

typedef VkResult (*PFN_vkMapMemory)(void*, uint64_t, uint64_t, uint64_t, uint32_t, void**);
static PFN_vkMapMemory real_map_memory = NULL;
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
    LOG("[D%d] vkBindBufferMemory: dev=%p buf=0x%llx mem=0x%llx result=%d\n",
        g_device_count, real, (unsigned long long)buffer,
        (unsigned long long)memory, res);
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
    VkResult res = real_begin_cmd_buf(real, pBeginInfo);
    LOG("[D%d] vkBeginCommandBuffer: cmdBuf=%p(real=%p) result=%d\n",
        g_device_count, cmdBuf, real, res);
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
     * offset 36: format(4) */
    uint64_t src_image = 0;
    if (pCreateInfo)
        src_image = *(const uint64_t*)((const char*)pCreateInfo + 24);
    VkResult res = real_create_image_view(real, pCreateInfo, pAllocator, pView);
    if (res == 0 && pView) {
        g_iv_track[g_iv_idx % IV_TRACK_MAX].view = *pView;
        g_iv_track[g_iv_idx % IV_TRACK_MAX].image = src_image;
        g_iv_idx++;
    }
    LOG("[D%d] vkCreateImageView: dev=%p img=0x%llx view=0x%llx result=%d\n",
        g_device_count, real, (unsigned long long)src_image,
        pView ? (unsigned long long)*pView : 0, res);
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
    VkResult res = real_create_shader_module(real, pCreateInfo, pAllocator, pModule);
    LOG("[D%d] vkCreateShaderModule: dev=%p result=%d module=0x%llx\n",
        g_device_count, real, res, pModule ? (unsigned long long)*pModule : 0);
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

    if (!pDependencyInfo || !real_cmd_pipeline_barrier_v1) {
        /* Fallback to v2 if no v1 function available */
        if (real_cmd_pipeline_barrier2)
            real_cmd_pipeline_barrier2(real, pDependencyInfo);
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

static void trace_CmdCopyBuffer(void* cmdBuf, uint64_t srcBuf, uint64_t dstBuf,
                                 uint32_t regionCount, const void* pRegions) {
    void* real = unwrap(cmdBuf);
    int op = ++g_cmd_op_count;
    LOG("[CMD#%d] CmdCopyBuffer: cb=%p src=0x%llx dst=0x%llx regions=%u\n",
        op, real, (unsigned long long)srcBuf, (unsigned long long)dstBuf, regionCount);
    real_cmd_copy_buffer(real, srcBuf, dstBuf, regionCount, pRegions);
}

/* --- CmdCopyBufferToImage --- */
typedef void (*PFN_vkCmdCopyBufToImg)(void*, uint64_t, uint64_t, uint32_t, uint32_t, const void*);
static PFN_vkCmdCopyBufToImg real_cmd_copy_buf_to_img = NULL;

static void trace_CmdCopyBufferToImage(void* cmdBuf, uint64_t buffer, uint64_t image,
                                         uint32_t imageLayout, uint32_t regionCount,
                                         const void* pRegions) {
    void* real = unwrap(cmdBuf);
    int op = ++g_cmd_op_count;
    LOG("[CMD#%d] CmdCopyBufferToImage: cb=%p buf=0x%llx img=0x%llx layout=%u regions=%u\n",
        op, real, (unsigned long long)buffer, (unsigned long long)image,
        imageLayout, regionCount);
    real_cmd_copy_buf_to_img(real, buffer, image, imageLayout, regionCount, pRegions);
}

/* Forward declarations for CmdCopyImageToBuffer + CmdEndRendering diagnostics */
typedef void (*PFN_vkCmdClearColorImage)(void*, uint64_t, uint32_t, const void*, uint32_t, const void*);
static PFN_vkCmdClearColorImage real_cmd_clear_color = NULL;
static uint64_t g_last_render_image = 0;

/* --- CmdCopyImageToBuffer --- */
typedef void (*PFN_vkCmdCopyImgToBuf)(void*, uint64_t, uint32_t, uint64_t, uint32_t, const void*);
static PFN_vkCmdCopyImgToBuf real_cmd_copy_img_to_buf = NULL;

/* Diagnostic: inject CmdClearColorImage RED before CopyImageToBuffer
 * to verify the copy pipeline works. If staging reads red, the pipeline
 * works but DXVK renders black. If staging reads zero, pipeline is broken. */
static int g_citb_diag_done = 0;
static void* g_last_render_cb = NULL; /* wrapped CB handle that last called CmdBeginRendering */

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
    real_cmd_bind_pipeline(real, bindPoint, pipeline);
}

/* --- CmdDraw --- */
typedef void (*PFN_vkCmdDraw)(void*, uint32_t, uint32_t, uint32_t, uint32_t);
static PFN_vkCmdDraw real_cmd_draw = NULL;

static void trace_CmdDraw(void* cmdBuf, uint32_t vertexCount, uint32_t instanceCount,
                           uint32_t firstVertex, uint32_t firstInstance) {
    void* real = unwrap(cmdBuf);
    int op = ++g_cmd_op_count;
    LOG("[CMD#%d] CmdDraw: cb=%p verts=%u inst=%u\n",
        op, real, vertexCount, instanceCount);
    real_cmd_draw(real, vertexCount, instanceCount, firstVertex, firstInstance);
}

/* --- CmdDrawIndexed --- */
typedef void (*PFN_vkCmdDrawIndexed)(void*, uint32_t, uint32_t, uint32_t, int32_t, uint32_t);
static PFN_vkCmdDrawIndexed real_cmd_draw_indexed = NULL;

static void trace_CmdDrawIndexed(void* cmdBuf, uint32_t indexCount, uint32_t instanceCount,
                                  uint32_t firstIndex, int32_t vertexOffset,
                                  uint32_t firstInstance) {
    void* real = unwrap(cmdBuf);
    int op = ++g_cmd_op_count;
    LOG("[CMD#%d] CmdDrawIndexed: cb=%p indices=%u inst=%u\n",
        op, real, indexCount, instanceCount);
    real_cmd_draw_indexed(real, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
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
    LOG("[CMD#%d] CmdBindDescriptorSets: cb=%p bindPoint=%u sets=%u dynOffs=%u\n",
        op, real, bindPoint, setCount, dynOffCount);
    real_cmd_bind_desc_sets(real, bindPoint, layout, firstSet, setCount, pSets, dynOffCount, pDynOffs);
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

/* --- CmdBindIndexBuffer --- */
typedef void (*PFN_vkCmdBindIdxBuf)(void*, uint64_t, uint64_t, uint32_t);
static PFN_vkCmdBindIdxBuf real_cmd_bind_idx_buf = NULL;

static void trace_CmdBindIndexBuffer(void* cmdBuf, uint64_t buffer, uint64_t offset, uint32_t indexType) {
    void* real = unwrap(cmdBuf);
    int op = ++g_cmd_op_count;
    LOG("[CMD#%d] CmdBindIndexBuffer: cb=%p buf=0x%llx type=%u\n",
        op, real, (unsigned long long)buffer, indexType);
    real_cmd_bind_idx_buf(real, buffer, offset, indexType);
}

/* --- CmdPushConstants --- */
typedef void (*PFN_vkCmdPushConsts)(void*, uint64_t, uint32_t, uint32_t, uint32_t, const void*);
static PFN_vkCmdPushConsts real_cmd_push_consts = NULL;

static void trace_CmdPushConstants(void* cmdBuf, uint64_t layout, uint32_t stageFlags,
                                    uint32_t offset, uint32_t size, const void* pValues) {
    void* real = unwrap(cmdBuf);
    int op = ++g_cmd_op_count;
    LOG("[CMD#%d] CmdPushConstants: cb=%p stages=0x%x off=%u size=%u\n",
        op, real, stageFlags, offset, size);
    real_cmd_push_consts(real, layout, stageFlags, offset, size, pValues);
}

/* --- vkCreateGraphicsPipelines --- */
typedef VkResult (*PFN_vkCreateGraphicsPipelines)(void*, uint64_t, uint32_t, const void*, const void*, uint64_t*);
static PFN_vkCreateGraphicsPipelines real_create_gfx_pipelines = NULL;

typedef void (*PFN_vkDestroyShaderModule)(void*, uint64_t, const void*);
static PFN_vkDestroyShaderModule real_destroy_shader_module = NULL;

/* Convert inline VkShaderModuleCreateInfo (maintenance5) to real VkShaderModule.
 * Vortek's IPC can't serialize pNext chains on shader stages, so we pre-create
 * the modules and patch the stage to use them.
 * Returns number of temp modules created; caller must destroy them after pipeline creation.
 */
#define MAX_TEMP_MODULES 32
static uint32_t fixup_inline_shaders(void* real_device, const void* pCreateInfos,
                                      uint32_t pipe_count, uint64_t* temp_modules) {
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
                    VkResult r = real_create_shader_module(real_device, (const void*)pn, NULL, &new_module);
                    if (r == 0 && new_module) {
                        *pModule = new_module;
                        temp_modules[n_temp++] = new_module;
                        uint64_t codeSize = *(uint64_t*)((uint8_t*)pn + 24);
                        LOG("  inline->module: stage[%u] codeSize=%lu module=0x%lx\n",
                            s, (unsigned long)codeSize, (unsigned long)new_module);
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
        void* pColorBlendState = *(void**)(ci + 88);
        uint64_t renderPass = *(uint64_t*)(ci + 112);

        LOG("[D%d] GfxPipe[%u]: stages=%u renderPass=0x%lx\n",
            g_device_count, i, stageCount, (unsigned long)renderPass);

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

    /* Convert inline shaders to real VkShaderModule objects for Vortek compatibility */
    uint64_t temp_modules[MAX_TEMP_MODULES];
    uint32_t n_temp = 0;
    if (real_create_shader_module) {
        n_temp = fixup_inline_shaders(real, pCreateInfos, count, temp_modules);
        if (n_temp > 0) LOG("  created %u temp shader modules\n", n_temp);
    }

    VkResult res = real_create_gfx_pipelines(real, cache, count, pCreateInfos, pAllocator, pPipelines);
    LOG("[D%d] vkCreateGraphicsPipelines: dev=%p count=%u result=%d\n",
        g_device_count, real, count, res);
    if (res != 0) {
        LOG("[D%d] *** CreateGraphicsPipelines FAILED: count=%u result=%d ***\n",
            g_device_count, count, res);
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
    if (!real_alloc_memory)
        real_alloc_memory = (PFN_vkAllocateMemory)resolve_dev_fn(real_device, "vkAllocateMemory");
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

static void null_guard_UpdateDescriptorSets(void* device, uint32_t writeCount,
                                             const void* pWrites,
                                             uint32_t copyCount, const void* pCopies) {
    void* real = unwrap(device);

    /* Lazily init dummy resources on first call */
    if (!g_dummies_init) create_dummy_resources(real);

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
    if (strcmp(pName, "vkCmdBindIndexBuffer") == 0) {
        real_cmd_bind_idx_buf = (PFN_vkCmdBindIdxBuf)fn;
        return (PFN_vkVoidFunction)trace_CmdBindIndexBuffer;
    }
    if (strcmp(pName, "vkCmdPushConstants") == 0) {
        real_cmd_push_consts = (PFN_vkCmdPushConsts)fn;
        return (PFN_vkVoidFunction)trace_CmdPushConstants;
    }

    /* All other device/queue/cmdbuf functions: simple unwrap trampoline.
     * The trampoline reads the real handle from wrapper offset 8 and
     * tail-calls the thunk function with all other args preserved. */
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
    (void)instance; (void)pName;
    return NULL;
}
