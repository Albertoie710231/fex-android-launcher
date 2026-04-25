/*
 * VK_LAYER_MALI_ALLOC_PROBE — descriptor-set-allocation hypothesis test.
 *
 * The PC vs tablet DXVK comparison (project_pc_vs_tablet_dxvk_comparison_2026_04_24.md)
 * showed 234x more vkAllocateDescriptorSets calls per submit on the Mali wrapper than
 * on PC (234 vs 0.07). DXVK already recycles descriptor *pools*, so the cost should
 * be near-zero per spec — but we're slow anyway. Hypothesis: the leegao Mali wrapper
 * (or the kbase driver under it) does an mmap of /dev/mali0 inside vkAllocateDescriptorSets
 * for each set, blowing up VMA count and kernel page-table walk cost.
 *
 * This layer hooks four entrypoints:
 *   vkCreateDescriptorPool       — count pool creates
 *   vkResetDescriptorPool        — count pool resets
 *   vkAllocateDescriptorSets     — count + sample mali0 VMA delta per call
 *   vkFreeDescriptorSets         — count
 *
 * Around every Nth vkAllocateDescriptorSets call we snapshot /proc/self/maps and
 * count lines containing "/dev/mali0". The delta tells us whether each allocate
 * actually mmaps a new region. If it does, then pooling/recycling at the
 * descriptor-set level (rather than just pool level, like DXVK already does) will
 * reduce VMA pressure dramatically. If it doesn't, the slowness is somewhere else
 * (CPU work in the wrapper, sync, etc.) and the dedup direction stays dead.
 *
 * Build:
 *   ~/Android/Sdk/ndk/27.2.12479018/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang \
 *     -shared -fPIC -O2 -o libmali_alloc_probe.so mali_alloc_probe_layer.c
 *
 * Activate via env: MALI_ALLOC_PROBE=1
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <unistd.h>

typedef int VkResult;
typedef void *VkInstance;
typedef void *VkDevice;
typedef void *VkPhysicalDevice;
typedef void *VkDescriptorPool;
typedef void *VkDescriptorSet;
typedef void *VkDescriptorSetLayout;

typedef struct {
    int32_t       sType;
    const void   *pNext;
    VkDescriptorPool descriptorPool;
    uint32_t      descriptorSetCount;
    const VkDescriptorSetLayout *pSetLayouts;
} VkDescriptorSetAllocateInfo;

typedef void (*PFN_vkVoidFunction)(void);
typedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddr)(VkInstance, const char*);
typedef PFN_vkVoidFunction (*PFN_vkGetDeviceProcAddr)(VkDevice, const char*);
typedef VkResult (*PFN_vkCreateInstance)(const void*, const void*, VkInstance*);
typedef VkResult (*PFN_vkCreateDevice)(VkPhysicalDevice, const void*, const void*, VkDevice*);
typedef VkResult (*PFN_vkAllocateDescriptorSets)(VkDevice, const VkDescriptorSetAllocateInfo*, VkDescriptorSet*);
typedef VkResult (*PFN_vkFreeDescriptorSets)(VkDevice, VkDescriptorPool, uint32_t, const VkDescriptorSet*);
typedef VkResult (*PFN_vkCreateDescriptorPool)(VkDevice, const void*, const void*, VkDescriptorPool*);

/* Mirror of VkDescriptorPoolCreateInfo + VkDescriptorPoolSize so we can dump
 * the parameters DXVK is asking for on each vkCreateDescriptorPool call.
 * Layout (LP64): sType(4) pad(4) pNext(8) flags(4) maxSets(4) poolSizeCount(4) pad(4) pPoolSizes(8). */
typedef struct VkDescriptorPoolSize {
    int32_t  type;
    uint32_t descriptorCount;
} VkDescriptorPoolSize;

typedef struct VkDescriptorPoolCreateInfo {
    int32_t  sType;
    int32_t  _pad0;
    const void *pNext;
    uint32_t flags;
    uint32_t maxSets;
    uint32_t poolSizeCount;
    uint32_t _pad1;
    const VkDescriptorPoolSize *pPoolSizes;
} VkDescriptorPoolCreateInfo;
typedef VkResult (*PFN_vkResetDescriptorPool)(VkDevice, VkDescriptorPool, uint32_t);
typedef void     (*PFN_vkDestroyDescriptorPool)(VkDevice, VkDescriptorPool, const void*);

