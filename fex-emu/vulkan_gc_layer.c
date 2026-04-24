/*
 * VK_LAYER_VK_GC — Vulkan implicit layer to force periodic GPU-side
 * garbage collection on Mali Vulkan drivers that leak per-resource
 * bookkeeping (visible as growing /dev/mali0 mapping count).
 *
 * Strategy:
 *   - Hook vkQueueSubmit to count submits
 *   - Every VK_GC_INTERVAL submits (default 120 ≈ 2s at 60 fps), call
 *     vkDeviceWaitIdle. This gives the underlying driver a chance to
 *     finalise pending work and release deferred kernel-side state.
 *   - Hook vkAllocateMemory / vkFreeMemory for stats logging.
 *
 * Non-goal: this layer does NOT force-free VkDeviceMemory behind
 * DXVK's back (that would be a use-after-free). We only prod the
 * driver's own cleanup path.
 *
 * Controlled by env var VK_GC_ENABLE=1. Default off → no-op.
 * Tunables:
 *   VK_GC_INTERVAL   submits between waitIdle calls (default 120)
 *   VK_GC_LOG_EVERY  log stats every N submits (default 600 ≈ 10s)
 *
 * Modeled after feature_spoof_layer.c in this same dir.
 *
 * Build: aarch64-linux-android28-clang -shared -fPIC -O2 \
 *        -o libvulkan_gc.so vulkan_gc_layer.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <time.h>

/* Minimal Vulkan typedefs — avoid needing vulkan.h. */
typedef uint32_t VkResult;
typedef void *VkInstance;
typedef void *VkPhysicalDevice;
typedef void *VkDevice;
typedef void *VkQueue;
typedef void *VkDeviceMemory;
typedef void (*PFN_vkVoidFunction)(void);

typedef struct VkLayerInstanceLink_ {
    struct VkLayerInstanceLink_ *pNext;
    PFN_vkVoidFunction (*pfnNextGetInstanceProcAddr)(VkInstance, const char*);
    PFN_vkVoidFunction (*pfnNextGetPhysicalDeviceProcAddr)(VkInstance, const char*);
} VkLayerInstanceLink;

typedef struct {
    int32_t sType;       /* VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO = 47 */
    const void *pNext;
    int32_t function;    /* VK_LAYER_LINK_INFO = 0 */
    union { VkLayerInstanceLink *pLayerInfo; void *_pad; } u;
} VkLayerInstanceCreateInfo;

typedef struct VkLayerDeviceLink_ {
    struct VkLayerDeviceLink_ *pNext;
    PFN_vkVoidFunction (*pfnNextGetInstanceProcAddr)(VkInstance, const char*);
    PFN_vkVoidFunction (*pfnNextGetDeviceProcAddr)(VkDevice, const char*);
} VkLayerDeviceLink;

typedef struct {
    int32_t sType;       /* VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO = 48 */
    const void *pNext;
    int32_t function;    /* VK_LAYER_LINK_INFO = 0 */
    union { VkLayerDeviceLink *pLayerInfo; void *_pad; } u;
} VkLayerDeviceCreateInfo;

typedef struct VkApplicationInfo {
    int32_t sType; const void *pNext;
    const char *pApplicationName; uint32_t applicationVersion;
    const char *pEngineName; uint32_t engineVersion;
    uint32_t apiVersion;
} VkApplicationInfo;

typedef struct VkInstanceCreateInfo {
    int32_t sType; const void *pNext;
    uint32_t flags;
    const VkApplicationInfo *pApplicationInfo;
    uint32_t enabledLayerCount;
    const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char *const *ppEnabledExtensionNames;
} VkInstanceCreateInfo;

typedef struct VkDeviceQueueCreateInfo {
    int32_t sType; const void *pNext;
    uint32_t flags;
    uint32_t queueFamilyIndex;
    uint32_t queueCount;
    const float *pQueuePriorities;
} VkDeviceQueueCreateInfo;

