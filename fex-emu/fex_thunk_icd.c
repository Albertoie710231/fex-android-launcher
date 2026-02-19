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

typedef void (*PFN_vkVoidFunction)(void);
typedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddr)(void*, const char*);
typedef int VkResult;

static void* thunk_lib = NULL;
static PFN_vkGetInstanceProcAddr real_gipa = NULL;
static int init_done = 0;
static void* saved_instance = NULL;

#define LOG(...) do { fprintf(stderr, "fex_thunk_icd: " __VA_ARGS__); fflush(stderr); } while(0)

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

static VkResult wrapped_CreateDevice(void* physDev, const void* pCreateInfo,
                                     const void* pAllocator, void** pDevice) {
    if (!real_create_device) return -3;
    VkResult res = real_create_device(physDev, pCreateInfo, pAllocator, pDevice);
    if (res == 0 && pDevice && *pDevice) {
        void* real_device = *pDevice;
        HandleWrapper* w = wrap_handle(real_device);
        if (!w) {
            LOG("CreateDevice: FATAL: wrap_handle failed (OOM)\n");
            /* Can't continue — unwrap trampolines would read wrong offset */
            PFN_vkVoidFunction dfn = real_gipa(saved_instance, "vkDestroyDevice");
            if (dfn) ((void(*)(void*,const void*))dfn)(real_device, pAllocator);
            return -1; /* VK_ERROR_OUT_OF_HOST_MEMORY */
        }
        *pDevice = w;
        LOG("CreateDevice OK: real=%p wrapper=%p\n", real_device, (void*)w);
    }
    return res;
}

/* ---- vkDestroyDevice: unwrap + free wrapper ---- */

typedef void (*PFN_vkDestroyDevice)(void*, const void*);
static PFN_vkDestroyDevice real_destroy_device = NULL;

static void wrapper_DestroyDevice(void* device, const void* pAllocator) {
    if (!device) return;
    void* real = unwrap(device);
    LOG("DestroyDevice: wrapper=%p real=%p\n", device, real);
    if (real_destroy_device) real_destroy_device(real, pAllocator);
    free_wrapper(device);
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

static VkResult wrapper_QueueSubmit(void* queue, uint32_t submitCount,
                                    const ICD_VkSubmitInfo* pSubmits,
                                    uint64_t fence) {
    void* real_queue = unwrap(queue);

    if (submitCount == 0 || !pSubmits)
        return real_queue_submit(real_queue, submitCount, pSubmits, fence);

    /* Count total cmdBufs to unwrap */
    uint32_t total = 0;
    for (uint32_t s = 0; s < submitCount; s++)
        total += pSubmits[s].commandBufferCount;

    if (total == 0)
        return real_queue_submit(real_queue, submitCount, pSubmits, fence);

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

    return real_queue_submit(real_queue, submitCount, tmp, fence);
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

/* ==== vkGetDeviceProcAddr: GIPA-based + unwrap trampolines ==== */

static PFN_vkVoidFunction wrapped_GDPA(void* device, const char* pName) {
    (void)device;
    if (!pName) return NULL;

    /* Use GIPA for all lookups — thunk's GDPA crashes */
    PFN_vkVoidFunction fn = NULL;
    if (real_gipa && saved_instance)
        fn = real_gipa(saved_instance, pName);
    if (!fn && thunk_lib)
        fn = (PFN_vkVoidFunction)dlsym(thunk_lib, pName);

    /* Block extensions that crash through thunks.
     * Wine checks if vkMapMemory2KHR is non-NULL and uses placed mapping
     * for ALL mappings. Vortek/thunks don't support placed mapping properly.
     * Returning NULL forces Wine to use standard vkMapMemory. */
    if (strcmp(pName, "vkMapMemory2KHR") == 0 ||
        strcmp(pName, "vkUnmapMemory2KHR") == 0) {
        LOG("GDPA: %s -> NULL (blocked: placed memory not supported)\n", pName);
        return NULL;
    }

    /* Block vkQueueSubmit2 — VkSubmitInfo2 has nested cmdBuf handles that
     * need unwrapping. Not yet implemented. Wine/DXVK falls back to
     * vkQueueSubmit which we handle properly. */
    if (strcmp(pName, "vkQueueSubmit2KHR") == 0 ||
        strcmp(pName, "vkQueueSubmit2") == 0) {
        LOG("GDPA: %s -> NULL (not yet supported with handle wrappers)\n", pName);
        return NULL;
    }

    /* Self-reference */
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0)
        return (PFN_vkVoidFunction)wrapped_GDPA;

    if (!fn) return NULL;

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
    if (strcmp(pName, "vkCmdExecuteCommands") == 0) {
        real_cmd_exec_cmds = (PFN_vkCmdExecCmds)fn;
        return (PFN_vkVoidFunction)wrapper_CmdExecuteCommands;
    }

    /* All other device/queue/cmdbuf functions: simple unwrap trampoline.
     * The trampoline reads the real handle from wrapper offset 8 and
     * tail-calls the thunk function with all other args preserved. */
    return make_unwrap_trampoline(fn);
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

    return real_gipa(instance, pName);
}

__attribute__((visibility("default")))
void* vk_icdGetPhysicalDeviceProcAddr(void *instance, const char *pName) {
    (void)instance; (void)pName;
    return NULL;
}
