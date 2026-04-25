/*
 * VK_LAYER_DEDUP_PROBE — phase 0 instrumentation layer.
 *
 * Hooks vkUpdateDescriptorSets, hashes the writes' content, tracks how
 * often the SAME content recurs across update calls. Decides whether the
 * full descriptor-set deduplication direction is worth the engineering.
 *
 * If the dup_rate logged here is <10%, dedup won't move the needle — the
 * descriptor-set churn we measured (234 allocs/submit on tablet vs PC's
 * 0.07) is genuinely producing unique content per allocation, and dedup
 * has nothing to merge.
 *
 * If dup_rate is >50%, dedup is the right lever; phase 1 implements
 * cache-and-swap.
 *
 * Build: aarch64-linux-android28-clang -shared -fPIC -O2 \
 *        -o libdedup_probe.so dedup_probe_layer.c
 *
 * Activate via env: DEDUP_PROBE=1
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>

/* Minimal Vulkan typedefs (avoid pulling vulkan.h to keep this self-contained
 * like feature_spoof_layer.c). */
typedef int VkResult;
typedef void *VkInstance;
typedef void *VkDevice;
typedef void *VkPhysicalDevice;
typedef void *VkDescriptorSet;
typedef void *VkBuffer;
typedef void *VkBufferView;
typedef void *VkImageView;
typedef void *VkSampler;
typedef int  VkDescriptorType;
typedef int  VkImageLayout;

typedef struct {
    int32_t       sType;
    const void   *pNext;
    VkDescriptorSet dstSet;
    uint32_t      dstBinding;
    uint32_t      dstArrayElement;
    uint32_t      descriptorCount;
    VkDescriptorType descriptorType;
    const void   *pImageInfo;          /* VkDescriptorImageInfo[] */
    const void   *pBufferInfo;         /* VkDescriptorBufferInfo[] */
    const void   *pTexelBufferView;    /* VkBufferView[]          */
} VkWriteDescriptorSet;

typedef struct {
    int32_t       sType;
    const void   *pNext;
    VkDescriptorSet srcSet;
    uint32_t      srcBinding;
    uint32_t      srcArrayElement;
    VkDescriptorSet dstSet;
    uint32_t      dstBinding;
    uint32_t      dstArrayElement;
    uint32_t      descriptorCount;
} VkCopyDescriptorSet;

typedef struct {                /* VkDescriptorImageInfo  */
    VkSampler     sampler;
    VkImageView   imageView;
    VkImageLayout imageLayout;
} VkDescriptorImageInfo;

typedef struct {                /* VkDescriptorBufferInfo */
    VkBuffer      buffer;
    uint64_t      offset;
    uint64_t      range;
} VkDescriptorBufferInfo;

typedef void (*PFN_vkVoidFunction)(void);
typedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddr)(VkInstance, const char*);
typedef PFN_vkVoidFunction (*PFN_vkGetDeviceProcAddr)(VkDevice, const char*);
typedef VkResult (*PFN_vkCreateInstance)(const void*, const void*, VkInstance*);
typedef VkResult (*PFN_vkCreateDevice)(VkPhysicalDevice, const void*, const void*, VkDevice*);
typedef void (*PFN_vkUpdateDescriptorSets)(
    VkDevice, uint32_t, const VkWriteDescriptorSet*,
              uint32_t, const VkCopyDescriptorSet*);

/* Loader chain link layout — same as feature_spoof_layer.c. */
typedef struct VkLayerDeviceLink_ {
    struct VkLayerDeviceLink_ *pNext;
    PFN_vkVoidFunction (*pfnNextGetInstanceProcAddr)(VkInstance, const char*);
    PFN_vkVoidFunction (*pfnNextGetDeviceProcAddr)(VkDevice, const char*);
} VkLayerDeviceLink;

typedef struct {
    int32_t sType;
    const void *pNext;
    int32_t function;
    union { VkLayerDeviceLink *pLayerInfo; void *_pad; } u;
} VkLayerDeviceCreateInfo;

typedef struct {
    int32_t sType;
    const void *pNext;
    int32_t function;
    union { struct {
        PFN_vkVoidFunction (*pfnNextGetInstanceProcAddr)(VkInstance, const char*);
    } *pLayerInfo; void *_pad; } u;
} VkLayerInstanceCreateInfo;

/* Chain state. */
static PFN_vkVoidFunction (*next_GetInstanceProcAddr)(VkInstance, const char*) = NULL;
static PFN_vkVoidFunction (*next_GetDeviceProcAddr)(VkDevice, const char*) = NULL;
static PFN_vkUpdateDescriptorSets next_UpdateDescriptorSets = NULL;

static int probe_enabled(void) {
    const char *e = getenv("DEDUP_PROBE");
    return e && e[0] == '1';
}