typedef struct VkPhysicalDeviceFeatures_opaque { uint32_t _raw[64]; } VkPhysicalDeviceFeatures_opaque;

typedef struct VkDeviceCreateInfo {
    int32_t sType; const void *pNext;
    uint32_t flags;
    uint32_t queueCreateInfoCount;
    const VkDeviceQueueCreateInfo *pQueueCreateInfos;
    uint32_t enabledLayerCount;
    const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char *const *ppEnabledExtensionNames;
    const VkPhysicalDeviceFeatures_opaque *pEnabledFeatures;
} VkDeviceCreateInfo;

/* Next-layer dispatch pointers. */
static PFN_vkVoidFunction (*next_GetInstanceProcAddr)(VkInstance, const char*) = NULL;
static PFN_vkVoidFunction (*next_GetDeviceProcAddr)(VkDevice, const char*) = NULL;

/* Function-pointer types for what we intercept. */
typedef VkResult (*PFN_vkCreateInstance)(const VkInstanceCreateInfo*, const void*, VkInstance*);
typedef VkResult (*PFN_vkCreateDevice)(VkPhysicalDevice, const VkDeviceCreateInfo*, const void*, VkDevice*);
typedef VkResult (*PFN_vkQueueSubmit)(VkQueue, uint32_t, const void*, void*);
typedef VkResult (*PFN_vkQueueSubmit2)(VkQueue, uint32_t, const void*, void*);
typedef VkResult (*PFN_vkDeviceWaitIdle)(VkDevice);
typedef VkResult (*PFN_vkAllocateMemory)(VkDevice, const void*, const void*, VkDeviceMemory*);
typedef void     (*PFN_vkFreeMemory)(VkDevice, VkDeviceMemory, const void*);

/* Resource class tracking. These hooks let us see which class is the real
 * driver-metadata consumer (40k /dev/mali0 mappings / 100 VkDeviceMemory =
 * ~400 Mali sub-mappings per memory — in per-resource views/samplers/etc). */
typedef void     *VkImageView;
typedef void     *VkSampler;
typedef void     *VkDescriptorPool;
typedef void     *VkDescriptorSet;
typedef void     *VkFramebuffer;
typedef VkResult (*PFN_vkCreateImageView)(VkDevice, const void*, const void*, VkImageView*);
typedef void     (*PFN_vkDestroyImageView)(VkDevice, VkImageView, const void*);
typedef VkResult (*PFN_vkCreateSampler)(VkDevice, const void*, const void*, VkSampler*);
typedef void     (*PFN_vkDestroySampler)(VkDevice, VkSampler, const void*);
typedef VkResult (*PFN_vkCreateFramebuffer)(VkDevice, const void*, const void*, VkFramebuffer*);
typedef void     (*PFN_vkDestroyFramebuffer)(VkDevice, VkFramebuffer, const void*);
typedef VkResult (*PFN_vkAllocateDescriptorSets)(VkDevice, const void*, VkDescriptorSet*);
typedef VkResult (*PFN_vkFreeDescriptorSets)(VkDevice, VkDescriptorPool, uint32_t, const VkDescriptorSet*);
typedef VkResult (*PFN_vkResetDescriptorPool)(VkDevice, VkDescriptorPool, uint32_t);

