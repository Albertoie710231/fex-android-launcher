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
static int g_device_count = 0;  /* track device creation order for tracing */

/* Forward declaration: thunk's real GDPA for device-level function resolution */
typedef PFN_vkVoidFunction (*PFN_vkGetDeviceProcAddr)(void*, const char*);
static PFN_vkGetDeviceProcAddr real_gdpa = NULL;

/* Shared-device: reuse the same real VkDevice for all CreateDevice calls.
 * DXVK's dxvk-submit thread crashes (NULL deref in SRWLOCK release) when
 * two REAL VkDevices coexist under FEX-Emu. By sharing the underlying device,
 * each DXVK D3D11Device gets its own wrapper but they all use the same
 * VkDevice/VkQueue at the thunk level. */
static void* shared_real_device = NULL;
static int device_ref_count = 0;

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
        /* Reject second CreateDevice — shared-device model causes DEVICE_LOST.
         * DXVK creates D2 (feat 11_1 probe) then destroys it; returning -3
         * makes it fall back to D1 only, which is the real rendering device. */
        LOG("CreateDevice #%d REJECTED: already have device, returning -3\n", g_device_count);
        return -3; /* VK_ERROR_INITIALIZATION_FAILED */
    }

    VkResult res = real_create_device(physDev, pCreateInfo, pAllocator, pDevice);
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

/* ---- vkDestroyDevice: unwrap + free wrapper ---- */

typedef void (*PFN_vkDestroyDevice)(void*, const void*);
static PFN_vkDestroyDevice real_destroy_device = NULL;