typedef struct VkLayerDeviceLink_ {
    struct VkLayerDeviceLink_ *pNext;
    PFN_vkVoidFunction (*pfnNextGetInstanceProcAddr)(VkInstance, const char*);
    PFN_vkVoidFunction (*pfnNextGetDeviceProcAddr)(VkDevice, const char*);
} VkLayerDeviceLink;

static PFN_vkVoidFunction (*next_GetInstanceProcAddr)(VkInstance, const char*) = NULL;
static PFN_vkVoidFunction (*next_GetDeviceProcAddr)(VkDevice, const char*) = NULL;

static PFN_vkAllocateDescriptorSets next_AllocateDescriptorSets = NULL;
static PFN_vkFreeDescriptorSets     next_FreeDescriptorSets     = NULL;
static PFN_vkCreateDescriptorPool   next_CreateDescriptorPool   = NULL;
static PFN_vkResetDescriptorPool    next_ResetDescriptorPool    = NULL;
static PFN_vkDestroyDescriptorPool  next_DestroyDescriptorPool  = NULL;

static int probe_enabled(void) {
    const char *e = getenv("MALI_ALLOC_PROBE");
    return e && e[0] == '1';
}

static _Atomic uint64_t pool_creates    = 0;
static _Atomic uint64_t pool_resets     = 0;
static _Atomic uint64_t pool_destroys   = 0;
static _Atomic uint64_t set_allocs      = 0;       /* number of vkAllocateDescriptorSets calls */
static _Atomic uint64_t sets_allocated  = 0;       /* total descriptorSetCount summed */
static _Atomic uint64_t set_frees       = 0;
static _Atomic uint64_t alloc_failures  = 0;

/* Mali VMA snapshot state. */
static _Atomic uint64_t last_mali_count = 0;       /* last /proc/self/maps mali0 count */
static _Atomic uint64_t mali_growth_cum = 0;       /* sum of positive deltas across samples */
static _Atomic uint64_t sample_count    = 0;
static _Atomic uint64_t alloc_call_at_last_sample = 0;

#define ALLOC_LOG_EVERY  1000   /* sample mali0 VMA every N alloc calls */

/* Counts how many lines in /proc/self/maps reference "/dev/mali0". Cheap
 * enough to call every Nth allocation when N is in the thousands. */
static uint64_t count_mali_vmas(void) {
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd < 0) return 0;
    char buf[8192];
    uint64_t count = 0;
    /* /proc/self/maps line ends in newline; we want to count occurrences
     * of the substring "/dev/mali0". The substring is short enough that
     * we just keep an 11-byte rolling tail across reads. */
    char tail[16] = {0};
    int tail_len = 0;
    static const char needle[] = "/dev/mali0";
    const int needle_len = (int)sizeof(needle) - 1;

    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        /* Scan a virtual buffer = tail + buf. */
        char *scan = buf;
        ssize_t scan_n = n;
        if (tail_len > 0) {
            /* Build a tiny search window from tail tail|buf[0..needle_len-1]. */
            char win[32];
            int copy_from_buf = needle_len - 1;
            if (copy_from_buf > scan_n) copy_from_buf = (int)scan_n;
            memcpy(win, tail, tail_len);
            memcpy(win + tail_len, buf, copy_from_buf);
            int win_len = tail_len + copy_from_buf;
            for (int i = 0; i + needle_len <= win_len; i++) {
                if (memcmp(win + i, needle, needle_len) == 0) count++;
            }
        }
        for (ssize_t i = 0; i + needle_len <= scan_n; i++) {
            if (scan[i] == '/' && memcmp(scan + i, needle, needle_len) == 0) count++;
        }
        /* Save tail for next iteration. */
        int tail_take = needle_len - 1;
        if (tail_take > scan_n) tail_take = (int)scan_n;
        memcpy(tail, scan + scan_n - tail_take, tail_take);
        tail_len = tail_take;
    }
    close(fd);
    return count;
}