static PFN_vkCreateDevice         next_CreateDevice = NULL;
static PFN_vkQueueSubmit          next_QueueSubmit = NULL;
static PFN_vkQueueSubmit2         next_QueueSubmit2 = NULL;
static PFN_vkQueueSubmit2         next_QueueSubmit2KHR = NULL;
static PFN_vkDeviceWaitIdle       next_DeviceWaitIdle = NULL;
static PFN_vkAllocateMemory       next_AllocateMemory = NULL;
static PFN_vkFreeMemory           next_FreeMemory = NULL;
static PFN_vkCreateImageView      next_CreateImageView = NULL;
static PFN_vkDestroyImageView     next_DestroyImageView = NULL;
static PFN_vkCreateSampler        next_CreateSampler = NULL;
static PFN_vkDestroySampler       next_DestroySampler = NULL;
static PFN_vkCreateFramebuffer    next_CreateFramebuffer = NULL;
static PFN_vkDestroyFramebuffer   next_DestroyFramebuffer = NULL;
static PFN_vkAllocateDescriptorSets next_AllocateDescriptorSets = NULL;
static PFN_vkFreeDescriptorSets   next_FreeDescriptorSets = NULL;
static PFN_vkResetDescriptorPool  next_ResetDescriptorPool = NULL;

/* Single-device simplification: remember the one VkDevice we wrap. DXVK in
 * this pipeline only creates one. If that ever changes, revisit. */
static VkDevice g_device = NULL;

/* Counters. Atomic because QueueSubmit can be called concurrently by DXVK's
 * multiple worker threads. */
static _Atomic uint64_t g_submit_count = 0;
static _Atomic uint64_t g_alloc_count = 0;
static _Atomic uint64_t g_free_count = 0;
static _Atomic uint64_t g_waitidle_count = 0;
static _Atomic uint64_t g_last_log_submit = 0;
/* Resource class counters. */
static _Atomic uint64_t g_iv_create = 0, g_iv_destroy = 0;
static _Atomic uint64_t g_sampler_create = 0, g_sampler_destroy = 0;
static _Atomic uint64_t g_fb_create = 0, g_fb_destroy = 0;
static _Atomic uint64_t g_ds_alloc = 0, g_ds_free = 0, g_ds_pool_reset = 0;

static int gc_enabled(void) {
    const char *e = getenv("VK_GC_ENABLE");
    return e && e[0] == '1';
}

static uint64_t gc_interval(void) {
    const char *e = getenv("VK_GC_INTERVAL");
    if (!e || !e[0]) return 120;
    long v = strtol(e, NULL, 10);
    return v > 0 ? (uint64_t)v : 120;
}

static uint64_t gc_log_every(void) {
    const char *e = getenv("VK_GC_LOG_EVERY");
    if (!e || !e[0]) return 600;
    long v = strtol(e, NULL, 10);
    return v > 0 ? (uint64_t)v : 600;
}

static void gc_log_stats(const char *tag) {
    uint64_t s = atomic_load(&g_submit_count);
    uint64_t a = atomic_load(&g_alloc_count);
    uint64_t f = atomic_load(&g_free_count);
    uint64_t w = atomic_load(&g_waitidle_count);
    uint64_t iv_c = atomic_load(&g_iv_create), iv_d = atomic_load(&g_iv_destroy);
    uint64_t sm_c = atomic_load(&g_sampler_create), sm_d = atomic_load(&g_sampler_destroy);
    uint64_t fb_c = atomic_load(&g_fb_create), fb_d = atomic_load(&g_fb_destroy);
    uint64_t ds_a = atomic_load(&g_ds_alloc), ds_f = atomic_load(&g_ds_free), ds_r = atomic_load(&g_ds_pool_reset);
    fprintf(stderr,
            "[vk_gc %s] submit=%llu | mem(alloc=%llu free=%llu live=%lld) "
            "| IV(c=%llu d=%llu live=%lld) | Sampler(c=%llu d=%llu live=%lld) "
            "| FB(c=%llu d=%llu live=%lld) | DS(a=%llu f=%llu reset=%llu live=%lld) "
            "| waitIdle=%llu\n",
            tag,
            (unsigned long long)s,
            (unsigned long long)a, (unsigned long long)f, (long long)(a-f),
            (unsigned long long)iv_c, (unsigned long long)iv_d, (long long)(iv_c-iv_d),
            (unsigned long long)sm_c, (unsigned long long)sm_d, (long long)(sm_c-sm_d),
            (unsigned long long)fb_c, (unsigned long long)fb_d, (long long)(fb_c-fb_d),
            (unsigned long long)ds_a, (unsigned long long)ds_f, (unsigned long long)ds_r, (long long)(ds_a-ds_f),
            (unsigned long long)w);
    fflush(stderr);
}