/* Hash table for seen content hashes. Open-addressed, fixed size, lossy
 * if overflowed (overwrites slots). 1M entries * 8 bytes = 8 MB. Plenty
 * for a measurement run. */
#define HT_SIZE (1u << 20)
static _Atomic uint64_t ht[HT_SIZE];

static _Atomic uint64_t total_writes = 0;       /* every VkWriteDescriptorSet */
static _Atomic uint64_t total_calls = 0;        /* every vkUpdateDescriptorSets call */
static _Atomic uint64_t dup_writes = 0;         /* writes whose content hash was already seen */
static _Atomic uint64_t unique_writes = 0;      /* first sighting of this hash */

#define LOG_EVERY 5000

/* splitmix64 — fast, decent distribution. */
static inline uint64_t mix64(uint64_t x) {
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    x = (x ^ (x >> 31));
    return x;
}

static inline uint64_t hash_combine(uint64_t a, uint64_t b) {
    return mix64(a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2)));
}

/* Hash a single VkWriteDescriptorSet's CONTENT (not dstSet — we want
 * to know if the same content is being written to different sets, which
 * is the dedup opportunity). */
static uint64_t hash_write(const VkWriteDescriptorSet *w) {
    uint64_t h = (uint64_t)w->descriptorType;
    h = hash_combine(h, (uint64_t)w->descriptorCount);
    h = hash_combine(h, (uint64_t)w->dstBinding);
    h = hash_combine(h, (uint64_t)w->dstArrayElement);

    /* Image-type descriptors */
    if (w->pImageInfo) {
        const VkDescriptorImageInfo *imgs = (const VkDescriptorImageInfo*)w->pImageInfo;
        for (uint32_t i = 0; i < w->descriptorCount; i++) {
            h = hash_combine(h, (uintptr_t)imgs[i].sampler);
            h = hash_combine(h, (uintptr_t)imgs[i].imageView);
            h = hash_combine(h, (uint64_t)imgs[i].imageLayout);
        }
    }
    /* Buffer-type descriptors */
    if (w->pBufferInfo) {
        const VkDescriptorBufferInfo *bufs = (const VkDescriptorBufferInfo*)w->pBufferInfo;
        for (uint32_t i = 0; i < w->descriptorCount; i++) {
            h = hash_combine(h, (uintptr_t)bufs[i].buffer);
            h = hash_combine(h, bufs[i].offset);
            h = hash_combine(h, bufs[i].range);
        }
    }
    /* Texel buffer views (rare in practice, but include). */
    if (w->pTexelBufferView) {
        const VkBufferView *tbvs = (const VkBufferView*)w->pTexelBufferView;
        for (uint32_t i = 0; i < w->descriptorCount; i++)
            h = hash_combine(h, (uintptr_t)tbvs[i]);
    }
    return h ? h : 1; /* never 0 — 0 = empty slot */
}

/* Returns 1 if hash was already in the table (dup), 0 if first sighting. */
static int ht_seen(uint64_t h) {
    uint64_t idx = h & (HT_SIZE - 1);
    /* Linear probe a few slots — keeps the whole probe fast and bounded. */
    for (int i = 0; i < 4; i++) {
        uint64_t slot_idx = (idx + i) & (HT_SIZE - 1);
        uint64_t cur = atomic_load_explicit(&ht[slot_idx], memory_order_relaxed);
        if (cur == h) return 1;
        if (cur == 0) {
            uint64_t expected = 0;
            if (atomic_compare_exchange_strong_explicit(
                    &ht[slot_idx], &expected, h,
                    memory_order_relaxed, memory_order_relaxed))
                return 0;
            /* Lost the race; check what's there now */
            if (expected == h) return 1;
        }
    }
    /* All probed slots taken with different hashes — overwrite slot 0
     * lossy. Treat as fresh (won't double-count). */
    atomic_store_explicit(&ht[idx], h, memory_order_relaxed);
    return 0;
}