static void maybe_sample(uint64_t alloc_call_id, uint32_t descriptorSetCount) {
    if (alloc_call_id % ALLOC_LOG_EVERY != 0) return;

    uint64_t mali_now = count_mali_vmas();
    uint64_t prev = atomic_exchange_explicit(&last_mali_count, mali_now, memory_order_relaxed);
    uint64_t prev_call = atomic_exchange_explicit(&alloc_call_at_last_sample, alloc_call_id, memory_order_relaxed);
    int64_t delta_mali = (int64_t)mali_now - (int64_t)prev;
    int64_t delta_calls = (int64_t)alloc_call_id - (int64_t)prev_call;
    if (delta_mali > 0)
        atomic_fetch_add_explicit(&mali_growth_cum, (uint64_t)delta_mali, memory_order_relaxed);

    uint64_t pc = atomic_load_explicit(&pool_creates,   memory_order_relaxed);
    uint64_t pr = atomic_load_explicit(&pool_resets,    memory_order_relaxed);
    uint64_t pd = atomic_load_explicit(&pool_destroys,  memory_order_relaxed);
    uint64_t sa = atomic_load_explicit(&set_allocs,     memory_order_relaxed);
    uint64_t sn = atomic_load_explicit(&sets_allocated, memory_order_relaxed);
    uint64_t sf = atomic_load_explicit(&set_frees,      memory_order_relaxed);
    uint64_t af = atomic_load_explicit(&alloc_failures, memory_order_relaxed);
    uint64_t sc = atomic_fetch_add_explicit(&sample_count, 1, memory_order_relaxed) + 1;

    double mali_per_call = delta_calls > 0
        ? (double)delta_mali / (double)delta_calls : 0.0;

    fprintf(stderr,
            "[mali_alloc_probe] sample=%llu pools(c=%llu r=%llu d=%llu) "
            "sets(allocs=%llu count=%llu frees=%llu fail=%llu setsPerAlloc=%.2f) "
            "mali0_vmas=%llu (+%lld since last, +%llu cum) "
            "mali_per_alloc_call=%.3f thisAlloc=%u\n",
            (unsigned long long)sc,
            (unsigned long long)pc,
            (unsigned long long)pr,
            (unsigned long long)pd,
            (unsigned long long)sa,
            (unsigned long long)sn,
            (unsigned long long)sf,
            (unsigned long long)af,
            sa ? (double)sn / (double)sa : 0.0,
            (unsigned long long)mali_now,
            (long long)delta_mali,
            (unsigned long long)atomic_load_explicit(&mali_growth_cum, memory_order_relaxed),
            mali_per_call,
            descriptorSetCount);
    fflush(stderr);
}

__attribute__((visibility("default")))
VkResult MaliProbe_AllocateDescriptorSets(
    VkDevice device, const VkDescriptorSetAllocateInfo *pInfo, VkDescriptorSet *pSets) {

    VkResult r = next_AllocateDescriptorSets(device, pInfo, pSets);

    if (!probe_enabled()) return r;

    uint64_t call_id = atomic_fetch_add_explicit(&set_allocs, 1, memory_order_relaxed) + 1;
    if (pInfo)
        atomic_fetch_add_explicit(&sets_allocated, pInfo->descriptorSetCount, memory_order_relaxed);
    if (r != 0)
        atomic_fetch_add_explicit(&alloc_failures, 1, memory_order_relaxed);

    maybe_sample(call_id, pInfo ? pInfo->descriptorSetCount : 0);
    return r;
}

__attribute__((visibility("default")))
VkResult MaliProbe_FreeDescriptorSets(
    VkDevice device, VkDescriptorPool pool, uint32_t count, const VkDescriptorSet *pSets) {
    if (probe_enabled())
        atomic_fetch_add_explicit(&set_frees, count, memory_order_relaxed);
    return next_FreeDescriptorSets(device, pool, count, pSets);
}