static void gc_maybe_tick(void) {
    if (!gc_enabled()) return;
    uint64_t s = atomic_fetch_add(&g_submit_count, 1) + 1;
    uint64_t interval = gc_interval();
    if (interval > 0 && (s % interval) == 0 && g_device && next_DeviceWaitIdle) {
        next_DeviceWaitIdle(g_device);
        atomic_fetch_add(&g_waitidle_count, 1);
    }
    uint64_t log_every = gc_log_every();
    if (log_every > 0 && (s - atomic_load(&g_last_log_submit)) >= log_every) {
        atomic_store(&g_last_log_submit, s);
        gc_log_stats("periodic");
    }
}

/* -------- interception entry points -------- */

__attribute__((visibility("default")))
VkResult VkGc_QueueSubmit(VkQueue queue, uint32_t submitCount,
                          const void *pSubmits, void *fence) {
    gc_maybe_tick();
    return next_QueueSubmit ? next_QueueSubmit(queue, submitCount, pSubmits, fence) : (VkResult)0;
}

__attribute__((visibility("default")))
VkResult VkGc_QueueSubmit2(VkQueue queue, uint32_t submitCount,
                           const void *pSubmits, void *fence) {
    gc_maybe_tick();
    return next_QueueSubmit2 ? next_QueueSubmit2(queue, submitCount, pSubmits, fence) : (VkResult)0;
}

__attribute__((visibility("default")))
VkResult VkGc_QueueSubmit2KHR(VkQueue queue, uint32_t submitCount,
                              const void *pSubmits, void *fence) {
    gc_maybe_tick();
    return next_QueueSubmit2KHR ? next_QueueSubmit2KHR(queue, submitCount, pSubmits, fence)
                                : (VkResult)0;
}

__attribute__((visibility("default")))
VkResult VkGc_AllocateMemory(VkDevice device, const void *pInfo,
                             const void *pAllocator, VkDeviceMemory *pMem) {
    VkResult r = next_AllocateMemory ? next_AllocateMemory(device, pInfo, pAllocator, pMem)
                                     : (VkResult)-1;
    if (r == 0) atomic_fetch_add(&g_alloc_count, 1);
    return r;
}

__attribute__((visibility("default")))
void VkGc_FreeMemory(VkDevice device, VkDeviceMemory mem, const void *pAllocator) {
    if (next_FreeMemory) next_FreeMemory(device, mem, pAllocator);
    if (mem) atomic_fetch_add(&g_free_count, 1);
}