static void wrapper_DestroyDevice(void* device, const void* pAllocator) {
    if (!device) return;
    void* real = unwrap(device);
    LOG("DestroyDevice: wrapper=%p real=%p\n", device, real);
    if (real_destroy_device) real_destroy_device(real, pAllocator);
    shared_real_device = NULL;
    device_ref_count = 0;
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

    int sn = ++submit_count_global;
    LOG("[D%d] vkQueueSubmit #%d: queue=%p submits=%u cmdBufs=%u\n",
        g_device_count, sn, real_queue, submitCount, total);

    VkResult res = real_queue_submit(real_queue, submitCount, tmp, fence);
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

/* ---- vkQueueSubmit2: unwrap queue + cmdBufs in VkSubmitInfo2 ---- */

/* VkCommandBufferSubmitInfo (32 bytes on x86-64) */
typedef struct {
    uint32_t    sType;          /* 0 */
    uint32_t    _pad0;          /* 4 */
    const void* pNext;          /* 8 */
    void*       commandBuffer;  /* 16 */
    uint32_t    deviceMask;     /* 24 */
    uint32_t    _pad1;          /* 28 */
} ICD_VkCommandBufferSubmitInfo;

/* VkSubmitInfo2 (72 bytes on x86-64) */
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

    if (submitCount == 0 || !pSubmits)
        return real_queue_submit2(real_queue, submitCount, pSubmits, fence);

    /* Count total cmdBufs to unwrap */
    uint32_t total = 0;
    for (uint32_t s = 0; s < submitCount; s++)
        total += pSubmits[s].commandBufferInfoCount;

    if (total == 0)
        return real_queue_submit2(real_queue, submitCount, pSubmits, fence);

    /* Create temp copies with unwrapped cmdBuf handles */
    ICD_VkSubmitInfo2* tmp = (ICD_VkSubmitInfo2*)alloca(
        submitCount * sizeof(ICD_VkSubmitInfo2));
    ICD_VkCommandBufferSubmitInfo* cbs = (ICD_VkCommandBufferSubmitInfo*)alloca(
        total * sizeof(ICD_VkCommandBufferSubmitInfo));
    uint32_t idx = 0;

    for (uint32_t s = 0; s < submitCount; s++) {
        tmp[s] = pSubmits[s];
        if (pSubmits[s].commandBufferInfoCount > 0 && pSubmits[s].pCommandBufferInfos) {
            tmp[s].pCommandBufferInfos = &cbs[idx];
            for (uint32_t c = 0; c < pSubmits[s].commandBufferInfoCount; c++) {
                cbs[idx] = pSubmits[s].pCommandBufferInfos[c];
                cbs[idx].commandBuffer = unwrap(cbs[idx].commandBuffer);
                idx++;
            }
        }
    }

    int sn = ++submit_count_global;
    LOG("[D%d] vkQueueSubmit2 #%d: queue=%p submits=%u cmdBufs=%u\n",
        g_device_count, sn, real_queue, submitCount, total);

    VkResult res = real_queue_submit2(real_queue, submitCount, tmp, fence);
    if (res != 0)
        LOG("[D%d] vkQueueSubmit2 #%d FAILED: %d\n", g_device_count, sn, res);

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

static VkResult trace_AllocateMemory(void* device, const void* pAllocInfo,
                                     const void* pAllocator, uint64_t* pMemory) {
    void* real = unwrap(device);
    /* VkMemoryAllocateInfo: sType(4) + pNext(8) + allocationSize(8) + memoryTypeIndex(4) */
    uint64_t alloc_size = 0;
    uint32_t mem_type = 0;
    if (pAllocInfo) {
        alloc_size = *(const uint64_t*)((const char*)pAllocInfo + 16);
        mem_type = *(const uint32_t*)((const char*)pAllocInfo + 24);
    }
    VkResult res = real_alloc_memory(real, pAllocInfo, pAllocator, pMemory);
    LOG("[D%d] vkAllocateMemory: dev=%p size=%llu type=%u result=%d mem=0x%llx\n",
        g_device_count, real, (unsigned long long)alloc_size, mem_type, res,
        pMemory ? (unsigned long long)*pMemory : 0);
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

static VkResult trace_MapMemory(void* device, uint64_t memory, uint64_t offset,
                                uint64_t size, uint32_t flags, void** ppData) {
    void* real = unwrap(device);
    VkResult res = real_map_memory(real, memory, offset, size, flags, ppData);
    LOG("[D%d] vkMapMemory: dev=%p mem=0x%llx off=%llu sz=%llu result=%d data=%p\n",
        g_device_count, real, (unsigned long long)memory,
        (unsigned long long)offset, (unsigned long long)size,
        res, ppData ? *ppData : NULL);
    return res;
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

/* Trace: vkEndCommandBuffer */
typedef VkResult (*PFN_vkEndCmdBuf)(void*);
static PFN_vkEndCmdBuf real_end_cmd_buf = NULL;

static VkResult trace_EndCommandBuffer(void* cmdBuf) {
    void* real = unwrap(cmdBuf);
    VkResult res = real_end_cmd_buf(real);
    LOG("[D%d] vkEndCommandBuffer: cmdBuf=%p(real=%p) result=%d\n",
        g_device_count, cmdBuf, real, res);
    return res;
}

/* Trace: vkCreateImageView */
typedef VkResult (*PFN_vkCreateImageView)(void*, const void*, const void*, uint64_t*);
static PFN_vkCreateImageView real_create_image_view = NULL;

static VkResult trace_CreateImageView(void* device, const void* pCreateInfo,
                                      const void* pAllocator, uint64_t* pView) {
    void* real = unwrap(device);
    VkResult res = real_create_image_view(real, pCreateInfo, pAllocator, pView);
    LOG("[D%d] vkCreateImageView: dev=%p result=%d view=0x%llx\n",
        g_device_count, real, res, pView ? (unsigned long long)*pView : 0);
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

    /* Block extensions that crash through thunks.
     * Wine checks if vkMapMemory2KHR is non-NULL and uses placed mapping
     * for ALL mappings. Vortek/thunks don't support placed mapping properly.
     * Returning NULL forces Wine to use standard vkMapMemory. */
    if (strcmp(pName, "vkMapMemory2KHR") == 0 ||
        strcmp(pName, "vkUnmapMemory2KHR") == 0) {
        LOG("GDPA: %s -> NULL (blocked: placed memory not supported)\n", pName);
        return NULL;
    }

    /* vkQueueSubmit2 wrapper — unwrap queue + cmdBuf handles in VkSubmitInfo2 */
    if (strcmp(pName, "vkQueueSubmit2KHR") == 0 ||
        strcmp(pName, "vkQueueSubmit2") == 0) {
        if (fn) {
            real_queue_submit2 = (PFN_vkQueueSubmit2)fn;
            return (PFN_vkVoidFunction)wrapper_QueueSubmit2;
        }
        LOG("GDPA: %s -> NULL (not available from ICD)\n", pName);
        return NULL;
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
    if (strcmp(pName, "vkCmdExecuteCommands") == 0) {
        real_cmd_exec_cmds = (PFN_vkCmdExecCmds)fn;
        return (PFN_vkVoidFunction)wrapper_CmdExecuteCommands;
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