__attribute__((visibility("default")))
void DedupProbe_UpdateDescriptorSets(
    VkDevice device,
    uint32_t writeCount, const VkWriteDescriptorSet *pWrites,
    uint32_t copyCount,  const VkCopyDescriptorSet  *pCopies) {

    if (next_UpdateDescriptorSets)
        next_UpdateDescriptorSets(device, writeCount, pWrites, copyCount, pCopies);

    if (!probe_enabled() || !writeCount || !pWrites)
        return;

    uint64_t calls = atomic_fetch_add_explicit(&total_calls, 1, memory_order_relaxed) + 1;
    for (uint32_t i = 0; i < writeCount; i++) {
        uint64_t h = hash_write(&pWrites[i]);
        atomic_fetch_add_explicit(&total_writes, 1, memory_order_relaxed);
        if (ht_seen(h))
            atomic_fetch_add_explicit(&dup_writes, 1, memory_order_relaxed);
        else
            atomic_fetch_add_explicit(&unique_writes, 1, memory_order_relaxed);
    }

    if (calls % LOG_EVERY == 0) {
        uint64_t tw = atomic_load_explicit(&total_writes, memory_order_relaxed);
        uint64_t dw = atomic_load_explicit(&dup_writes,   memory_order_relaxed);
        uint64_t uw = atomic_load_explicit(&unique_writes, memory_order_relaxed);
        unsigned dup_pct = tw ? (unsigned)((dw * 100) / tw) : 0;
        fprintf(stderr,
                "[dedup_probe] calls=%llu writes=%llu unique=%llu dup=%llu "
                "dup_rate=%u%% (writes/call=%.1f)\n",
                (unsigned long long)calls,
                (unsigned long long)tw,
                (unsigned long long)uw,
                (unsigned long long)dw,
                dup_pct,
                (double)tw / (double)calls);
        fflush(stderr);
    }
}

__attribute__((visibility("default")))
PFN_vkVoidFunction DedupProbe_GetDeviceProcAddr(VkDevice device, const char *pName) {
    if (pName && strcmp(pName, "vkUpdateDescriptorSets") == 0)
        return (PFN_vkVoidFunction)DedupProbe_UpdateDescriptorSets;
    if (next_GetDeviceProcAddr) return next_GetDeviceProcAddr(device, pName);
    return NULL;
}

__attribute__((visibility("default")))
VkResult DedupProbe_CreateDevice(
    VkPhysicalDevice pd, const void *pCreateInfo, const void *pAllocator, VkDevice *pDevice) {
    /* Walk chain to find next gipa+gdpa. */
    typedef struct chain_hdr { int32_t sType; const void *pNext; int32_t function; void *u; } chain_hdr;
    const chain_hdr *h = (const chain_hdr*)((const void**)pCreateInfo)[1]; /* pNext */
    while (h && !(h->sType == 48 /* VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO */ && h->function == 0))
        h = (const chain_hdr*)h->pNext;
    if (!h || !h->u) return -3;

    VkLayerDeviceLink *link = (VkLayerDeviceLink*)h->u;
    PFN_vkVoidFunction (*gipa)(VkInstance, const char*) = link->pfnNextGetInstanceProcAddr;
    next_GetDeviceProcAddr = link->pfnNextGetDeviceProcAddr;

    /* Advance the chain for downstream layers. */
    ((chain_hdr*)h)->u = link->pNext;

    PFN_vkCreateDevice createNext =
        (PFN_vkCreateDevice)gipa(NULL, "vkCreateDevice");
    if (!createNext) return -3;

    VkResult r = createNext(pd, pCreateInfo, pAllocator, pDevice);
    if (r != 0) return r;

    next_UpdateDescriptorSets =
        (PFN_vkUpdateDescriptorSets)next_GetDeviceProcAddr(*pDevice, "vkUpdateDescriptorSets");

    if (probe_enabled()) {
        fprintf(stderr,
                "[dedup_probe] device created, hooks armed "
                "(next_UpdateDescriptorSets=%p)\n",
                (void*)next_UpdateDescriptorSets);
        fflush(stderr);
    }
    return r;
}

__attribute__((visibility("default")))
VkResult DedupProbe_CreateInstance(const void *pCreateInfo, const void *pAllocator, VkInstance *pInstance) {
    typedef struct chain_hdr { int32_t sType; const void *pNext; int32_t function; void *u; } chain_hdr;
    const chain_hdr *h = (const chain_hdr*)((const void**)pCreateInfo)[1]; /* pNext */
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
        fprintf(stderr, "[dedup_probe] instance created\n");
        fflush(stderr);
    }
    return r;
}

__attribute__((visibility("default")))
PFN_vkVoidFunction DedupProbe_GetInstanceProcAddr(VkInstance instance, const char *pName) {
    if (!pName) return NULL;
    if (strcmp(pName, "vkCreateInstance") == 0)
        return (PFN_vkVoidFunction)DedupProbe_CreateInstance;
    if (strcmp(pName, "vkGetInstanceProcAddr") == 0)
        return (PFN_vkVoidFunction)DedupProbe_GetInstanceProcAddr;
    if (strcmp(pName, "vkCreateDevice") == 0)
        return (PFN_vkVoidFunction)DedupProbe_CreateDevice;
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0)
        return (PFN_vkVoidFunction)DedupProbe_GetDeviceProcAddr;
    if (strcmp(pName, "vkUpdateDescriptorSets") == 0)
        return (PFN_vkVoidFunction)DedupProbe_UpdateDescriptorSets;
    if (next_GetInstanceProcAddr) return next_GetInstanceProcAddr(instance, pName);
    return NULL;
}
