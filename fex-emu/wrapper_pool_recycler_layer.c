/*
 * VK_LAYER_WRAPPER_POOL_RECYCLER — pool consolidation for libbcn_layer's
 * GPU BCn compute decoder.
 *
 * Background (mali_alloc_probe data, 2026-04-24):
 * libbcn_layer creates ~36k descriptor pools during Sekiro texture-cache
 * warmup, one per BCn texture decoded. Signature: maxSets=32, exactly 2
 * sizes (VK_DESCRIPTOR_TYPE_STORAGE_IMAGE count=1, STORAGE_BUFFER count=1),
 * flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT. Each pool costs
 * ~1 /dev/mali0 mmap on the leegao wrapper, so 36k pools ≈ 39k mali0 VMAs
 * — most of the kernel-VMA-tree pressure that bottlenecks frametime.
 *
 * Strategy: when libbcn_layer calls vkCreateDescriptorPool with the BCn
 * signature, return a fake handle backed by a shared "real pool" with
 * maxSets=REAL_POOL_MAX_SETS (default 4096). Allocations from the fake
 * handle are routed to the real pool; sets are tracked per-fake so reset/
 * destroy can free precisely. With 36k textures sharing 4096-set pools,
 * we need ~9 real pools instead of 36k → mali0 drops from ~40k to ~10k.
 *
 * Pools NOT matching the BCn signature pass through untouched (DXVK's
 * 8192-set 9-type pool, etc).
 *
 * Activate via WRAPPER_POOL_RECYCLER=1.
 *
 * Build:
 *   $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang \
 *     -shared -fPIC -O2 -o libwrapper_pool_recycler.so wrapper_pool_recycler_layer.c
 *
 * Deploy:
 *   files/imagefs_bionic/usr/lib/libwrapper_pool_recycler.so
 *   files/imagefs_bionic/usr/share/vulkan/implicit_layer.d/wrapper_pool_recycler_layer.json
 *
 * Layer name 'w' sorts after libbcn_layer ('l') so it loads BELOW BCn in
 * the implicit-layer chain — BCn's calls flow down through us. Verified
 * via the same observation that put mali_alloc_probe ('m') below BCn.
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
typedef void *VkPhysicalDevice;
typedef void *VkDevice;
typedef uint64_t VkDescriptorPool;
typedef uint64_t VkDescriptorSet;
typedef uint64_t VkDescriptorSetLayout;
typedef uint32_t VkBool32;

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

typedef struct VkDescriptorSetAllocateInfo {
    int32_t       sType;
    int32_t       _pad0;
    const void   *pNext;
    VkDescriptorPool descriptorPool;
    uint32_t      descriptorSetCount;
    uint32_t      _pad1;
    const VkDescriptorSetLayout *pSetLayouts;
} VkDescriptorSetAllocateInfo;

typedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddr)(VkInstance, const char*);
typedef PFN_vkVoidFunction (*PFN_vkGetDeviceProcAddr)(VkDevice, const char*);
typedef VkResult (*PFN_vkCreateInstance)(const void*, const void*, VkInstance*);
typedef VkResult (*PFN_vkCreateDevice)(VkPhysicalDevice, const void*, const void*, VkDevice*);
typedef VkResult (*PFN_vkCreateDescriptorPool)(VkDevice, const VkDescriptorPoolCreateInfo*, const void*, VkDescriptorPool*);
typedef VkResult (*PFN_vkResetDescriptorPool)(VkDevice, VkDescriptorPool, uint32_t);
typedef void     (*PFN_vkDestroyDescriptorPool)(VkDevice, VkDescriptorPool, const void*);
typedef VkResult (*PFN_vkAllocateDescriptorSets)(VkDevice, const VkDescriptorSetAllocateInfo*, VkDescriptorSet*);
typedef VkResult (*PFN_vkFreeDescriptorSets)(VkDevice, VkDescriptorPool, uint32_t, const VkDescriptorSet*);

#define VK_DESCRIPTOR_TYPE_STORAGE_IMAGE  3
#define VK_DESCRIPTOR_TYPE_STORAGE_BUFFER 7
#define VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT 0x1u

/* ===== state ===== */