__attribute__((visibility("default")))
VkResult VkGc_CreateImageView(VkDevice dev, const void *info, const void *alloc, VkImageView *out) {
    VkResult r = next_CreateImageView ? next_CreateImageView(dev, info, alloc, out) : (VkResult)-1;
    if (r == 0) atomic_fetch_add(&g_iv_create, 1);
    return r;
}
__attribute__((visibility("default")))
void VkGc_DestroyImageView(VkDevice dev, VkImageView v, const void *alloc) {
    if (next_DestroyImageView) next_DestroyImageView(dev, v, alloc);
    if (v) atomic_fetch_add(&g_iv_destroy, 1);
}
__attribute__((visibility("default")))
VkResult VkGc_CreateSampler(VkDevice dev, const void *info, const void *alloc, VkSampler *out) {
    VkResult r = next_CreateSampler ? next_CreateSampler(dev, info, alloc, out) : (VkResult)-1;
    if (r == 0) atomic_fetch_add(&g_sampler_create, 1);
    return r;
}
__attribute__((visibility("default")))
void VkGc_DestroySampler(VkDevice dev, VkSampler s, const void *alloc) {
    if (next_DestroySampler) next_DestroySampler(dev, s, alloc);
    if (s) atomic_fetch_add(&g_sampler_destroy, 1);
}
__attribute__((visibility("default")))
VkResult VkGc_CreateFramebuffer(VkDevice dev, const void *info, const void *alloc, VkFramebuffer *out) {
    VkResult r = next_CreateFramebuffer ? next_CreateFramebuffer(dev, info, alloc, out) : (VkResult)-1;
    if (r == 0) atomic_fetch_add(&g_fb_create, 1);
    return r;
}
__attribute__((visibility("default")))
void VkGc_DestroyFramebuffer(VkDevice dev, VkFramebuffer fb, const void *alloc) {
    if (next_DestroyFramebuffer) next_DestroyFramebuffer(dev, fb, alloc);
    if (fb) atomic_fetch_add(&g_fb_destroy, 1);
}
__attribute__((visibility("default")))
VkResult VkGc_AllocateDescriptorSets(VkDevice dev, const void *info, VkDescriptorSet *out) {
    VkResult r = next_AllocateDescriptorSets ? next_AllocateDescriptorSets(dev, info, out) : (VkResult)-1;
    if (r == 0) {
        /* VkDescriptorSetAllocateInfo on 64-bit Linux:
         *   offset  0:  VkStructureType sType      (4 bytes)
         *   offset  4:  <padding>                  (4 bytes)
         *   offset  8:  const void *pNext          (8 bytes)
         *   offset 16:  VkDescriptorPool pool      (8 bytes, opaque handle)
         *   offset 24:  uint32_t descriptorSetCount
         *   offset 32:  const VkDescriptorSetLayout *pSetLayouts
         */
        uint32_t n = *((const uint32_t*)((const char*)info + 24));
        atomic_fetch_add(&g_ds_alloc, n);
    }
    return r;
}
__attribute__((visibility("default")))
VkResult VkGc_FreeDescriptorSets(VkDevice dev, VkDescriptorPool pool, uint32_t n, const VkDescriptorSet *sets) {
    VkResult r = next_FreeDescriptorSets ? next_FreeDescriptorSets(dev, pool, n, sets) : (VkResult)-1;
    if (r == 0) atomic_fetch_add(&g_ds_free, n);
    return r;
}
__attribute__((visibility("default")))
VkResult VkGc_ResetDescriptorPool(VkDevice dev, VkDescriptorPool pool, uint32_t flags) {
    VkResult r = next_ResetDescriptorPool ? next_ResetDescriptorPool(dev, pool, flags) : (VkResult)-1;
    if (r == 0) atomic_fetch_add(&g_ds_pool_reset, 1);
    return r;
}