__attribute__((visibility("default")))
VkResult MaliProbe_CreateDescriptorPool(
    VkDevice device, const void *pCreateInfo, const void *pAllocator, VkDescriptorPool *pPool) {
    VkResult r = next_CreateDescriptorPool(device, pCreateInfo, pAllocator, pPool);
    if (probe_enabled() && r == 0) {
        uint64_t which = atomic_fetch_add_explicit(&pool_creates, 1, memory_order_relaxed) + 1;
        /* Log full create-info on the first 5 pools plus every 5000th. The
         * first few tell us whether DXVK is feeding maxSets=8192 (patch live)
         * vs maxSets=2048 (baseline). The periodic samples reveal whether
         * all pools have the same shape or if Sekiro creates differently
         * sized pools (e.g. one large + many small per layout). */
        if (which <= 5 || (which % 5000) == 0) {
            const VkDescriptorPoolCreateInfo *ci = (const VkDescriptorPoolCreateInfo *)pCreateInfo;
            if (ci) {
                fprintf(stderr,
                        "[mali_alloc_probe] poolCreate#%llu maxSets=%u poolSizeCount=%u flags=0x%x\n",
                        (unsigned long long)which, ci->maxSets, ci->poolSizeCount, ci->flags);
                uint32_t n = ci->poolSizeCount;
                if (n > 16) n = 16;
                for (uint32_t i = 0; i < n; i++) {
                    fprintf(stderr,
                            "  pool#%llu  type=%d count=%u\n",
                            (unsigned long long)which,
                            ci->pPoolSizes[i].type,
                            ci->pPoolSizes[i].descriptorCount);
                }
                fflush(stderr);
            }
        }
    }
    return r;
}

__attribute__((visibility("default")))
VkResult MaliProbe_ResetDescriptorPool(
    VkDevice device, VkDescriptorPool pool, uint32_t flags) {
    if (probe_enabled())
        atomic_fetch_add_explicit(&pool_resets, 1, memory_order_relaxed);
    return next_ResetDescriptorPool(device, pool, flags);
}

__attribute__((visibility("default")))
void MaliProbe_DestroyDescriptorPool(
    VkDevice device, VkDescriptorPool pool, const void *pAllocator) {
    if (probe_enabled())
        atomic_fetch_add_explicit(&pool_destroys, 1, memory_order_relaxed);
    next_DestroyDescriptorPool(device, pool, pAllocator);
}

__attribute__((visibility("default")))
PFN_vkVoidFunction MaliProbe_GetDeviceProcAddr(VkDevice device, const char *pName) {
    if (pName) {
        if (strcmp(pName, "vkAllocateDescriptorSets") == 0)
            return (PFN_vkVoidFunction)MaliProbe_AllocateDescriptorSets;
        if (strcmp(pName, "vkFreeDescriptorSets") == 0)
            return (PFN_vkVoidFunction)MaliProbe_FreeDescriptorSets;
        if (strcmp(pName, "vkCreateDescriptorPool") == 0)
            return (PFN_vkVoidFunction)MaliProbe_CreateDescriptorPool;
        if (strcmp(pName, "vkResetDescriptorPool") == 0)
            return (PFN_vkVoidFunction)MaliProbe_ResetDescriptorPool;
        if (strcmp(pName, "vkDestroyDescriptorPool") == 0)
            return (PFN_vkVoidFunction)MaliProbe_DestroyDescriptorPool;
    }
    if (next_GetDeviceProcAddr) return next_GetDeviceProcAddr(device, pName);
    return NULL;
}

__attribute__((visibility("default")))
VkResult MaliProbe_CreateDevice(
    VkPhysicalDevice pd, const void *pCreateInfo, const void *pAllocator, VkDevice *pDevice) {
    typedef struct chain_hdr { int32_t sType; const void *pNext; int32_t function; void *u; } chain_hdr;
    const chain_hdr *h = (const chain_hdr*)((const void**)pCreateInfo)[1];
    while (h && !(h->sType == 48 /* VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO */ && h->function == 0))
        h = (const chain_hdr*)h->pNext;
    if (!h || !h->u) return -3;

    VkLayerDeviceLink *link = (VkLayerDeviceLink*)h->u;
    PFN_vkVoidFunction (*gipa)(VkInstance, const char*) = link->pfnNextGetInstanceProcAddr;
    next_GetDeviceProcAddr = link->pfnNextGetDeviceProcAddr;

    ((chain_hdr*)h)->u = link->pNext;

    PFN_vkCreateDevice createNext = (PFN_vkCreateDevice)gipa(NULL, "vkCreateDevice");
    if (!createNext) return -3;

    VkResult r = createNext(pd, pCreateInfo, pAllocator, pDevice);
    if (r != 0) return r;

    next_AllocateDescriptorSets = (PFN_vkAllocateDescriptorSets)next_GetDeviceProcAddr(*pDevice, "vkAllocateDescriptorSets");
    next_FreeDescriptorSets     = (PFN_vkFreeDescriptorSets)    next_GetDeviceProcAddr(*pDevice, "vkFreeDescriptorSets");
    next_CreateDescriptorPool   = (PFN_vkCreateDescriptorPool)  next_GetDeviceProcAddr(*pDevice, "vkCreateDescriptorPool");
    next_ResetDescriptorPool    = (PFN_vkResetDescriptorPool)   next_GetDeviceProcAddr(*pDevice, "vkResetDescriptorPool");
    next_DestroyDescriptorPool  = (PFN_vkDestroyDescriptorPool) next_GetDeviceProcAddr(*pDevice, "vkDestroyDescriptorPool");

    if (probe_enabled()) {
        uint64_t baseline = count_mali_vmas();
        atomic_store_explicit(&last_mali_count, baseline, memory_order_relaxed);
        fprintf(stderr,
                "[mali_alloc_probe] device created, hooks armed, baseline mali0_vmas=%llu\n",
                (unsigned long long)baseline);
        fflush(stderr);
    }
    return r;
}