#define REAL_POOL_MAX_SETS    4096    /* sets per real pool */
#define MAX_REAL_POOLS         128
#define FAKE_HASH_SIZE       65536    /* power of 2; load 36k → 55% */
#define FAKE_HASH_MASK       (FAKE_HASH_SIZE - 1)
#define BCN_FAKE_INIT_CAP       32    /* initial per-fake set-tracking capacity; grows dynamically */
#define LOG_FIRST_OPS         100u    /* verbose log first N ops, then quiet */

struct RealPool {
    VkDevice         device;
    VkDescriptorPool real;
    uint32_t         sets_used;
};

/* Per-fake set tracking: dynamic array. The wrapper requested a pool with
 * maxSets=32, but because we share a real pool across many fakes, we route
 * the wrapper's allocations to the real pool's larger capacity. The per-fake
 * tracker grows as needed so reset/destroy can precisely free sets that
 * came from THIS fake (real pool keeps the rest alive). */
struct FakePool {
    int              real_idx;        /* index into real_pools[] */
    uint32_t         set_count;
    uint32_t         set_capacity;
    VkDescriptorSet *sets;
};

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static struct RealPool g_real_pools[MAX_REAL_POOLS];
static int             g_real_pools_count = 0;

/* Open-addressed hash set: keys are fake VkDescriptorPool values (which are
 * the malloc'd FakePool*). 0 = empty slot. Real wrapper pools won't be 0. */
static VkDescriptorPool g_fake_keys[FAKE_HASH_SIZE];
static struct FakePool *g_fake_vals[FAKE_HASH_SIZE];
static uint32_t         g_fake_count = 0;

/* Hooks captured during chain walk. Single-device assumption — DXVK creates
 * one device, and so do nearly all real apps. If multi-device shows up the
 * last-wins behaviour will silently be wrong; we'd need per-device tables. */
static PFN_vkVoidFunction (*next_GetInstanceProcAddr)(VkInstance, const char*) = NULL;
static PFN_vkVoidFunction (*next_GetDeviceProcAddr)(VkDevice, const char*) = NULL;

static PFN_vkCreateDescriptorPool   next_CreateDescriptorPool   = NULL;
static PFN_vkResetDescriptorPool    next_ResetDescriptorPool    = NULL;
static PFN_vkDestroyDescriptorPool  next_DestroyDescriptorPool  = NULL;
static PFN_vkAllocateDescriptorSets next_AllocateDescriptorSets = NULL;
static PFN_vkFreeDescriptorSets     next_FreeDescriptorSets     = NULL;

/* Stats (atomic, lock-free read). */
static _Atomic uint64_t s_fakes_created   = 0;
static _Atomic uint64_t s_fakes_destroyed = 0;
static _Atomic uint64_t s_real_created    = 0;
static _Atomic uint64_t s_sets_alloc      = 0;
static _Atomic uint64_t s_sets_free       = 0;
static _Atomic uint64_t s_resets          = 0;
static _Atomic uint64_t s_bypass_creates  = 0;
static _Atomic uint64_t s_op_counter      = 0;

static int recycler_enabled(void) {
    const char *e = getenv("WRAPPER_POOL_RECYCLER");
    return e && e[0] == '1';
}

static int verbose_op(void) {
    /* Verbose for the first LOG_FIRST_OPS ops, then every 5000th. */
    uint64_t op = atomic_fetch_add_explicit(&s_op_counter, 1, memory_order_relaxed) + 1;
    return op <= LOG_FIRST_OPS || (op % 5000) == 0;
}

/* ===== signature match ===== */

static int is_bcn_signature(const VkDescriptorPoolCreateInfo *ci) {
    if (!ci) return 0;
    if (ci->maxSets != 32) return 0;
    if (ci->poolSizeCount != 2) return 0;
    if ((ci->flags & VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT) == 0) return 0;
    if (!ci->pPoolSizes) return 0;
    int has_si = 0, has_sb = 0;
    for (uint32_t i = 0; i < 2; i++) {
        int t = ci->pPoolSizes[i].type;
        uint32_t c = ci->pPoolSizes[i].descriptorCount;
        if (t == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE  && c == 1) has_si = 1;
        if (t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && c == 1) has_sb = 1;
    }
    return has_si && has_sb;
}

/* ===== hash set ===== */

static uint64_t hash64(uint64_t x) {
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    x = x ^ (x >> 31);
    return x;
}

