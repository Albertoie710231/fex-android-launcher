/*
 * VK_LAYER_WRAPPER_POOL_RESETTER — Phase 2 v0: per-descriptor-pool tracking.
 *
 * Background. The leegao Vulkan wrapper has a single memory-bounding
 * mechanism for descriptor-pool memory: vkResetDescriptorPool. Each
 * vkAllocateDescriptorSets costs ~1 mali0 mmap; without periodic resets
 * the mmap count climbs until vm.max_map_count or the OOM killer ends
 * the process. DXVK 1.10.3 (current production) calls vkResetDescriptorPool
 * naturally and stays bounded. DXVK 2.x's set-list cache deliberately
 * skips the reset because it assumes spec-conformant pool semantics —
 * exactly the code path that OOM-killed the experimental 1.10.3+set-list-
 * cache backport in 3.6 min (`failed_attempts.md:11`).
 *
 * Phase 2 will add wrapper-aware reset enforcement: when a pool is
 * "quiescent" (no in-flight submits reference any of its sets) AND the
 * pool's allocated count exceeds a threshold, force a reset to recycle
 * the underlying mali memory. This requires:
 *   - Per-pool stats (allocs, frees, resets, currently-allocated). [v0]
 *   - In-flight tracking via cmd buffer + fence hooks.              [v1]
 *   - Forced-reset trigger logic.                                   [v2]
 *
 * v0 (this layer): observability only. Hooks the five pool/set
 * entrypoints, maintains a per-pool linked list of stats, and logs
 * periodically to stderr. No enforcement; useful as a baseline against
 * which v1/v2 changes can be measured. Also informs the threshold for
 * forced resets — we want to see, on the production 1.10.3 stack, how
 * often DXVK resets and how many sets accumulate per pool before resets.
 *
 * Build:
 *   ~/Android/Sdk/ndk/27.3.13750724/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang \
 *     -shared -fPIC -O2 -o libwrapper_pool_resetter.so wrapper_pool_resetter_layer.c
 *
 * Deploy (mirror existing layer pattern):
 *   files/imagefs_bionic/usr/lib/libwrapper_pool_resetter.so
 *   files/imagefs_bionic/usr/share/vulkan/implicit_layer.d/wrapper_pool_resetter_layer.json
 *
 * Activate via env: WRAPPER_POOL_RESETTER=1
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>

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

typedef void (*PFN_vkVoidFunction)(void);
typedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddr)(VkInstance, const char*);
typedef PFN_vkVoidFunction (*PFN_vkGetDeviceProcAddr)(VkDevice, const char*);
typedef VkResult (*PFN_vkCreateInstance)(const void*, const void*, VkInstance*);
typedef VkResult (*PFN_vkCreateDevice)(VkPhysicalDevice, const void*, const void*, VkDevice*);
typedef VkResult (*PFN_vkAllocateDescriptorSets)(VkDevice, const VkDescriptorSetAllocateInfo*, VkDescriptorSet*);
typedef VkResult (*PFN_vkFreeDescriptorSets)(VkDevice, VkDescriptorPool, uint32_t, const VkDescriptorSet*);
typedef VkResult (*PFN_vkCreateDescriptorPool)(VkDevice, const void*, const void*, VkDescriptorPool*);
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

static int enabled(void) {
    const char *e = getenv("WRAPPER_POOL_RESETTER");
    return e && e[0] == '1';
}

/* Per-pool tracking: linked list of PoolEntry. The active pool count
 * is small (DXVK has on the order of tens of pools live at once); a
 * linear scan is fine and avoids needing a hash-table dependency. */
typedef struct PoolEntry {
    VkDescriptorPool pool;
    uint32_t maxSets;
    uint32_t flags;
    /* Cumulative counters across the pool's lifetime. */
    uint64_t allocs_calls;
    uint64_t allocs_sets;
    uint64_t free_calls;
    uint64_t free_sets;
    uint64_t reset_calls;
    /* Currently allocated set count (resets at vkResetDescriptorPool). */
    int64_t  allocated_now;
    int64_t  max_allocated_seen;
    struct PoolEntry *next;
} PoolEntry;

static PoolEntry *g_pools = NULL;
static pthread_mutex_t g_pools_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_op_counter = 0;

static PoolEntry *find_pool_locked(VkDescriptorPool pool) {
    for (PoolEntry *e = g_pools; e; e = e->next) {
        if (e->pool == pool) return e;
    }
    return NULL;
}