__attribute__((visibility("default")))
VkResult MaliProbe_CreateInstance(const void *pCreateInfo, const void *pAllocator, VkInstance *pInstance) {
    typedef struct chain_hdr { int32_t sType; const void *pNext; int32_t function; void *u; } chain_hdr;
    const chain_hdr *h = (const chain_hdr*)((const void**)pCreateInfo)[1];
    while (h && !(h->sType == 47 /* VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO */ && h->function == 0))
        h = (const chain_hdr*)h->pNext;
    if (!h || !h->u) return -3;

    typedef struct { void *pNext; PFN_vkVoidFunction (*pfnNextGetInstanceProcAddr)(VkInstance, const char*); } iLink;
    iLink *link = (iLink*)h->u;
    PFN_vkVoidFunction (*gipa)(VkInstance, const char*) = link->pfnNextGetInstanceProcAddr;
    next_GetInstanceProcAddr = gipa;

    ((chain_hdr*)h)->u = link->pNext;

    PFN_vkCreateInstance createNext = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    if (!createNext) return -3;

    VkResult r = createNext(pCreateInfo, pAllocator, pInstance);
    if (r != 0) return r;

    if (probe_enabled()) {
        fprintf(stderr, "[mali_alloc_probe] instance created\n");
        fflush(stderr);
    }
    return r;
}

__attribute__((visibility("default")))
PFN_vkVoidFunction MaliProbe_GetInstanceProcAddr(VkInstance instance, const char *pName) {
    if (!pName) return NULL;
    if (strcmp(pName, "vkCreateInstance") == 0)
        return (PFN_vkVoidFunction)MaliProbe_CreateInstance;
    if (strcmp(pName, "vkGetInstanceProcAddr") == 0)
        return (PFN_vkVoidFunction)MaliProbe_GetInstanceProcAddr;
    if (strcmp(pName, "vkCreateDevice") == 0)
        return (PFN_vkVoidFunction)MaliProbe_CreateDevice;
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0)
        return (PFN_vkVoidFunction)MaliProbe_GetDeviceProcAddr;
    if (strcmp(pName, "vkAllocateDescriptorSets") == 0)
        return (PFN_vkVoidFunction)MaliProbe_AllocateDescriptorSets;
    if (strcmp(pName, "vkFreeDescriptorSets") == 0)
        return (PFN_vkVoidFunction)MaliProbe_FreeDescriptorSets;
    if (strcmp(pName, "vkCreateDescriptorPool") == 0)
        return (PFN_vkVoidFunction)MaliProbe_CreateDescriptorPool;
    if (strcmp(pName, "vkResetDescriptorPool") == 0)
        return (PFN_vkVoidFunction)MaliProbe_ResetDescriptorPool;
    if (strcmp(pName, "vkDestroyDescriptorPool") == 0)
        return (PFN_vkVoidFunction)MaliProbe_DestroyDescriptorPool;
    if (next_GetInstanceProcAddr) return next_GetInstanceProcAddr(instance, pName);
    return NULL;
}