/* Caller holds g_mu. */
static struct FakePool *fake_lookup_locked(VkDescriptorPool h) {
    if (!h) return NULL;
    uint64_t i = hash64((uint64_t)h) & FAKE_HASH_MASK;
    /* Bound the probe to FAKE_HASH_SIZE in case the table is full. */
    for (uint32_t step = 0; step < FAKE_HASH_SIZE; step++) {
        VkDescriptorPool k = g_fake_keys[i];
        if (k == 0) return NULL;
        if (k == h) return g_fake_vals[i];
        i = (i + 1) & FAKE_HASH_MASK;
    }
    return NULL;
}

/* Caller holds g_mu. Returns 0 on success, -1 on table-full. */
static int fake_insert_locked(VkDescriptorPool h, struct FakePool *fp) {
    uint64_t i = hash64((uint64_t)h) & FAKE_HASH_MASK;
    for (uint32_t step = 0; step < FAKE_HASH_SIZE; step++) {
        if (g_fake_keys[i] == 0) {
            g_fake_keys[i] = h;
            g_fake_vals[i] = fp;
            g_fake_count++;
            return 0;
        }
        i = (i + 1) & FAKE_HASH_MASK;
    }
    return -1;
}

/* Caller holds g_mu. Open-addressing tombstone-free remove via backshift. */
static void fake_remove_locked(VkDescriptorPool h) {
    uint64_t i = hash64((uint64_t)h) & FAKE_HASH_MASK;
    for (uint32_t step = 0; step < FAKE_HASH_SIZE; step++) {
        if (g_fake_keys[i] == 0) return;          /* not present */
        if (g_fake_keys[i] == h) break;
        i = (i + 1) & FAKE_HASH_MASK;
    }
    /* Backshift loop: walk forward, moving entries that hash to <= i. */
    uint64_t j = i;
    g_fake_keys[i] = 0;
    g_fake_vals[i] = NULL;
    g_fake_count--;
    for (uint32_t step = 0; step < FAKE_HASH_SIZE; step++) {
        j = (j + 1) & FAKE_HASH_MASK;
        VkDescriptorPool k = g_fake_keys[j];
        if (k == 0) return;
        uint64_t natural = hash64((uint64_t)k) & FAKE_HASH_MASK;
        /* Distance from natural to current j, vs natural to i. If i is
         * "closer" to natural in the probe order, the entry can move back
         * to i without breaking lookups. */
        uint64_t dist_j = (j - natural) & FAKE_HASH_MASK;
        uint64_t dist_i = (i - natural) & FAKE_HASH_MASK;
        if (dist_i < dist_j) {
            g_fake_keys[i] = k;
            g_fake_vals[i] = g_fake_vals[j];
            g_fake_keys[j] = 0;
            g_fake_vals[j] = NULL;
            i = j;
        }
    }
}

/* ===== real pool management ===== */

/* Caller holds g_mu. Creates a new real pool on the given device. */
static int real_pool_create_locked(VkDevice device) {
    if (g_real_pools_count >= MAX_REAL_POOLS) return -1;

    VkDescriptorPoolSize sizes[2];
    sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    sizes[0].descriptorCount = REAL_POOL_MAX_SETS;
    sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sizes[1].descriptorCount = REAL_POOL_MAX_SETS;

    VkDescriptorPoolCreateInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.sType         = 33; /* VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO */
    ci.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    ci.maxSets       = REAL_POOL_MAX_SETS;
    ci.poolSizeCount = 2;
    ci.pPoolSizes    = sizes;

    VkDescriptorPool real = 0;
    VkResult r = next_CreateDescriptorPool(device, &ci, NULL, &real);
    if (r != 0 || real == 0) {
        fprintf(stderr, "[wrapper_pool_recycler] real pool create FAILED r=%d\n", r);
        fflush(stderr);
        return -1;
    }
    int idx = g_real_pools_count++;
    g_real_pools[idx].device    = device;
    g_real_pools[idx].real      = real;
    g_real_pools[idx].sets_used = 0;
    atomic_fetch_add_explicit(&s_real_created, 1, memory_order_relaxed);
    fprintf(stderr,
            "[wrapper_pool_recycler] created real pool #%d handle=%llx (capacity=%u)\n",
            idx, (unsigned long long)real, (unsigned)REAL_POOL_MAX_SETS);
    fflush(stderr);
    return idx;
}

/* Caller holds g_mu. Returns idx of a real pool with at least `need` set
 * capacity remaining, creating one if none exists. */
