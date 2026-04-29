/*
 * VK_LAYER_WRAPPER_POOL_RESETTER — Phase 2 v0 (per-pool stats) + v1 (in-flight tracking).
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
 * periodically to stderr. No enforcement; useful as a baseline.
 *
 * v1 (this layer, gated on WRAPPER_POOL_RESETTER_V1=1): adds the
 * "is anyone using this pool right now?" signal. Hooks
 * vkAllocate/FreeCommandBuffers, vkBegin/ResetCommandBuffer,
 * vkCmdBindDescriptorSets, vkQueueSubmit, vkWaitForFences,
 * vkGetFenceStatus, vkResetFences, vkDestroyFence. Maintains:
 *   - set->pool hashtable (populated at vkAllocateDescriptorSets)
 *   - per-cmd-buffer dedup'd list of pools its bound sets touch
 *   - per-fence list of in-flight pool refs
 *   - per-pool inflight_uses counter (incremented at submit,
 *     decremented at fence completion / reset / destroy)
 *
 * v1 is OBSERVABILITY ONLY — we do NOT force resets, do NOT invalidate
 * sets, and do NOT trust fence completion as a hard "GPU done" signal
 * (state_stack_wrapper.md:18 — wrapper sync semantics are not per-spec).
 * The inflight_uses counter is logged as a heuristic upper bound for v2
 * to combine with other signals, not as a safety primitive.
 *
 * PoolEntry lifetime: never freed. vkDestroyDescriptorPool sets a
 * destroyed flag; the entry stays around so any cmd-buffer / fence list
 * that still references the pointer can keep doing dead-counter ops
 * without UAF. get_or_create_pool reuses destroyed entries that match
 * a recycled VkDescriptorPool address.
 *
 * Build:
 *   ~/Android/Sdk/ndk/27.3.13750724/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang \
 *     -shared -fPIC -O2 -o libwrapper_pool_resetter.so wrapper_pool_resetter_layer.c
 *
 * Deploy (mirror existing layer pattern):
 *   files/imagefs_bionic/usr/lib/libwrapper_pool_resetter.so
 *   files/imagefs_bionic/usr/share/vulkan/implicit_layer.d/wrapper_pool_resetter_layer.json
 *
 * Activate via env:
 *   WRAPPER_POOL_RESETTER=1     — v0 stats (always required)
 *   WRAPPER_POOL_RESETTER_V1=1  — v1 in-flight tracking (additional)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>

typedef int VkResult;
typedef void *VkInstance;
typedef void *VkDevice;
typedef void *VkPhysicalDevice;
typedef void *VkDescriptorPool;
typedef void *VkDescriptorSet;
typedef void *VkDescriptorSetLayout;
typedef void *VkCommandBuffer;
typedef void *VkCommandPool;
typedef void *VkFence;
typedef void *VkQueue;
typedef void *VkSemaphore;
typedef void *VkPipelineLayout;
typedef int   VkPipelineBindPoint;
typedef uint32_t VkBool32;

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

typedef struct VkCommandBufferAllocateInfo {
    int32_t sType;
    const void *pNext;
    VkCommandPool commandPool;
    int32_t  level;
    uint32_t commandBufferCount;
} VkCommandBufferAllocateInfo;

typedef struct VkSubmitInfo {
    int32_t sType;
    const void *pNext;
    uint32_t waitSemaphoreCount;
    const VkSemaphore *pWaitSemaphores;
    const uint32_t    *pWaitDstStageMask;
    uint32_t commandBufferCount;
    const VkCommandBuffer *pCommandBuffers;
    uint32_t signalSemaphoreCount;
    const VkSemaphore *pSignalSemaphores;
} VkSubmitInfo;

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
typedef VkResult (*PFN_vkAllocateCommandBuffers)(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer*);
typedef void     (*PFN_vkFreeCommandBuffers)(VkDevice, VkCommandPool, uint32_t, const VkCommandBuffer*);
typedef VkResult (*PFN_vkBeginCommandBuffer)(VkCommandBuffer, const void*);
typedef VkResult (*PFN_vkResetCommandBuffer)(VkCommandBuffer, uint32_t);
typedef void     (*PFN_vkCmdBindDescriptorSets)(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t, const VkDescriptorSet*, uint32_t, const uint32_t*);
typedef VkResult (*PFN_vkQueueSubmit)(VkQueue, uint32_t, const VkSubmitInfo*, VkFence);
typedef VkResult (*PFN_vkWaitForFences)(VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t);
typedef VkResult (*PFN_vkGetFenceStatus)(VkDevice, VkFence);
typedef VkResult (*PFN_vkResetFences)(VkDevice, uint32_t, const VkFence*);
typedef void     (*PFN_vkDestroyFence)(VkDevice, VkFence, const void*);

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
static PFN_vkAllocateCommandBuffers next_AllocateCommandBuffers = NULL;
static PFN_vkFreeCommandBuffers     next_FreeCommandBuffers     = NULL;
static PFN_vkBeginCommandBuffer     next_BeginCommandBuffer     = NULL;
static PFN_vkResetCommandBuffer     next_ResetCommandBuffer     = NULL;
static PFN_vkCmdBindDescriptorSets  next_CmdBindDescriptorSets  = NULL;
static PFN_vkQueueSubmit            next_QueueSubmit            = NULL;
static PFN_vkWaitForFences          next_WaitForFences          = NULL;
static PFN_vkGetFenceStatus         next_GetFenceStatus         = NULL;
static PFN_vkResetFences            next_ResetFences            = NULL;
static PFN_vkDestroyFence           next_DestroyFence           = NULL;

static int enabled(void) {
    const char *e = getenv("WRAPPER_POOL_RESETTER");
    return e && e[0] == '1';
}

static int enabled_v1(void) {
    const char *e = getenv("WRAPPER_POOL_RESETTER_V1");
    return e && e[0] == '1';
}

static int enabled_v2(void) {
    const char *e = getenv("WRAPPER_POOL_RESETTER_V2");
    return e && e[0] == '1';
}

/* v2a observer mode — does NOT call vkResetDescriptorPool. Instead, when
 * the trigger condition matches, it logs "[wrapper_pool_resetter] WOULD
 * force reset ..." and bumps a per-pool counter. Validates the trigger
 * logic without risking a regression in DXVK 1.10.3's natural reset
 * cycle (failed_attempts.md:11 — wrong reset semantics OOM'd Sekiro
 * in 3.6 min historically).
 *
 * Trigger: pool.inflight_uses == 0 AND pool.allocated_now >= threshold
 * AND time since last app vkResetDescriptorPool >= grace period.
 *
 * Tunables (read once at first device-create):
 *   WRAPPER_POOL_RESETTER_THRESHOLD  — default 2000
 *   WRAPPER_POOL_RESETTER_GRACE_MS   — default 1000 (1 second)
 */