__attribute__((visibility("default")))
VkResult VkGc_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *pCreateInfo,
                           const void *pAllocator, VkDevice *pDevice) {
    /* Walk the layer chain to locate next GetDeviceProcAddr + next CreateDevice. */
    const VkLayerDeviceCreateInfo *ci = (const VkLayerDeviceCreateInfo*)pCreateInfo->pNext;
    while (ci && !(ci->sType == 48 && ci->function == 0))
        ci = (const VkLayerDeviceCreateInfo*)ci->pNext;
    if (!ci || !ci->u.pLayerInfo) return (VkResult)-3;

    PFN_vkVoidFunction (*next_gipa)(VkInstance, const char*) =
        ci->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkVoidFunction (*next_gdpa)(VkDevice, const char*) =
        ci->u.pLayerInfo->pfnNextGetDeviceProcAddr;

    /* Advance chain so downstream layers see their own next-entry. */
    ((VkLayerDeviceCreateInfo*)ci)->u.pLayerInfo = ci->u.pLayerInfo->pNext;

    PFN_vkCreateDevice createNext = (PFN_vkCreateDevice)next_gipa(NULL, "vkCreateDevice");
    if (!createNext) return (VkResult)-3;
    next_CreateDevice = createNext;

    VkResult r = createNext(pd, pCreateInfo, pAllocator, pDevice);
    if (r != 0) return r;

    /* Capture next layer's device-level function pointers. */
    next_GetDeviceProcAddr = next_gdpa;
    next_QueueSubmit      = (PFN_vkQueueSubmit)next_gdpa(*pDevice, "vkQueueSubmit");
    next_QueueSubmit2     = (PFN_vkQueueSubmit2)next_gdpa(*pDevice, "vkQueueSubmit2");
    next_QueueSubmit2KHR  = (PFN_vkQueueSubmit2)next_gdpa(*pDevice, "vkQueueSubmit2KHR");
    next_DeviceWaitIdle   = (PFN_vkDeviceWaitIdle)next_gdpa(*pDevice, "vkDeviceWaitIdle");
    next_AllocateMemory   = (PFN_vkAllocateMemory)next_gdpa(*pDevice, "vkAllocateMemory");
    next_FreeMemory       = (PFN_vkFreeMemory)next_gdpa(*pDevice, "vkFreeMemory");
    next_CreateImageView  = (PFN_vkCreateImageView)next_gdpa(*pDevice, "vkCreateImageView");
    next_DestroyImageView = (PFN_vkDestroyImageView)next_gdpa(*pDevice, "vkDestroyImageView");
    next_CreateSampler    = (PFN_vkCreateSampler)next_gdpa(*pDevice, "vkCreateSampler");
    next_DestroySampler   = (PFN_vkDestroySampler)next_gdpa(*pDevice, "vkDestroySampler");
    next_CreateFramebuffer= (PFN_vkCreateFramebuffer)next_gdpa(*pDevice, "vkCreateFramebuffer");
    next_DestroyFramebuffer=(PFN_vkDestroyFramebuffer)next_gdpa(*pDevice, "vkDestroyFramebuffer");
    next_AllocateDescriptorSets=(PFN_vkAllocateDescriptorSets)next_gdpa(*pDevice, "vkAllocateDescriptorSets");
    next_FreeDescriptorSets=(PFN_vkFreeDescriptorSets)next_gdpa(*pDevice, "vkFreeDescriptorSets");
    next_ResetDescriptorPool=(PFN_vkResetDescriptorPool)next_gdpa(*pDevice, "vkResetDescriptorPool");

    g_device = *pDevice;

    if (gc_enabled()) {
        fprintf(stderr,
                "[vk_gc] device created. interval=%llu log_every=%llu\n",
                (unsigned long long)gc_interval(),
                (unsigned long long)gc_log_every());
        fflush(stderr);
    }
    return r;
}

__attribute__((visibility("default")))
VkResult VkGc_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                             const void *pAllocator, VkInstance *pInstance) {
    const VkLayerInstanceCreateInfo *ci = (const VkLayerInstanceCreateInfo*)pCreateInfo->pNext;
    while (ci && !(ci->sType == 47 && ci->function == 0))
        ci = (const VkLayerInstanceCreateInfo*)ci->pNext;
    if (!ci || !ci->u.pLayerInfo) return (VkResult)-3;

    PFN_vkVoidFunction (*gipa)(VkInstance, const char*) =
        ci->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    next_GetInstanceProcAddr = gipa;
    ((VkLayerInstanceCreateInfo*)ci)->u.pLayerInfo = ci->u.pLayerInfo->pNext;

    PFN_vkCreateInstance createNext = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    if (!createNext) return (VkResult)-3;

    VkResult r = createNext(pCreateInfo, pAllocator, pInstance);
    if (r == 0 && gc_enabled()) {
        fprintf(stderr, "[vk_gc] instance created, layer active\n");
        fflush(stderr);
    }
    return r;
}