static int find_or_create_real_pool_locked(VkDevice device, uint32_t need) {
    for (int i = 0; i < g_real_pools_count; i++) {
        if (g_real_pools[i].device != device) continue;
        if (g_real_pools[i].sets_used + need <= REAL_POOL_MAX_SETS) return i;
    }
    return real_pool_create_locked(device);
}

/* ===== stats logging ===== */

static void maybe_log_stats(const char *tag, VkDescriptorPool fake, VkDescriptorPool real, uint32_t count) {
    if (!verbose_op()) return;
    fprintf(stderr,
            "[wrapper_pool_recycler] %s fake=%llx real=%llx count=%u "
            "stats(fakes_c=%llu fakes_d=%llu real_c=%llu sets_a=%llu sets_f=%llu resets=%llu bypass=%llu)\n",
            tag,
            (unsigned long long)fake,
            (unsigned long long)real,
            count,
            (unsigned long long)atomic_load_explicit(&s_fakes_created,   memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&s_fakes_destroyed, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&s_real_created,    memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&s_sets_alloc,      memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&s_sets_free,       memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&s_resets,          memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&s_bypass_creates,  memory_order_relaxed));
    fflush(stderr);
}

/* ===== hooks ===== */

__attribute__((visibility("default")))
VkResult WPR_CreateDescriptorPool(
    VkDevice device, const VkDescriptorPoolCreateInfo *pCreateInfo,
    const void *pAllocator, VkDescriptorPool *pPool) {

    if (!recycler_enabled() || !is_bcn_signature(pCreateInfo)) {
        atomic_fetch_add_explicit(&s_bypass_creates, 1, memory_order_relaxed);
        return next_CreateDescriptorPool(device, pCreateInfo, pAllocator, pPool);
    }

    /* BCn signature → consolidate. */
    struct FakePool *fp = (struct FakePool*)calloc(1, sizeof(*fp));
    if (!fp) return -1; /* VK_ERROR_OUT_OF_HOST_MEMORY */
    fp->sets = (VkDescriptorSet*)calloc(BCN_FAKE_INIT_CAP, sizeof(VkDescriptorSet));
    if (!fp->sets) { free(fp); return -1; }
    fp->set_capacity = BCN_FAKE_INIT_CAP;
    fp->set_count = 0;

    pthread_mutex_lock(&g_mu);
    int idx = find_or_create_real_pool_locked(device, 1);
    if (idx < 0) {
        pthread_mutex_unlock(&g_mu);
        free(fp->sets);
        free(fp);
        /* Fallback: pass through to the wrapper so we don't break BCn. */
        atomic_fetch_add_explicit(&s_bypass_creates, 1, memory_order_relaxed);
        return next_CreateDescriptorPool(device, pCreateInfo, pAllocator, pPool);
    }
    fp->real_idx = idx;

    /* Insert into hash table; fake handle = fp pointer. */
    VkDescriptorPool fake = (VkDescriptorPool)(uintptr_t)fp;
    if (fake_insert_locked(fake, fp) != 0) {
        pthread_mutex_unlock(&g_mu);
        free(fp);
        atomic_fetch_add_explicit(&s_bypass_creates, 1, memory_order_relaxed);
        return next_CreateDescriptorPool(device, pCreateInfo, pAllocator, pPool);
    }
    VkDescriptorPool real = g_real_pools[idx].real;
    pthread_mutex_unlock(&g_mu);

    *pPool = fake;
    atomic_fetch_add_explicit(&s_fakes_created, 1, memory_order_relaxed);
    maybe_log_stats("CreatePool", fake, real, 0);
    return 0;
}