static int      g_v2_threshold = 2000;
static uint64_t g_v2_grace_ns  = 1000000000ULL;
static pthread_once_t g_v2_init_once = PTHREAD_ONCE_INIT;

static void v2_init_impl(void) {
    const char *t = getenv("WRAPPER_POOL_RESETTER_THRESHOLD");
    if (t) { int v = atoi(t); if (v > 0) g_v2_threshold = v; }
    const char *g = getenv("WRAPPER_POOL_RESETTER_GRACE_MS");
    if (g) { int v = atoi(g); if (v >= 0) g_v2_grace_ns = (uint64_t)v * 1000000ULL; }
}

static void v2_init(void) {
    pthread_once(&g_v2_init_once, v2_init_impl);
}

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ================================================================
 *  v0 — per-pool stats (linked list, small N, linear scan is fine)
 * ================================================================ */

typedef struct PoolEntry {
    VkDescriptorPool pool;
    uint32_t maxSets;
    uint32_t flags;
    uint64_t allocs_calls;
    uint64_t allocs_sets;
    uint64_t free_calls;
    uint64_t free_sets;
    uint64_t reset_calls;
    int64_t  allocated_now;
    int64_t  max_allocated_seen;
    /* v1: in-flight cmd-buffer references to this pool. Heuristic upper
     * bound (wrapper fence semantics not trusted, state_stack_wrapper.md:18). */
    int64_t  inflight_uses;
    int64_t  inflight_uses_peak;
    /* v2a observer-mode trigger bookkeeping. */
    uint64_t last_reset_ns;      /* monotonic ns at last app-driven reset (0 = never) */
    uint64_t last_logged_ns;     /* monotonic ns at last WOULD-reset log (rate limiter) */
    uint64_t would_reset_count;  /* trigger fire count (logged or rate-limited) */
    int      destroyed;          /* never freed; reusable if same pool addr is recycled */
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
    if (e && e->destroyed) {
        e->destroyed = 0;
        e->maxSets = maxSets;
        e->flags = flags;
        e->allocs_calls = 0;
        e->allocs_sets = 0;
        e->free_calls = 0;
        e->free_sets = 0;
        e->reset_calls = 0;
        e->allocated_now = 0;
        e->max_allocated_seen = 0;
        e->inflight_uses = 0;
        e->inflight_uses_peak = 0;
        e->last_reset_ns = 0;
        e->last_logged_ns = 0;
        e->would_reset_count = 0;
    } else if (!e) {
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

/* ================================================================
 *  v1 — set->pool hashtable.
 *  Populated at vkAllocateDescriptorSets, drained at FreeDescriptorSets
 *  and ResetDescriptorPool. Not drained at DestroyDescriptorPool —
 *  destroyed pools' entries leak until end of session (bounded by total
 *  alloc count, fine in practice).
 * ================================================================ */
#define SET_BUCKETS 16384  /* power of 2 */

typedef struct SetEntry {
    VkDescriptorSet set;
    PoolEntry *pool_entry;
    struct SetEntry *next;
} SetEntry;

static SetEntry *g_set_buckets[SET_BUCKETS];
static pthread_mutex_t g_set_mutex = PTHREAD_MUTEX_INITIALIZER;

static unsigned set_hash(VkDescriptorSet s) {
    uintptr_t p = (uintptr_t)s;
    p ^= p >> 16;
    p *= 0x45d9f3bUL;
    p ^= p >> 16;
    return (unsigned)(p & (SET_BUCKETS - 1));
}

static void set_table_insert(VkDescriptorSet s, PoolEntry *pe) {
    SetEntry *e = (SetEntry*)calloc(1, sizeof(SetEntry));
    if (!e) return;
    e->set = s;
    e->pool_entry = pe;
    unsigned b = set_hash(s);
    pthread_mutex_lock(&g_set_mutex);
    e->next = g_set_buckets[b];
    g_set_buckets[b] = e;
    pthread_mutex_unlock(&g_set_mutex);
}

static void set_table_remove(VkDescriptorSet s) {
    unsigned b = set_hash(s);
    pthread_mutex_lock(&g_set_mutex);
    SetEntry **pp = &g_set_buckets[b];
    while (*pp) {
        if ((*pp)->set == s) {
            SetEntry *v = *pp;
            *pp = v->next;
            free(v);
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_set_mutex);
}

/* Walk all buckets and drop entries belonging to pe. Called from
 * vkResetDescriptorPool (sets are invalidated en-masse). */
static void set_table_purge_pool(PoolEntry *pe) {
    pthread_mutex_lock(&g_set_mutex);
    for (int i = 0; i < SET_BUCKETS; i++) {
        SetEntry **pp = &g_set_buckets[i];
        while (*pp) {
            if ((*pp)->pool_entry == pe) {
                SetEntry *v = *pp;
                *pp = v->next;
                free(v);
            } else {
                pp = &(*pp)->next;
            }
        }
    }
    pthread_mutex_unlock(&g_set_mutex);
}

static PoolEntry *set_table_lookup(VkDescriptorSet s) {
    unsigned b = set_hash(s);
    pthread_mutex_lock(&g_set_mutex);
    PoolEntry *r = NULL;
    for (SetEntry *e = g_set_buckets[b]; e; e = e->next) {
        if (e->set == s) { r = e->pool_entry; break; }
    }
    pthread_mutex_unlock(&g_set_mutex);
    return r;
}

/* ================================================================
 *  v1 — per-cmd-buffer record. Tracks the dedup'd set of pools whose
 *  sets were bound during the current recording. Cleared at Begin/Reset,
 *  snapshotted at Submit.
 * ================================================================ */
typedef struct CmdBufEntry {
    VkCommandBuffer cmdbuf;
    PoolEntry **pool_refs;
    int pool_refs_count;
    int pool_refs_cap;
    struct CmdBufEntry *next;
} CmdBufEntry;

static CmdBufEntry *g_cmdbufs = NULL;
static pthread_mutex_t g_cmdbufs_mutex = PTHREAD_MUTEX_INITIALIZER;

static CmdBufEntry *find_cmdbuf_locked(VkCommandBuffer cb) {
    for (CmdBufEntry *e = g_cmdbufs; e; e = e->next)
        if (e->cmdbuf == cb) return e;
    return NULL;
}

static CmdBufEntry *cmdbuf_get_or_create_locked(VkCommandBuffer cb) {
    CmdBufEntry *e = find_cmdbuf_locked(cb);
    if (!e) {
        e = (CmdBufEntry*)calloc(1, sizeof(CmdBufEntry));
        if (e) {
            e->cmdbuf = cb;
            e->next = g_cmdbufs;
            g_cmdbufs = e;
        }
    }
    return e;
}

static void cmdbuf_register(VkCommandBuffer cb) {
    pthread_mutex_lock(&g_cmdbufs_mutex);
    cmdbuf_get_or_create_locked(cb);
    pthread_mutex_unlock(&g_cmdbufs_mutex);
}

static void cmdbuf_clear(VkCommandBuffer cb) {
    pthread_mutex_lock(&g_cmdbufs_mutex);
    CmdBufEntry *e = find_cmdbuf_locked(cb);
    if (e) {
        free(e->pool_refs);
        e->pool_refs = NULL;
        e->pool_refs_count = 0;
        e->pool_refs_cap = 0;
    }
    pthread_mutex_unlock(&g_cmdbufs_mutex);
}

static void cmdbuf_destroy(VkCommandBuffer cb) {
    pthread_mutex_lock(&g_cmdbufs_mutex);
    CmdBufEntry **pp = &g_cmdbufs;
    while (*pp) {
        if ((*pp)->cmdbuf == cb) {
            CmdBufEntry *v = *pp;
            *pp = v->next;
            free(v->pool_refs);
            free(v);
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_cmdbufs_mutex);
}

static void cmdbuf_add_ref_locked(CmdBufEntry *e, PoolEntry *pe) {
    for (int i = 0; i < e->pool_refs_count; i++) {
        if (e->pool_refs[i] == pe) return;
    }
    if (e->pool_refs_count == e->pool_refs_cap) {
        int new_cap = e->pool_refs_cap ? e->pool_refs_cap * 2 : 4;
        PoolEntry **new_refs = (PoolEntry**)realloc(e->pool_refs, new_cap * sizeof(PoolEntry*));
        if (!new_refs) return;
        e->pool_refs = new_refs;
        e->pool_refs_cap = new_cap;
    }
    e->pool_refs[e->pool_refs_count++] = pe;
}

/* ================================================================
 *  v2a — observer-mode trigger
 *
 *  Called from fence_drain after pool inflight_uses are decremented;
 *  caller must hold g_pools_mutex. Returns a snapshot of trigger
 *  state if the rate-limited log should fire (caller logs after lock
 *  release to avoid blocking other pool ops on stderr). Defined ahead
 *  of fence_drain so the struct is in scope there.
 * ================================================================ */
typedef struct V2TriggerInfo {
    void    *pool;
    int64_t  allocated_now;
    int64_t  max_allocated_seen;
    uint64_t since_last_reset_us;
    uint64_t would_reset_count;
    int      fired;
} V2TriggerInfo;

static V2TriggerInfo v2_check_trigger_locked(PoolEntry *pe, uint64_t now_ns) {
    V2TriggerInfo info = (V2TriggerInfo){0};
    if (!enabled_v2()) return info;
    if (pe->destroyed) return info;
    if (pe->inflight_uses != 0) return info;
    if (pe->allocated_now < g_v2_threshold) return info;
    if (pe->last_reset_ns != 0 && now_ns - pe->last_reset_ns < g_v2_grace_ns) return info;

    pe->would_reset_count++;
    /* Rate-limit per-pool log to once per second to avoid stderr spam.
     * The would_reset_count keeps incrementing regardless. */
    if (pe->last_logged_ns != 0 && now_ns - pe->last_logged_ns < 1000000000ULL) {
        return info;
    }
    pe->last_logged_ns = now_ns;
    info.pool = pe->pool;
    info.allocated_now = pe->allocated_now;
    info.max_allocated_seen = pe->max_allocated_seen;
    info.since_last_reset_us = pe->last_reset_ns ? (now_ns - pe->last_reset_ns) / 1000 : 0;
    info.would_reset_count = pe->would_reset_count;
    info.fired = 1;
    return info;
}

/* ================================================================
 *  v1 — per-fence in-flight refs.
 * ================================================================ */
typedef struct InflightRef {
    PoolEntry *pe;
    int count;
} InflightRef;

typedef struct FenceEntry {
    VkFence fence;
    InflightRef *refs;
    int refs_count;
    int refs_cap;
    struct FenceEntry *next;
} FenceEntry;

static FenceEntry *g_fences = NULL;
static pthread_mutex_t g_fences_mutex = PTHREAD_MUTEX_INITIALIZER;

static FenceEntry *find_fence_locked(VkFence f) {
    for (FenceEntry *e = g_fences; e; e = e->next)
        if (e->fence == f) return e;
    return NULL;
}

static FenceEntry *fence_get_or_create_locked(VkFence f) {
    FenceEntry *e = find_fence_locked(f);
    if (!e) {
        e = (FenceEntry*)calloc(1, sizeof(FenceEntry));
        if (e) {
            e->fence = f;
            e->next = g_fences;
            g_fences = e;
        }
    }
    return e;
}

/* Append (pe, count) to fence's list, merging if pe is already there. */
static void fence_add_refs(VkFence f, PoolEntry **pes, int n) {
    if (n <= 0) return;
    pthread_mutex_lock(&g_fences_mutex);
    FenceEntry *fe = fence_get_or_create_locked(f);
    if (!fe) { pthread_mutex_unlock(&g_fences_mutex); return; }
    for (int i = 0; i < n; i++) {
        PoolEntry *pe = pes[i];
        int merged = 0;
        for (int j = 0; j < fe->refs_count; j++) {
            if (fe->refs[j].pe == pe) { fe->refs[j].count++; merged = 1; break; }
        }
        if (merged) continue;
        if (fe->refs_count == fe->refs_cap) {
            int new_cap = fe->refs_cap ? fe->refs_cap * 2 : 4;
            InflightRef *nr = (InflightRef*)realloc(fe->refs, new_cap * sizeof(InflightRef));
            if (!nr) continue;
            fe->refs = nr;
            fe->refs_cap = new_cap;
        }
        fe->refs[fe->refs_count].pe = pe;
        fe->refs[fe->refs_count].count = 1;
        fe->refs_count++;
    }
    pthread_mutex_unlock(&g_fences_mutex);
}

/* Snapshot a fence's in-flight list, then clear it. Decrement pool
 * inflight_uses for each ref. Avoids holding two locks at once. */
static void fence_drain(VkFence f) {
    pthread_mutex_lock(&g_fences_mutex);
    FenceEntry *fe = find_fence_locked(f);
    if (!fe || fe->refs_count == 0) {
        pthread_mutex_unlock(&g_fences_mutex);
        return;
    }
    int n = fe->refs_count;
    InflightRef *snap = (InflightRef*)malloc(n * sizeof(InflightRef));
    if (!snap) { pthread_mutex_unlock(&g_fences_mutex); return; }
    memcpy(snap, fe->refs, n * sizeof(InflightRef));
    fe->refs_count = 0;
    pthread_mutex_unlock(&g_fences_mutex);

    V2TriggerInfo *trig = NULL;
    int trig_count = 0;
    if (enabled_v2()) {
        trig = (V2TriggerInfo*)malloc(n * sizeof(V2TriggerInfo));
    }
    uint64_t now_ns = enabled_v2() ? monotonic_ns() : 0;

    pthread_mutex_lock(&g_pools_mutex);
    for (int i = 0; i < n; i++) {
        snap[i].pe->inflight_uses -= snap[i].count;
        if (snap[i].pe->inflight_uses < 0) snap[i].pe->inflight_uses = 0;
        if (trig && snap[i].pe->inflight_uses == 0) {
            V2TriggerInfo info = v2_check_trigger_locked(snap[i].pe, now_ns);
            if (info.fired) trig[trig_count++] = info;
        }
    }
    pthread_mutex_unlock(&g_pools_mutex);
    free(snap);

    if (trig) {
        for (int i = 0; i < trig_count; i++) {
            fprintf(stderr,
                "[wrapper_pool_resetter] WOULD force reset pool=%p (now=%lld peak=%lld inflight=0 last_reset=%lluus_ago count=%llu)\n",
                trig[i].pool,
                (long long)trig[i].allocated_now,
                (long long)trig[i].max_allocated_seen,
                (unsigned long long)trig[i].since_last_reset_us,
                (unsigned long long)trig[i].would_reset_count);
        }
        if (trig_count > 0) fflush(stderr);
        free(trig);
    }
}

static void fence_destroy(VkFence f) {
    fence_drain(f);
    pthread_mutex_lock(&g_fences_mutex);
    FenceEntry **pp = &g_fences;
    while (*pp) {
        if ((*pp)->fence == f) {
            FenceEntry *v = *pp;
            *pp = v->next;
            free(v->refs);
            free(v);
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_fences_mutex);
}

/* ================================================================
 *  Logging
 * ================================================================ */
#define LOG_EVERY 1000

static void maybe_log(void) {
    uint64_t op = atomic_fetch_add_explicit(&g_op_counter, 1, memory_order_relaxed) + 1;
    if (op % LOG_EVERY != 0) return;

    pthread_mutex_lock(&g_pools_mutex);
    int n = 0;
    int n_unbounded = 0;
    int n_qgrow = 0;
    int64_t total_now = 0;
    int64_t total_peak = 0;
    int64_t total_inflight = 0;
    uint64_t total_allocs = 0;
    uint64_t total_resets = 0;
    uint64_t total_would_reset = 0;
    PoolEntry *worst = NULL;
    for (PoolEntry *e = g_pools; e; e = e->next) {
        if (e->destroyed) continue;
        n++;
        total_now += e->allocated_now;
        total_peak += e->max_allocated_seen;
        total_allocs += e->allocs_sets;
        total_resets += e->reset_calls;
        total_inflight += e->inflight_uses;
        total_would_reset += e->would_reset_count;
        if (e->reset_calls == 0 && e->allocs_sets > 0) n_unbounded++;
        if (e->inflight_uses == 0 && e->allocated_now > 0) n_qgrow++;
        if (!worst || e->max_allocated_seen > worst->max_allocated_seen) worst = e;
    }
    fprintf(stderr,
        "[wrapper_pool_resetter] op=%llu pools=%d unbounded=%d qgrow=%d "
        "(allocs=%llu resets=%llu now=%lld peak=%lld inflight=%lld would_reset=%llu)",
        (unsigned long long)op, n, n_unbounded, n_qgrow,
        (unsigned long long)total_allocs,
        (unsigned long long)total_resets,
        (long long)total_now,
        (long long)total_peak,
        (long long)total_inflight,
        (unsigned long long)total_would_reset);
    if (worst) {
        fprintf(stderr,
            " worst=%p (allocs=%llu frees=%llu resets=%llu now=%lld peak=%lld inflight=%lld would_reset=%llu maxSets=%u flags=0x%x)",
            worst->pool,
            (unsigned long long)worst->allocs_sets,
            (unsigned long long)worst->free_sets,
            (unsigned long long)worst->reset_calls,
            (long long)worst->allocated_now,
            (long long)worst->max_allocated_seen,
            (long long)worst->inflight_uses,
            (unsigned long long)worst->would_reset_count,
            worst->maxSets,
            worst->flags);
    }
    fputc('\n', stderr);
    fflush(stderr);
    pthread_mutex_unlock(&g_pools_mutex);
}

/* ================================================================
 *  v0 hooks (extended in v1 to update the set->pool table when active)
 * ================================================================ */

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
        if (enabled_v1() && e && pSets) {
            for (uint32_t i = 0; i < pInfo->descriptorSetCount; i++) {
                set_table_insert(pSets[i], e);
            }
        }
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
        if (enabled_v1() && pSets) {
            for (uint32_t i = 0; i < count; i++) set_table_remove(pSets[i]);
        }
    }
    return r;
}

__attribute__((visibility("default")))
VkResult PoolResetter_ResetDescriptorPool(
    VkDevice device, VkDescriptorPool pool, uint32_t flags) {
    VkResult r = next_ResetDescriptorPool(device, pool, flags);
    if (enabled() && r == 0) {
        uint64_t now_ns = enabled_v2() ? monotonic_ns() : 0;
        pthread_mutex_lock(&g_pools_mutex);
        PoolEntry *e = find_pool_locked(pool);
        if (e) {
            e->reset_calls++;
            e->allocated_now = 0;
            if (now_ns) e->last_reset_ns = now_ns;
        }
        pthread_mutex_unlock(&g_pools_mutex);
        if (enabled_v1() && e) set_table_purge_pool(e);
    }
    return r;
}

__attribute__((visibility("default")))
void PoolResetter_DestroyDescriptorPool(
    VkDevice device, VkDescriptorPool pool, const void *pAllocator) {
    if (enabled()) {
        pthread_mutex_lock(&g_pools_mutex);
        PoolEntry *e = find_pool_locked(pool);
        if (e) e->destroyed = 1;
        pthread_mutex_unlock(&g_pools_mutex);
    }
    next_DestroyDescriptorPool(device, pool, pAllocator);
}

/* ================================================================
 *  v1 hooks
 * ================================================================ */

__attribute__((visibility("default")))
VkResult PoolResetter_AllocateCommandBuffers(
    VkDevice device, const VkCommandBufferAllocateInfo *pInfo, VkCommandBuffer *pCmdBufs) {
    VkResult r = next_AllocateCommandBuffers(device, pInfo, pCmdBufs);
    if (enabled_v1() && r == 0 && pInfo && pCmdBufs) {
        for (uint32_t i = 0; i < pInfo->commandBufferCount; i++) {
            cmdbuf_register(pCmdBufs[i]);
        }
    }
    return r;
}

__attribute__((visibility("default")))
void PoolResetter_FreeCommandBuffers(
    VkDevice device, VkCommandPool pool, uint32_t count, const VkCommandBuffer *pCmdBufs) {
    if (enabled_v1() && pCmdBufs) {
        for (uint32_t i = 0; i < count; i++) cmdbuf_destroy(pCmdBufs[i]);
    }
    next_FreeCommandBuffers(device, pool, count, pCmdBufs);
}

__attribute__((visibility("default")))
VkResult PoolResetter_BeginCommandBuffer(VkCommandBuffer cb, const void *pInfo) {
    if (enabled_v1()) cmdbuf_clear(cb);
    return next_BeginCommandBuffer(cb, pInfo);
}

__attribute__((visibility("default")))
VkResult PoolResetter_ResetCommandBuffer(VkCommandBuffer cb, uint32_t flags) {
    if (enabled_v1()) cmdbuf_clear(cb);
    return next_ResetCommandBuffer(cb, flags);
}

__attribute__((visibility("default")))
void PoolResetter_CmdBindDescriptorSets(
    VkCommandBuffer cb, VkPipelineBindPoint bindPoint, VkPipelineLayout layout,
    uint32_t firstSet, uint32_t setCount, const VkDescriptorSet *pSets,
    uint32_t dynOffCount, const uint32_t *pDynOff) {
    next_CmdBindDescriptorSets(cb, bindPoint, layout, firstSet, setCount, pSets, dynOffCount, pDynOff);
    if (enabled_v1() && pSets && setCount > 0) {
        pthread_mutex_lock(&g_cmdbufs_mutex);
        CmdBufEntry *cbe = cmdbuf_get_or_create_locked(cb);
        if (cbe) {
            for (uint32_t i = 0; i < setCount; i++) {
                PoolEntry *pe = set_table_lookup(pSets[i]);  /* takes g_set_mutex */
                if (pe) cmdbuf_add_ref_locked(cbe, pe);
            }
        }
        pthread_mutex_unlock(&g_cmdbufs_mutex);
    }
}

__attribute__((visibility("default")))
VkResult PoolResetter_QueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo *pSubmits, VkFence fence) {
    /* Snapshot pool refs from each cmd buffer in the submit, increment
     * per-pool inflight_uses, and append to the fence's in-flight list.
     * Done BEFORE forwarding so the count is a strict upper bound during
     * the submit window. */
    if (enabled_v1() && fence && pSubmits && submitCount > 0) {
        for (uint32_t s = 0; s < submitCount; s++) {
            const VkSubmitInfo *si = &pSubmits[s];
            if (!si->pCommandBuffers) continue;
            for (uint32_t c = 0; c < si->commandBufferCount; c++) {
                VkCommandBuffer cb = si->pCommandBuffers[c];
                pthread_mutex_lock(&g_cmdbufs_mutex);
                CmdBufEntry *cbe = find_cmdbuf_locked(cb);
                int n = cbe ? cbe->pool_refs_count : 0;
                PoolEntry **snap = NULL;
                if (n > 0) {
                    snap = (PoolEntry**)malloc(n * sizeof(PoolEntry*));
                    if (snap) memcpy(snap, cbe->pool_refs, n * sizeof(PoolEntry*));
                }
                pthread_mutex_unlock(&g_cmdbufs_mutex);
                if (snap) {
                    pthread_mutex_lock(&g_pools_mutex);
                    for (int i = 0; i < n; i++) {
                        snap[i]->inflight_uses++;
                        if (snap[i]->inflight_uses > snap[i]->inflight_uses_peak)
                            snap[i]->inflight_uses_peak = snap[i]->inflight_uses;
                    }
                    pthread_mutex_unlock(&g_pools_mutex);
                    fence_add_refs(fence, snap, n);
                    free(snap);
                }
            }
        }
    }
    return next_QueueSubmit(queue, submitCount, pSubmits, fence);
}

__attribute__((visibility("default")))
VkResult PoolResetter_WaitForFences(VkDevice device, uint32_t count, const VkFence *pFences, VkBool32 waitAll, uint64_t timeout) {
    VkResult r = next_WaitForFences(device, count, pFences, waitAll, timeout);
    if (enabled_v1() && r == 0 && pFences) {
        /* SUCCESS means all waited fences are signaled. Heuristic: drain. */
        for (uint32_t i = 0; i < count; i++) fence_drain(pFences[i]);
    }
    return r;
}

__attribute__((visibility("default")))
VkResult PoolResetter_GetFenceStatus(VkDevice device, VkFence fence) {
    VkResult r = next_GetFenceStatus(device, fence);
    if (enabled_v1() && r == 0) fence_drain(fence);
    return r;
}

__attribute__((visibility("default")))
VkResult PoolResetter_ResetFences(VkDevice device, uint32_t count, const VkFence *pFences) {
    if (enabled_v1() && pFences) {
        for (uint32_t i = 0; i < count; i++) fence_drain(pFences[i]);
    }
    return next_ResetFences(device, count, pFences);
}

__attribute__((visibility("default")))
void PoolResetter_DestroyFence(VkDevice device, VkFence fence, const void *pAllocator) {
    if (enabled_v1()) fence_destroy(fence);
    next_DestroyFence(device, fence, pAllocator);
}

/* ================================================================
 *  procaddr / device-init scaffolding
 * ================================================================ */

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
        if (strcmp(pName, "vkAllocateCommandBuffers") == 0)
            return (PFN_vkVoidFunction)PoolResetter_AllocateCommandBuffers;
        if (strcmp(pName, "vkFreeCommandBuffers") == 0)
            return (PFN_vkVoidFunction)PoolResetter_FreeCommandBuffers;
        if (strcmp(pName, "vkBeginCommandBuffer") == 0)
            return (PFN_vkVoidFunction)PoolResetter_BeginCommandBuffer;
        if (strcmp(pName, "vkResetCommandBuffer") == 0)
            return (PFN_vkVoidFunction)PoolResetter_ResetCommandBuffer;
        if (strcmp(pName, "vkCmdBindDescriptorSets") == 0)
            return (PFN_vkVoidFunction)PoolResetter_CmdBindDescriptorSets;
        if (strcmp(pName, "vkQueueSubmit") == 0)
            return (PFN_vkVoidFunction)PoolResetter_QueueSubmit;
        if (strcmp(pName, "vkWaitForFences") == 0)
            return (PFN_vkVoidFunction)PoolResetter_WaitForFences;
        if (strcmp(pName, "vkGetFenceStatus") == 0)
            return (PFN_vkVoidFunction)PoolResetter_GetFenceStatus;
        if (strcmp(pName, "vkResetFences") == 0)
            return (PFN_vkVoidFunction)PoolResetter_ResetFences;
        if (strcmp(pName, "vkDestroyFence") == 0)
            return (PFN_vkVoidFunction)PoolResetter_DestroyFence;
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
    next_AllocateCommandBuffers = (PFN_vkAllocateCommandBuffers)next_GetDeviceProcAddr(*pDevice, "vkAllocateCommandBuffers");
    next_FreeCommandBuffers     = (PFN_vkFreeCommandBuffers)    next_GetDeviceProcAddr(*pDevice, "vkFreeCommandBuffers");
    next_BeginCommandBuffer     = (PFN_vkBeginCommandBuffer)    next_GetDeviceProcAddr(*pDevice, "vkBeginCommandBuffer");
    next_ResetCommandBuffer     = (PFN_vkResetCommandBuffer)    next_GetDeviceProcAddr(*pDevice, "vkResetCommandBuffer");
    next_CmdBindDescriptorSets  = (PFN_vkCmdBindDescriptorSets) next_GetDeviceProcAddr(*pDevice, "vkCmdBindDescriptorSets");
    next_QueueSubmit            = (PFN_vkQueueSubmit)           next_GetDeviceProcAddr(*pDevice, "vkQueueSubmit");
    next_WaitForFences          = (PFN_vkWaitForFences)         next_GetDeviceProcAddr(*pDevice, "vkWaitForFences");
    next_GetFenceStatus         = (PFN_vkGetFenceStatus)        next_GetDeviceProcAddr(*pDevice, "vkGetFenceStatus");
    next_ResetFences            = (PFN_vkResetFences)           next_GetDeviceProcAddr(*pDevice, "vkResetFences");
    next_DestroyFence           = (PFN_vkDestroyFence)          next_GetDeviceProcAddr(*pDevice, "vkDestroyFence");

    v2_init();

    if (enabled()) {
        fprintf(stderr,
                "[wrapper_pool_resetter] device created, hooks armed (v1=%s v2=%s",
                enabled_v1() ? "on" : "off",
                enabled_v2() ? "on" : "off");
        if (enabled_v2()) {
            fprintf(stderr, " threshold=%d grace_ms=%llu",
                    g_v2_threshold,
                    (unsigned long long)(g_v2_grace_ns / 1000000ULL));
        }
        fputs(")\n", stderr);
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
    /* device-level entrypoints can also be queried via instance procaddr;
     * delegate so the name table lives in one place. */
    PFN_vkVoidFunction p = PoolResetter_GetDeviceProcAddr(NULL, pName);
    if (p) return p;
    if (next_GetInstanceProcAddr) return next_GetInstanceProcAddr(instance, pName);
    return NULL;
}