static PoolEntry *get_or_create_pool(VkDescriptorPool pool, uint32_t maxSets, uint32_t flags) {
    pthread_mutex_lock(&g_pools_mutex);
    PoolEntry *e = find_pool_locked(pool);
    if (!e) {
        e = (PoolEntry *)calloc(1, sizeof(PoolEntry));
        if (e) {
            e->pool = pool;
            e->maxSets = maxSets;
            e->flags = flags;
            e->next = g_pools;
            g_pools = e;
        }
    }
    pthread_mutex_unlock(&g_pools_mutex);
    return e;
}

static void destroy_pool(VkDescriptorPool pool) {
    pthread_mutex_lock(&g_pools_mutex);
    PoolEntry **pp = &g_pools;
    while (*pp) {
        if ((*pp)->pool == pool) {
            PoolEntry *victim = *pp;
            *pp = victim->next;
            free(victim);
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_pools_mutex);
}

#define LOG_EVERY 1000

static void maybe_log(void) {
    uint64_t op = atomic_fetch_add_explicit(&g_op_counter, 1, memory_order_relaxed) + 1;
    if (op % LOG_EVERY != 0) return;

    pthread_mutex_lock(&g_pools_mutex);
    int n = 0;
    int n_unbounded = 0;
    int64_t total_now = 0;
    int64_t total_peak = 0;
    uint64_t total_allocs = 0;
    uint64_t total_resets = 0;
    PoolEntry *worst = NULL;
    for (PoolEntry *e = g_pools; e; e = e->next) {
        n++;
        total_now += e->allocated_now;
        total_peak += e->max_allocated_seen;
        total_allocs += e->allocs_sets;
        total_resets += e->reset_calls;
        if (e->reset_calls == 0 && e->allocs_sets > 0) n_unbounded++;
        if (!worst || e->max_allocated_seen > worst->max_allocated_seen) worst = e;
    }
    fprintf(stderr,
        "[wrapper_pool_resetter] op=%llu pools=%d unbounded=%d "
        "(allocs=%llu resets=%llu now=%lld peak=%lld)",
        (unsigned long long)op, n, n_unbounded,
        (unsigned long long)total_allocs,
        (unsigned long long)total_resets,
        (long long)total_now,
        (long long)total_peak);
    if (worst) {
        fprintf(stderr,
            " worst=%p (allocs=%llu frees=%llu resets=%llu now=%lld peak=%lld maxSets=%u flags=0x%x)",
            worst->pool,
            (unsigned long long)worst->allocs_sets,
            (unsigned long long)worst->free_sets,
            (unsigned long long)worst->reset_calls,
            (long long)worst->allocated_now,
            (long long)worst->max_allocated_seen,
            worst->maxSets,
            worst->flags);
    }
    fputc('\n', stderr);
    fflush(stderr);
    pthread_mutex_unlock(&g_pools_mutex);
}

__attribute__((visibility("default")))
VkResult PoolResetter_CreateDescriptorPool(
    VkDevice device, const void *pCreateInfo, const void *pAllocator, VkDescriptorPool *pPool) {
    VkResult r = next_CreateDescriptorPool(device, pCreateInfo, pAllocator, pPool);
    if (enabled() && r == 0 && pPool) {
        const VkDescriptorPoolCreateInfo *ci = (const VkDescriptorPoolCreateInfo *)pCreateInfo;
        get_or_create_pool(*pPool, ci ? ci->maxSets : 0, ci ? ci->flags : 0);
    }
    return r;
}

__attribute__((visibility("default")))
VkResult PoolResetter_AllocateDescriptorSets(
    VkDevice device, const VkDescriptorSetAllocateInfo *pInfo, VkDescriptorSet *pSets) {
    VkResult r = next_AllocateDescriptorSets(device, pInfo, pSets);
    if (enabled() && r == 0 && pInfo) {
        pthread_mutex_lock(&g_pools_mutex);
        PoolEntry *e = find_pool_locked(pInfo->descriptorPool);
        if (e) {
            e->allocs_calls++;
            e->allocs_sets += pInfo->descriptorSetCount;
            e->allocated_now += (int64_t)pInfo->descriptorSetCount;
            if (e->allocated_now > e->max_allocated_seen)
                e->max_allocated_seen = e->allocated_now;
        }
        pthread_mutex_unlock(&g_pools_mutex);
        maybe_log();
    }
    return r;
}

__attribute__((visibility("default")))
VkResult PoolResetter_FreeDescriptorSets(
    VkDevice device, VkDescriptorPool pool, uint32_t count, const VkDescriptorSet *pSets) {
    VkResult r = next_FreeDescriptorSets(device, pool, count, pSets);
    if (enabled() && r == 0) {
        pthread_mutex_lock(&g_pools_mutex);
        PoolEntry *e = find_pool_locked(pool);
        if (e) {
            e->free_calls++;
            e->free_sets += count;
            e->allocated_now -= (int64_t)count;
        }
        pthread_mutex_unlock(&g_pools_mutex);
    }
    return r;
}

__attribute__((visibility("default")))
VkResult PoolResetter_ResetDescriptorPool(
    VkDevice device, VkDescriptorPool pool, uint32_t flags) {
    VkResult r = next_ResetDescriptorPool(device, pool, flags);
    if (enabled() && r == 0) {
        pthread_mutex_lock(&g_pools_mutex);
        PoolEntry *e = find_pool_locked(pool);
        if (e) {
            e->reset_calls++;
            e->allocated_now = 0;
        }
        pthread_mutex_unlock(&g_pools_mutex);
    }
    return r;
}

__attribute__((visibility("default")))
void PoolResetter_DestroyDescriptorPool(
    VkDevice device, VkDescriptorPool pool, const void *pAllocator) {
    if (enabled()) destroy_pool(pool);
    next_DestroyDescriptorPool(device, pool, pAllocator);
}

__attribute__((visibility("default")))
PFN_vkVoidFunction PoolResetter_GetDeviceProcAddr(VkDevice device, const char *pName) {
    if (pName) {
        if (strcmp(pName, "vkCreateDescriptorPool") == 0)
            return (PFN_vkVoidFunction)PoolResetter_CreateDescriptorPool;
        if (strcmp(pName, "vkDestroyDescriptorPool") == 0)
            return (PFN_vkVoidFunction)PoolResetter_DestroyDescriptorPool;
        if (strcmp(pName, "vkResetDescriptorPool") == 0)
            return (PFN_vkVoidFunction)PoolResetter_ResetDescriptorPool;
        if (strcmp(pName, "vkAllocateDescriptorSets") == 0)
            return (PFN_vkVoidFunction)PoolResetter_AllocateDescriptorSets;
        if (strcmp(pName, "vkFreeDescriptorSets") == 0)
            return (PFN_vkVoidFunction)PoolResetter_FreeDescriptorSets;
    }
    if (next_GetDeviceProcAddr) return next_GetDeviceProcAddr(device, pName);
    return NULL;
}

__attribute__((visibility("default")))
VkResult PoolResetter_CreateDevice(
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

    if (enabled()) {
        fprintf(stderr, "[wrapper_pool_resetter] device created, hooks armed\n");
        fflush(stderr);
    }
    return r;
}

__attribute__((visibility("default")))
VkResult PoolResetter_CreateInstance(const void *pCreateInfo, const void *pAllocator, VkInstance *pInstance) {
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

    if (enabled()) {
        fprintf(stderr, "[wrapper_pool_resetter] instance created\n");
        fflush(stderr);
    }
    return r;
}

__attribute__((visibility("default")))
PFN_vkVoidFunction PoolResetter_GetInstanceProcAddr(VkInstance instance, const char *pName) {
    if (!pName) return NULL;
    if (strcmp(pName, "vkCreateInstance") == 0)
        return (PFN_vkVoidFunction)PoolResetter_CreateInstance;
    if (strcmp(pName, "vkGetInstanceProcAddr") == 0)
        return (PFN_vkVoidFunction)PoolResetter_GetInstanceProcAddr;
    if (strcmp(pName, "vkCreateDevice") == 0)
        return (PFN_vkVoidFunction)PoolResetter_CreateDevice;
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0)
        return (PFN_vkVoidFunction)PoolResetter_GetDeviceProcAddr;
    if (strcmp(pName, "vkAllocateDescriptorSets") == 0)
        return (PFN_vkVoidFunction)PoolResetter_AllocateDescriptorSets;
    if (strcmp(pName, "vkFreeDescriptorSets") == 0)
        return (PFN_vkVoidFunction)PoolResetter_FreeDescriptorSets;
    if (strcmp(pName, "vkCreateDescriptorPool") == 0)
        return (PFN_vkVoidFunction)PoolResetter_CreateDescriptorPool;
    if (strcmp(pName, "vkResetDescriptorPool") == 0)
        return (PFN_vkVoidFunction)PoolResetter_ResetDescriptorPool;
    if (strcmp(pName, "vkDestroyDescriptorPool") == 0)
        return (PFN_vkVoidFunction)PoolResetter_DestroyDescriptorPool;
    if (next_GetInstanceProcAddr) return next_GetInstanceProcAddr(instance, pName);
    return NULL;
}