__attribute__((visibility("default")))
VkResult WPR_AllocateDescriptorSets(
    VkDevice device, const VkDescriptorSetAllocateInfo *pInfo, VkDescriptorSet *pSets) {

    if (!recycler_enabled() || !pInfo) {
        return next_AllocateDescriptorSets(device, pInfo, pSets);
    }

    pthread_mutex_lock(&g_mu);
    struct FakePool *fp = fake_lookup_locked(pInfo->descriptorPool);
    if (!fp) {
        pthread_mutex_unlock(&g_mu);
        return next_AllocateDescriptorSets(device, pInfo, pSets);
    }

    uint32_t want = pInfo->descriptorSetCount;

    /* Grow per-fake tracking array if needed. The wrapper requested
     * maxSets=32 but our consolidated real pool has 4096 capacity, so we
     * routinely accept >32 allocs per fake. */
    if (fp->set_count + want > fp->set_capacity) {
        uint32_t newcap = fp->set_capacity ? fp->set_capacity : BCN_FAKE_INIT_CAP;
        while (newcap < fp->set_count + want) newcap *= 2;
        VkDescriptorSet *grown = (VkDescriptorSet*)realloc(
            fp->sets, newcap * sizeof(VkDescriptorSet));
        if (!grown) {
            pthread_mutex_unlock(&g_mu);
            return -1; /* VK_ERROR_OUT_OF_HOST_MEMORY */
        }
        fp->sets = grown;
        fp->set_capacity = newcap;
    }

    /* If the current real pool can't satisfy this alloc, find or create
     * another. Real pool capacity is REAL_POOL_MAX_SETS (4096), so 36k
     * texture decodes spread across ~9 real pools. */
    if (g_real_pools[fp->real_idx].sets_used + want > REAL_POOL_MAX_SETS) {
        int nidx = find_or_create_real_pool_locked(g_real_pools[fp->real_idx].device, want);
        if (nidx < 0) {
            pthread_mutex_unlock(&g_mu);
            return -3; /* VK_ERROR_OUT_OF_POOL_MEMORY */
        }
        fp->real_idx = nidx;
    }

    /* Build a temp AllocateInfo with the real pool. */
    VkDescriptorSetAllocateInfo info = *pInfo;
    info.descriptorPool = g_real_pools[fp->real_idx].real;

    VkDevice realdev = g_real_pools[fp->real_idx].device;
    /* Forward while still holding the lock — vkAllocateDescriptorSets must
     * be externally synchronised on its pool, and our real pool is shared
     * across many fake pools, so this is the synchronisation point. */
    VkResult r = next_AllocateDescriptorSets(realdev, &info, pSets);
    if (r == 0) {
        for (uint32_t i = 0; i < want; i++) {
            fp->sets[fp->set_count++] = pSets[i];
        }
        g_real_pools[fp->real_idx].sets_used += want;
        atomic_fetch_add_explicit(&s_sets_alloc, want, memory_order_relaxed);
    }
    VkDescriptorPool real = g_real_pools[fp->real_idx].real;
    pthread_mutex_unlock(&g_mu);

    if (verbose_op()) {
        fprintf(stderr,
                "[wrapper_pool_recycler] AllocSets r=%d fake=%llx real=%llx count=%u\n",
                r, (unsigned long long)pInfo->descriptorPool, (unsigned long long)real, want);
        fflush(stderr);
    }
    return r;
}

__attribute__((visibility("default")))
VkResult WPR_FreeDescriptorSets(
    VkDevice device, VkDescriptorPool pool, uint32_t count, const VkDescriptorSet *pSets) {

    if (!recycler_enabled()) {
        return next_FreeDescriptorSets(device, pool, count, pSets);
    }

    pthread_mutex_lock(&g_mu);
    struct FakePool *fp = fake_lookup_locked(pool);
    if (!fp) {
        pthread_mutex_unlock(&g_mu);
        return next_FreeDescriptorSets(device, pool, count, pSets);
    }
    VkDescriptorPool real = g_real_pools[fp->real_idx].real;
    VkDevice realdev      = g_real_pools[fp->real_idx].device;

    /* Drop tracked sets. */
    for (uint32_t i = 0; i < count; i++) {
        for (uint32_t j = 0; j < fp->set_count; j++) {
            if (fp->sets[j] == pSets[i]) {
                fp->sets[j] = fp->sets[--fp->set_count];
                break;
            }
        }
    }
    if (g_real_pools[fp->real_idx].sets_used >= count)
        g_real_pools[fp->real_idx].sets_used -= count;
    else
        g_real_pools[fp->real_idx].sets_used = 0;
    atomic_fetch_add_explicit(&s_sets_free, count, memory_order_relaxed);

    VkResult r = next_FreeDescriptorSets(realdev, real, count, pSets);
    pthread_mutex_unlock(&g_mu);
    return r;
}