__attribute__((visibility("default")))
PFN_vkVoidFunction VkGc_GetInstanceProcAddr(VkInstance instance, const char *pName) {
    if (!pName) return NULL;
    if (!strcmp(pName, "vkCreateInstance"))      return (PFN_vkVoidFunction)VkGc_CreateInstance;
    if (!strcmp(pName, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)VkGc_GetInstanceProcAddr;
    if (!strcmp(pName, "vkCreateDevice"))        return (PFN_vkVoidFunction)VkGc_CreateDevice;
    if (next_GetInstanceProcAddr) return next_GetInstanceProcAddr(instance, pName);
    return NULL;
}

__attribute__((visibility("default")))
PFN_vkVoidFunction VkGc_GetDeviceProcAddr(VkDevice device, const char *pName) {
    if (!pName) return NULL;
    if (!strcmp(pName, "vkQueueSubmit"))     return (PFN_vkVoidFunction)VkGc_QueueSubmit;
    if (!strcmp(pName, "vkQueueSubmit2"))    return (PFN_vkVoidFunction)VkGc_QueueSubmit2;
    if (!strcmp(pName, "vkQueueSubmit2KHR")) return (PFN_vkVoidFunction)VkGc_QueueSubmit2KHR;
    if (!strcmp(pName, "vkAllocateMemory"))  return (PFN_vkVoidFunction)VkGc_AllocateMemory;
    if (!strcmp(pName, "vkFreeMemory"))      return (PFN_vkVoidFunction)VkGc_FreeMemory;
    if (!strcmp(pName, "vkCreateImageView"))  return (PFN_vkVoidFunction)VkGc_CreateImageView;
    if (!strcmp(pName, "vkDestroyImageView")) return (PFN_vkVoidFunction)VkGc_DestroyImageView;
    if (!strcmp(pName, "vkCreateSampler"))    return (PFN_vkVoidFunction)VkGc_CreateSampler;
    if (!strcmp(pName, "vkDestroySampler"))   return (PFN_vkVoidFunction)VkGc_DestroySampler;
    if (!strcmp(pName, "vkCreateFramebuffer"))return (PFN_vkVoidFunction)VkGc_CreateFramebuffer;
    if (!strcmp(pName, "vkDestroyFramebuffer"))return (PFN_vkVoidFunction)VkGc_DestroyFramebuffer;
    if (!strcmp(pName, "vkAllocateDescriptorSets")) return (PFN_vkVoidFunction)VkGc_AllocateDescriptorSets;
    if (!strcmp(pName, "vkFreeDescriptorSets"))     return (PFN_vkVoidFunction)VkGc_FreeDescriptorSets;
    if (!strcmp(pName, "vkResetDescriptorPool"))    return (PFN_vkVoidFunction)VkGc_ResetDescriptorPool;
    if (!strcmp(pName, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)VkGc_GetDeviceProcAddr;
    if (next_GetDeviceProcAddr) return next_GetDeviceProcAddr(device, pName);
    return NULL;
}

/* Explicit layer entrypoint per the Vulkan loader layer interface. */
typedef struct {
    int32_t sType; /* 73 = LAYER_NEGOTIATE_INTERFACE_STRUCT */
    void *pNext;
    uint32_t loaderLayerInterfaceVersion;
    PFN_vkVoidFunction (*pfnGetInstanceProcAddr)(VkInstance, const char *);
    PFN_vkVoidFunction (*pfnGetDeviceProcAddr)(VkDevice, const char *);
    PFN_vkVoidFunction (*pfnGetPhysicalDeviceProcAddr)(VkInstance, const char *);
} VkNegotiateLayerInterface;

__attribute__((visibility("default")))
VkResult vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v) {
    if (!v) return (VkResult)-3;
    if (v->loaderLayerInterfaceVersion > 2) v->loaderLayerInterfaceVersion = 2;
    v->pfnGetInstanceProcAddr = VkGc_GetInstanceProcAddr;
    v->pfnGetDeviceProcAddr   = VkGc_GetDeviceProcAddr;
    v->pfnGetPhysicalDeviceProcAddr = NULL;
    return 0;
}