__attribute__((visibility("default")))
VkResult WPR_ResetDescriptorPool(VkDevice device, VkDescriptorPool pool, uint32_t flags) {
    if (!recycler_enabled()) {
        return next_ResetDescriptorPool(device, pool, flags);
    }

    pthread_mutex_lock(&g_mu);
    struct FakePool *fp = fake_lookup_locked(pool);
    if (!fp) {
        pthread_mutex_unlock(&g_mu);
        return next_ResetDescriptorPool(device, pool, flags);
    }
    /* Free all the sets this fake pool currently holds, but DON'T reset the
     * real pool — that would clobber other fake pools sharing the same real. */
    VkResult r = 0;
    if (fp->set_count > 0) {
        VkDescriptorPool real = g_real_pools[fp->real_idx].real;
        VkDevice realdev      = g_real_pools[fp->real_idx].device;
        r = next_FreeDescriptorSets(realdev, real, fp->set_count, fp->sets);
        if (g_real_pools[fp->real_idx].sets_used >= fp->set_count)
            g_real_pools[fp->real_idx].sets_used -= fp->set_count;
        else
            g_real_pools[fp->real_idx].sets_used = 0;
        atomic_fetch_add_explicit(&s_sets_free, fp->set_count, memory_order_relaxed);
    }
    fp->set_count = 0;
    atomic_fetch_add_explicit(&s_resets, 1, memory_order_relaxed);
    pthread_mutex_unlock(&g_mu);
    return r;
}

__attribute__((visibility("default")))
void WPR_DestroyDescriptorPool(VkDevice device, VkDescriptorPool pool, const void *pAllocator) {
    if (!recycler_enabled()) {
        next_DestroyDescriptorPool(device, pool, pAllocator);
        return;
    }

    pthread_mutex_lock(&g_mu);
    struct FakePool *fp = fake_lookup_locked(pool);
    if (!fp) {
        pthread_mutex_unlock(&g_mu);
        next_DestroyDescriptorPool(device, pool, pAllocator);
        return;
    }
    /* Free outstanding sets back to the real pool, leave the real pool alive. */
    if (fp->set_count > 0) {
        VkDescriptorPool real = g_real_pools[fp->real_idx].real;
        VkDevice realdev      = g_real_pools[fp->real_idx].device;
        next_FreeDescriptorSets(realdev, real, fp->set_count, fp->sets);
        if (g_real_pools[fp->real_idx].sets_used >= fp->set_count)
            g_real_pools[fp->real_idx].sets_used -= fp->set_count;
        else
            g_real_pools[fp->real_idx].sets_used = 0;
        atomic_fetch_add_explicit(&s_sets_free, fp->set_count, memory_order_relaxed);
    }
    fake_remove_locked(pool);
    pthread_mutex_unlock(&g_mu);
    free(fp->sets);
    free(fp);
    atomic_fetch_add_explicit(&s_fakes_destroyed, 1, memory_order_relaxed);
    maybe_log_stats("DestroyPool", pool, 0, 0);
}

/* ===== chain init ===== */

__attribute__((visibility("default")))
PFN_vkVoidFunction WPR_GetDeviceProcAddr(VkDevice device, const char *pName) {
    if (pName) {
        if (strcmp(pName, "vkCreateDescriptorPool") == 0)
            return (PFN_vkVoidFunction)WPR_CreateDescriptorPool;
        if (strcmp(pName, "vkResetDescriptorPool") == 0)
            return (PFN_vkVoidFunction)WPR_ResetDescriptorPool;
        if (strcmp(pName, "vkDestroyDescriptorPool") == 0)
            return (PFN_vkVoidFunction)WPR_DestroyDescriptorPool;
        if (strcmp(pName, "vkAllocateDescriptorSets") == 0)
            return (PFN_vkVoidFunction)WPR_AllocateDescriptorSets;
        if (strcmp(pName, "vkFreeDescriptorSets") == 0)
            return (PFN_vkVoidFunction)WPR_FreeDescriptorSets;
    }
    if (next_GetDeviceProcAddr) return next_GetDeviceProcAddr(device, pName);
    return NULL;
}

__attribute__((visibility("default")))
VkResult WPR_CreateDevice(
    VkPhysicalDevice pd, const void *pCreateInfo, const void *pAllocator, VkDevice *pDevice) {

    /* Walk the device-create-info chain to find LOADER_DEVICE_CREATE_INFO. */
    typedef struct chain_hdr { int32_t sType; const void *pNext; int32_t function; void *u; } chain_hdr;
    const chain_hdr *h = (const chain_hdr*)((const void**)pCreateInfo)[1];
    while (h && !(h->sType == 48 && h->function == 0))
        h = (const chain_hdr*)h->pNext;
    if (!h || !h->u) return -3;

    VkLayerDeviceLink *link = (VkLayerDeviceLink*)h->u;
    PFN_vkVoidFunction (*gipa)(VkInstance, const char*) = link->pfnNextGetInstanceProcAddr;
    next_GetDeviceProcAddr = link->pfnNextGetDeviceProcAddr;

    /* Advance the chain so the next layer sees its link. */
    ((chain_hdr*)h)->u = link->pNext;

    PFN_vkCreateDevice createNext = (PFN_vkCreateDevice)gipa(NULL, "vkCreateDevice");
    if (!createNext) return -3;
    VkResult r = createNext(pd, pCreateInfo, pAllocator, pDevice);
    if (r != 0) return r;

    next_CreateDescriptorPool   = (PFN_vkCreateDescriptorPool)  next_GetDeviceProcAddr(*pDevice, "vkCreateDescriptorPool");
    next_ResetDescriptorPool    = (PFN_vkResetDescriptorPool)   next_GetDeviceProcAddr(*pDevice, "vkResetDescriptorPool");
    next_DestroyDescriptorPool  = (PFN_vkDestroyDescriptorPool) next_GetDeviceProcAddr(*pDevice, "vkDestroyDescriptorPool");
    next_AllocateDescriptorSets = (PFN_vkAllocateDescriptorSets)next_GetDeviceProcAddr(*pDevice, "vkAllocateDescriptorSets");
    next_FreeDescriptorSets     = (PFN_vkFreeDescriptorSets)    next_GetDeviceProcAddr(*pDevice, "vkFreeDescriptorSets");

    if (recycler_enabled()) {
        fprintf(stderr,
                "[wrapper_pool_recycler] device created, hooks armed "
                "(REAL_POOL_MAX_SETS=%u BCN_FAKE_INIT_CAP=%u)\n",
                (unsigned)REAL_POOL_MAX_SETS, (unsigned)BCN_FAKE_INIT_CAP);
        fflush(stderr);
    }
    return r;
}

__attribute__((visibility("default")))
VkResult WPR_CreateInstance(const void *pCreateInfo, const void *pAllocator, VkInstance *pInstance) {
    typedef struct chain_hdr { int32_t sType; const void *pNext; int32_t function; void *u; } chain_hdr;
    const chain_hdr *h = (const chain_hdr*)((const void**)pCreateInfo)[1];
    while (h && !(h->sType == 47 && h->function == 0))
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

    if (recycler_enabled()) {
        fprintf(stderr, "[wrapper_pool_recycler] instance created\n");
        fflush(stderr);
    }
    return r;
}

__attribute__((visibility("default")))
PFN_vkVoidFunction WPR_GetInstanceProcAddr(VkInstance instance, const char *pName) {
    if (!pName) return NULL;
    if (strcmp(pName, "vkCreateInstance") == 0)
        return (PFN_vkVoidFunction)WPR_CreateInstance;
    if (strcmp(pName, "vkGetInstanceProcAddr") == 0)
        return (PFN_vkVoidFunction)WPR_GetInstanceProcAddr;
    if (strcmp(pName, "vkCreateDevice") == 0)
        return (PFN_vkVoidFunction)WPR_CreateDevice;
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0)
        return (PFN_vkVoidFunction)WPR_GetDeviceProcAddr;
    if (strcmp(pName, "vkCreateDescriptorPool") == 0)
        return (PFN_vkVoidFunction)WPR_CreateDescriptorPool;
    if (strcmp(pName, "vkResetDescriptorPool") == 0)
        return (PFN_vkVoidFunction)WPR_ResetDescriptorPool;
    if (strcmp(pName, "vkDestroyDescriptorPool") == 0)
        return (PFN_vkVoidFunction)WPR_DestroyDescriptorPool;
    if (strcmp(pName, "vkAllocateDescriptorSets") == 0)
        return (PFN_vkVoidFunction)WPR_AllocateDescriptorSets;
    if (strcmp(pName, "vkFreeDescriptorSets") == 0)
        return (PFN_vkVoidFunction)WPR_FreeDescriptorSets;
    if (next_GetInstanceProcAddr) return next_GetInstanceProcAddr(instance, pName);
    return NULL;
}
