/* Vulkan implicit layer that forces dualSrcBlend / logicOp and a few other
 * commonly-Mali-missing features to VK_TRUE in vkGetPhysicalDeviceFeatures
 * before returning to DXVK. Lets DXVK's D3D_FEATURE_LEVEL_11_0 check pass on
 * Mali devices where the underlying driver reports those features as FALSE.
 *
 * The layer only overrides the bools that DXVK 1.10.3 FL11_0 hard-requires
 * and that Mali Valhall traditionally exposes as FALSE:
 *   - dualSrcBlend   (offset 7*4 = 28 in VkPhysicalDeviceFeatures)
 *   - logicOp        (offset 8*4 = 32)
 *   - shaderStorageImageExtendedFormats (offset 29*4 = 116)
 * Everything else is passed through unchanged.
 *
 * Controlled by env var SPOOF_FEATURES=1. Default off, so the layer is a
 * no-op when not explicitly enabled (matches the pattern used by
 * libbcn_layer with ENABLE_BCN_COMPUTE).
 *
 * Build: aarch64-linux-android28-clang -shared -fPIC -O2 -o libfeatspoof.so
 *        feature_spoof_layer.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Minimal Vulkan typedefs — avoid needing vulkan.h at build time. */
typedef uint32_t VkResult;
typedef uint32_t VkBool32;
typedef void *VkInstance;
typedef void *VkPhysicalDevice;
typedef void *VkDevice;
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

/* VkPhysicalDeviceFeatures layout — 55 VkBool32 fields in Vulkan 1.0 order. */
typedef struct {
    VkBool32 robustBufferAccess;                           /* 0 */
    VkBool32 fullDrawIndexUint32;                          /* 1 */
    VkBool32 imageCubeArray;                               /* 2 */
    VkBool32 independentBlend;                             /* 3 */
    VkBool32 geometryShader;                               /* 4 */
    VkBool32 tessellationShader;                           /* 5 */
    VkBool32 sampleRateShading;                            /* 6 */
    VkBool32 dualSrcBlend;                                 /* 7  ← force true */
    VkBool32 logicOp;                                      /* 8  ← force true */
    VkBool32 multiDrawIndirect;                            /* 9 */
    VkBool32 drawIndirectFirstInstance;                    /* 10 */
    VkBool32 depthClamp;                                   /* 11 */
    VkBool32 depthBiasClamp;                               /* 12 */
    VkBool32 fillModeNonSolid;                             /* 13 */
    VkBool32 depthBounds;                                  /* 14 */
    VkBool32 wideLines;                                    /* 15 */
    VkBool32 largePoints;                                  /* 16 */
    VkBool32 alphaToOne;                                   /* 17 */
    VkBool32 multiViewport;                                /* 18 */
    VkBool32 samplerAnisotropy;                            /* 19 */
    VkBool32 textureCompressionETC2;                       /* 20 */
    VkBool32 textureCompressionASTC_LDR;                   /* 21 */
    VkBool32 textureCompressionBC;                         /* 22 */
    VkBool32 occlusionQueryPrecise;                        /* 23 */
    VkBool32 pipelineStatisticsQuery;                      /* 24 */
    VkBool32 vertexPipelineStoresAndAtomics;               /* 25 */
    VkBool32 fragmentStoresAndAtomics;                     /* 26 */
    VkBool32 shaderTessellationAndGeometryPointSize;       /* 27 */
    VkBool32 shaderImageGatherExtended;                    /* 28 */
    VkBool32 shaderStorageImageExtendedFormats;            /* 29 ← force true */
    VkBool32 shaderStorageImageMultisample;                /* 30 */
    VkBool32 shaderStorageImageReadWithoutFormat;
    VkBool32 shaderStorageImageWriteWithoutFormat;
    VkBool32 shaderUniformBufferArrayDynamicIndexing;
    VkBool32 shaderSampledImageArrayDynamicIndexing;
    VkBool32 shaderStorageBufferArrayDynamicIndexing;
    VkBool32 shaderStorageImageArrayDynamicIndexing;
    VkBool32 shaderClipDistance;
    VkBool32 shaderCullDistance;
    VkBool32 shaderFloat64;
    VkBool32 shaderInt64;
    VkBool32 shaderInt16;
    VkBool32 shaderResourceResidency;
    VkBool32 shaderResourceMinLod;
    VkBool32 sparseBinding;
    VkBool32 sparseResidencyBuffer;
    VkBool32 sparseResidencyImage2D;
    VkBool32 sparseResidencyImage3D;
    VkBool32 sparseResidency2Samples;
    VkBool32 sparseResidency4Samples;
    VkBool32 sparseResidency8Samples;
    VkBool32 sparseResidency16Samples;
    VkBool32 sparseResidencyAliased;
    VkBool32 variablePointersStorageBuffer;
    VkBool32 inheritedQueries;
} VkPhysicalDeviceFeatures;

/* Chain state. */
static PFN_vkVoidFunction (*next_GetInstanceProcAddr)(VkInstance, const char*);

/* Downstream function pointers captured after instance create. */
typedef void (*PFN_vkGetPhysicalDeviceFeatures)(VkPhysicalDevice, VkPhysicalDeviceFeatures*);
static PFN_vkGetPhysicalDeviceFeatures next_GetPhysicalDeviceFeatures = NULL;

/* vkGetPhysicalDeviceFeatures2 — VkPhysicalDeviceFeatures2 has a standard
 * header (sType+pNext) then a VkPhysicalDeviceFeatures embedded, then
 * optionally chains extension feature structs via pNext. DXVK 1.10+ uses
 * this path. */
typedef struct VkPhysicalDeviceFeatures2 {
    int32_t sType;       /* VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 = 1000059000 */
    void *pNext;
    VkPhysicalDeviceFeatures features;
} VkPhysicalDeviceFeatures2;
typedef void (*PFN_vkGetPhysicalDeviceFeatures2)(VkPhysicalDevice, VkPhysicalDeviceFeatures2*);
static PFN_vkGetPhysicalDeviceFeatures2 next_GetPhysicalDeviceFeatures2 = NULL;
static PFN_vkGetPhysicalDeviceFeatures2 next_GetPhysicalDeviceFeatures2KHR = NULL;

typedef VkResult (*PFN_vkCreateInstance)(const VkInstanceCreateInfo*, const void*, VkInstance*);

/* vkCreateDevice intercept — log the VkDeviceCreateInfo DXVK passes (extensions,
 * feature chain) and the VkResult from the underlying driver, so we can tell
 * whether the failure is an unsupported extension, a feature mismatch, etc. */
typedef struct VkDeviceQueueCreateInfo {
    int32_t sType; const void *pNext;
    uint32_t flags;
    uint32_t queueFamilyIndex;
    uint32_t queueCount;
    const float *pQueuePriorities;
} VkDeviceQueueCreateInfo;

typedef struct VkDeviceCreateInfo {
    int32_t sType; const void *pNext;
    uint32_t flags;
    uint32_t queueCreateInfoCount;
    const VkDeviceQueueCreateInfo *pQueueCreateInfos;
    uint32_t enabledLayerCount;
    const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char *const *ppEnabledExtensionNames;
    const VkPhysicalDeviceFeatures *pEnabledFeatures;
} VkDeviceCreateInfo;

typedef VkResult (*PFN_vkCreateDevice)(VkPhysicalDevice, const VkDeviceCreateInfo*, const void*, VkDevice*);
static PFN_vkCreateDevice next_CreateDevice = NULL;
/* Captured during FeatSpoof_CreateDevice so FeatSpoof_GetDeviceProcAddr can
 * forward. Single-device simplification: overwritten if multiple devices are
 * created, but DXVK only creates one here. Without this, BCnLayer (which
 * sits above us) queries device funcs through us, gets NULL, then calls
 * through NULL → DEP fault in BCnLayer_CreateDevice. */
static PFN_vkVoidFunction (*next_GetDeviceProcAddr)(VkDevice, const char*) = NULL;

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

static int spoof_enabled(void) {
    const char *e = getenv("SPOOF_FEATURES");
    return e && e[0] == '1';
}

/* Snapshot of the REAL driver features (before our spoof). Captured on the
 * first v1/v2 query so we can later mask DXVK's enabled features back down
 * to what the driver actually supports. */
static VkPhysicalDeviceFeatures real_features;
static int real_features_captured = 0;

/* Cache of driver-real extension feature struct values. Populated on the first
 * vkGetPhysicalDeviceFeatures2 call with a pNext chain. Indexed linearly; 32
 * slots is more than any real DXVK chain. */
#define EXT_CACHE_MAX 32
#define EXT_CACHE_BOOLS 16
static struct {
    int32_t sType;
    int bool_count;
    VkBool32 bools[EXT_CACHE_BOOLS];
} ext_cache[EXT_CACHE_MAX];
static int ext_cache_len = 0;

/* Hardcoded bool counts for known feature struct sTypes. Must match the Vulkan
 * spec exactly — writing past the actual struct end corrupts the pNext chain
 * and crashes wine's UNIX_CALL unmarshal. 0 = unknown, don't touch. */
static int sType_bool_count(int32_t sType) {
    switch (sType) {
    case 1000028000: return 2; /* PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT */
    case 1000063000: return 1; /* PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES */
    case 1000207000: return 2; /* PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT */
    case 1000257000: return 3; /* PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES */
    case 1000261000: return 1; /* PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES */
    case 1000267000: return 1; /* PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES */
    case 1000276000: return 1; /* PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES_EXT */
    case 1000287002: return 2; /* PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR */
    case 1000340000: return 2; /* PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT */
    default: return 0;
    }
}

static void capture_ext_chain(const void *pNext) {
    if (ext_cache_len > 0) return; /* one-shot */
    struct chain_hdr { int32_t sType; const void *pNext; };
    const struct chain_hdr *h = (const struct chain_hdr*)pNext;
    while (h && ext_cache_len < EXT_CACHE_MAX) {
        if ((int32_t)h->sType == 48) { h = (const struct chain_hdr*)h->pNext; continue; }
        int nbools = sType_bool_count((int32_t)h->sType);
        if (nbools <= 0) {
            /* Unknown struct — skip caching to avoid reading past its end. */
            fprintf(stderr, "  ext_cache: SKIP sType=%d (unknown, no strip)\n", (int32_t)h->sType);
            h = (const struct chain_hdr*)h->pNext;
            continue;
        }
        const VkBool32 *body = (const VkBool32*)(((const char*)h) + sizeof(*h));
        ext_cache[ext_cache_len].sType = (int32_t)h->sType;
        ext_cache[ext_cache_len].bool_count = nbools;
        for (int i = 0; i < nbools && i < EXT_CACHE_BOOLS; i++)
            ext_cache[ext_cache_len].bools[i] = body[i];
        fprintf(stderr, "  ext_cache[%d] sType=%d nbools=%d bools=[",
                ext_cache_len, (int32_t)h->sType, nbools);
        for (int i = 0; i < nbools; i++) fprintf(stderr, "%u ", body[i]);
        fprintf(stderr, "]\n");
        ext_cache_len++;
        h = (const struct chain_hdr*)h->pNext;
    }
    fflush(stderr);
}

static const int *ext_cache_lookup(int32_t sType, int *out_count) {
    for (int i = 0; i < ext_cache_len; i++) {
        if (ext_cache[i].sType == sType) {
            *out_count = ext_cache[i].bool_count;
            return (const int*)ext_cache[i].bools;
        }
    }
    return NULL;
}

static void capture_real(const VkPhysicalDeviceFeatures *f) {
    if (!real_features_captured && f) {
        real_features = *f;
        real_features_captured = 1;
    }
}

/* AND `out` with `real_features`: any feature requested=1 but driver=0 is
 * cleared. Call only if real_features_captured. */
static void mask_to_real(VkPhysicalDeviceFeatures *out) {
    VkBool32 *o = (VkBool32*)out;
    VkBool32 *r = (VkBool32*)&real_features;
    size_t n = sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32);
    for (size_t i = 0; i < n; i++) o[i] = o[i] && r[i];
}

static void dump_features_once(const VkPhysicalDeviceFeatures *f, const char *tag) {
    static int dump_v1 = 0, dump_v2 = 0;
    int is_v2 = (tag && tag[0] == '2');
    if ((is_v2 ? dump_v2 : dump_v1) || !f) return;
    if (is_v2) dump_v2 = 1; else dump_v1 = 1;
    fprintf(stderr, "feature_spoof: call #%s VkPhysicalDeviceFeatures bitmap from driver:\n", tag);
    #define P(x) fprintf(stderr, "  %-48s %u\n", #x, (unsigned)f->x)
    P(robustBufferAccess); P(fullDrawIndexUint32); P(imageCubeArray);
    P(independentBlend); P(geometryShader); P(tessellationShader);
    P(sampleRateShading); P(dualSrcBlend); P(logicOp);
    P(multiDrawIndirect); P(drawIndirectFirstInstance);
    P(depthClamp); P(depthBiasClamp); P(fillModeNonSolid);
    P(depthBounds); P(wideLines); P(largePoints); P(alphaToOne);
    P(multiViewport); P(samplerAnisotropy);
    P(textureCompressionETC2); P(textureCompressionASTC_LDR); P(textureCompressionBC);
    P(occlusionQueryPrecise); P(pipelineStatisticsQuery);
    P(vertexPipelineStoresAndAtomics); P(fragmentStoresAndAtomics);
    P(shaderTessellationAndGeometryPointSize);
    P(shaderImageGatherExtended); P(shaderStorageImageExtendedFormats);
    P(shaderStorageImageMultisample);
    P(shaderStorageImageReadWithoutFormat); P(shaderStorageImageWriteWithoutFormat);
    P(shaderUniformBufferArrayDynamicIndexing); P(shaderSampledImageArrayDynamicIndexing);
    P(shaderStorageBufferArrayDynamicIndexing); P(shaderStorageImageArrayDynamicIndexing);
    P(shaderClipDistance); P(shaderCullDistance);
    P(shaderFloat64); P(shaderInt64); P(shaderInt16);
    P(shaderResourceResidency); P(shaderResourceMinLod);
    P(sparseBinding);
    P(sparseResidencyBuffer); P(sparseResidencyImage2D); P(sparseResidencyImage3D);
    P(sparseResidency2Samples); P(sparseResidency4Samples);
    P(sparseResidency8Samples); P(sparseResidency16Samples); P(sparseResidencyAliased);
    P(variablePointersStorageBuffer); P(inheritedQueries);
    #undef P
    fflush(stderr);
}

static void apply_spoof(VkPhysicalDeviceFeatures *f) {
    if (!spoof_enabled() || !f) return;
    /* Force every feature DXVK FL11_0 cares about on. */
    f->dualSrcBlend = 1;
    f->logicOp = 1;
    f->imageCubeArray = 1;
    f->independentBlend = 1;
    f->geometryShader = 1;
    f->tessellationShader = 1;
    f->sampleRateShading = 1;
    f->multiDrawIndirect = 1;
    f->drawIndirectFirstInstance = 1;
    f->depthClamp = 1;
    f->depthBiasClamp = 1;
    f->fillModeNonSolid = 1;
    f->multiViewport = 1;
    f->samplerAnisotropy = 1;
    f->textureCompressionBC = 1;
    f->occlusionQueryPrecise = 1;
    f->pipelineStatisticsQuery = 1;
    f->vertexPipelineStoresAndAtomics = 1;
    f->fragmentStoresAndAtomics = 1;
    f->shaderTessellationAndGeometryPointSize = 1;
    f->shaderImageGatherExtended = 1;
    f->shaderStorageImageExtendedFormats = 1;
    f->shaderStorageImageMultisample = 1;
    f->shaderStorageImageReadWithoutFormat = 1;
    f->shaderStorageImageWriteWithoutFormat = 1;
    f->shaderUniformBufferArrayDynamicIndexing = 1;
    f->shaderSampledImageArrayDynamicIndexing = 1;
    f->shaderStorageBufferArrayDynamicIndexing = 1;
    f->shaderStorageImageArrayDynamicIndexing = 1;
    f->shaderClipDistance = 1;
    f->shaderCullDistance = 1;
}

__attribute__((visibility("default")))
void FeatSpoof_GetPhysicalDeviceFeatures(VkPhysicalDevice pd, VkPhysicalDeviceFeatures *f) {
    if (next_GetPhysicalDeviceFeatures) next_GetPhysicalDeviceFeatures(pd, f);
    capture_real(f);
    dump_features_once(f, "1");
    apply_spoof(f);
}

__attribute__((visibility("default")))
void FeatSpoof_GetPhysicalDeviceFeatures2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f2) {
    if (next_GetPhysicalDeviceFeatures2) next_GetPhysicalDeviceFeatures2(pd, f2);
    if (f2) {
        capture_real(&f2->features);
        if (f2->pNext && ext_cache_len == 0) {
            fprintf(stderr, "feature_spoof: caching ext feature chain from driver\n");
            capture_ext_chain(f2->pNext);
        }
        dump_features_once(&f2->features, "2");
        apply_spoof(&f2->features);
    }
}

__attribute__((visibility("default")))
void FeatSpoof_GetPhysicalDeviceFeatures2KHR(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f2) {
    if (next_GetPhysicalDeviceFeatures2KHR) next_GetPhysicalDeviceFeatures2KHR(pd, f2);
    else if (next_GetPhysicalDeviceFeatures2) next_GetPhysicalDeviceFeatures2(pd, f2);
    if (f2) {
        capture_real(&f2->features);
        if (f2->pNext && ext_cache_len == 0) {
            fprintf(stderr, "feature_spoof: caching ext feature chain from driver (KHR)\n");
            capture_ext_chain(f2->pNext);
        }
        dump_features_once(&f2->features, "2KHR");
        apply_spoof(&f2->features);
    }
}

__attribute__((visibility("default")))
VkResult FeatSpoof_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *pCreateInfo, const void *pAllocator, VkDevice *pDevice) {
    /* Walk chain to find the next-layer gipa + gdpa. */
    const VkLayerDeviceCreateInfo *ci = (const VkLayerDeviceCreateInfo*)pCreateInfo->pNext;
    while (ci && !(ci->sType == 48 && ci->function == 0))
        ci = (const VkLayerDeviceCreateInfo*)ci->pNext;
    if (!ci || !ci->u.pLayerInfo) {
        fprintf(stderr, "feature_spoof: CreateDevice couldn't find layer link\n");
        fflush(stderr);
        return (VkResult)-3;
    }
    PFN_vkVoidFunction (*gipa)(VkInstance, const char*) = ci->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    next_GetDeviceProcAddr = ci->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    ((VkLayerDeviceCreateInfo*)ci)->u.pLayerInfo = ci->u.pLayerInfo->pNext;

    PFN_vkCreateDevice createNext = (PFN_vkCreateDevice)gipa(NULL, "vkCreateDevice");
    if (!createNext) {
        fprintf(stderr, "feature_spoof: CreateDevice: next createDevice null\n");
        fflush(stderr);
        return (VkResult)-3;
    }

    /* Build a modified VkDeviceCreateInfo where pEnabledFeatures is masked
     * to (requested AND real). This prevents VK_ERROR_FEATURE_NOT_PRESENT
     * (-8) when DXVK enables features our layer spoofed but the driver
     * doesn't actually support (e.g. dualSrcBlend, logicOp on Mali). */
    VkDeviceCreateInfo local_ci = *pCreateInfo;
    VkPhysicalDeviceFeatures masked_features;
    int masked = 0;
    if (pCreateInfo->pEnabledFeatures && real_features_captured) {
        masked_features = *pCreateInfo->pEnabledFeatures;
        VkPhysicalDeviceFeatures before = masked_features;
        mask_to_real(&masked_features);
        local_ci.pEnabledFeatures = &masked_features;
        masked = 1;
        /* Report which features we stripped. */
        VkBool32 *b = (VkBool32*)&before;
        VkBool32 *a = (VkBool32*)&masked_features;
        const char *names[] = {
            "robustBufferAccess","fullDrawIndexUint32","imageCubeArray",
            "independentBlend","geometryShader","tessellationShader",
            "sampleRateShading","dualSrcBlend","logicOp",
            "multiDrawIndirect","drawIndirectFirstInstance",
            "depthClamp","depthBiasClamp","fillModeNonSolid",
            "depthBounds","wideLines","largePoints","alphaToOne",
            "multiViewport","samplerAnisotropy",
            "textureCompressionETC2","textureCompressionASTC_LDR","textureCompressionBC",
            "occlusionQueryPrecise","pipelineStatisticsQuery",
            "vertexPipelineStoresAndAtomics","fragmentStoresAndAtomics",
            "shaderTessellationAndGeometryPointSize",
            "shaderImageGatherExtended","shaderStorageImageExtendedFormats",
            "shaderStorageImageMultisample",
            "shaderStorageImageReadWithoutFormat","shaderStorageImageWriteWithoutFormat",
            "shaderUniformBufferArrayDynamicIndexing","shaderSampledImageArrayDynamicIndexing",
            "shaderStorageBufferArrayDynamicIndexing","shaderStorageImageArrayDynamicIndexing",
            "shaderClipDistance","shaderCullDistance",
            "shaderFloat64","shaderInt64","shaderInt16",
            "shaderResourceResidency","shaderResourceMinLod",
            "sparseBinding","sparseResidencyBuffer","sparseResidencyImage2D",
            "sparseResidencyImage3D","sparseResidency2Samples","sparseResidency4Samples",
            "sparseResidency8Samples","sparseResidency16Samples","sparseResidencyAliased",
            "variablePointersStorageBuffer","inheritedQueries"
        };
        for (size_t i = 0; i < sizeof(names)/sizeof(names[0]); i++) {
            if (b[i] && !a[i])
                fprintf(stderr, "  STRIP: %s (requested but driver=0)\n", names[i]);
        }
    }

    fprintf(stderr, "feature_spoof: vkCreateDevice %u ext, pEnabledFeatures=%s\n",
            pCreateInfo->enabledExtensionCount,
            pCreateInfo->pEnabledFeatures ? (masked ? "masked-to-real" : "passthrough-no-real-cache")
                                          : "null-check-pNext-chain");

    /* Walk pCreateInfo->pNext and, for each struct whose sType we cached at
     * Features2 query time, overwrite the feature bools with the driver-real
     * cached values. This is in-place but minimal (no Vulkan re-entry). */
    if (pCreateInfo->pNext && ext_cache_len > 0) {
        struct chain_hdr { int32_t sType; void *pNext; };
        struct chain_hdr *h = (struct chain_hdr*)pCreateInfo->pNext;
        int n = 0;
        while (h && n < 20) {
            int cached_count = 0;
            const int *cached_bools = ext_cache_lookup((int32_t)h->sType, &cached_count);
            if (cached_bools) {
                VkBool32 *body = (VkBool32*)(((char*)h) + sizeof(*h));
                int changed = 0;
                for (int i = 0; i < cached_count && i < EXT_CACHE_BOOLS; i++) {
                    if (body[i] != (VkBool32)cached_bools[i]) {
                        if (!changed) fprintf(stderr, "  MASK-EXT: sType=%d", (int)h->sType);
                        fprintf(stderr, " [%d:%u->%u]", i, body[i], (VkBool32)cached_bools[i]);
                        body[i] = (VkBool32)cached_bools[i];
                        changed = 1;
                    }
                }
                if (changed) { fprintf(stderr, "\n"); }
            }
            h = (struct chain_hdr*)h->pNext;
            n++;
        }
    }
    fflush(stderr);

    VkResult r = createNext(pd, &local_ci, pAllocator, pDevice);
    fprintf(stderr, "feature_spoof: vkCreateDevice returned VkResult=%d\n", (int)r);
    fflush(stderr);
    return r;
}

__attribute__((visibility("default")))
VkResult FeatSpoof_CreateInstance(const VkInstanceCreateInfo *pCreateInfo, const void *pAllocator, VkInstance *pInstance) {
    /* Walk the layer chain to find the next GetInstanceProcAddr. */
    const VkLayerInstanceCreateInfo *ci = (const VkLayerInstanceCreateInfo*)pCreateInfo->pNext;
    while (ci && !(ci->sType == 47 && ci->function == 0))
        ci = (const VkLayerInstanceCreateInfo*)ci->pNext;
    if (!ci || !ci->u.pLayerInfo) return (VkResult)-3; /* VK_ERROR_INITIALIZATION_FAILED */

    PFN_vkVoidFunction (*gipa)(VkInstance, const char*) = ci->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    next_GetInstanceProcAddr = gipa;

    /* Advance the chain for downstream layers. */
    ((VkLayerInstanceCreateInfo*)ci)->u.pLayerInfo = ci->u.pLayerInfo->pNext;

    PFN_vkCreateInstance createNext = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    if (!createNext) return (VkResult)-3;

    VkResult r = createNext(pCreateInfo, pAllocator, pInstance);
    if (r != 0) return r;

    /* Capture GetPhysicalDeviceFeatures + v2 variants from the newly-created
     * instance chain. DXVK 1.10+ uses v2 (core in 1.1) or v2KHR (extension). */
    next_GetPhysicalDeviceFeatures =
        (PFN_vkGetPhysicalDeviceFeatures)gipa(*pInstance, "vkGetPhysicalDeviceFeatures");
    next_GetPhysicalDeviceFeatures2 =
        (PFN_vkGetPhysicalDeviceFeatures2)gipa(*pInstance, "vkGetPhysicalDeviceFeatures2");
    next_GetPhysicalDeviceFeatures2KHR =
        (PFN_vkGetPhysicalDeviceFeatures2)gipa(*pInstance, "vkGetPhysicalDeviceFeatures2KHR");

    if (spoof_enabled()) {
        fprintf(stderr,
                "feature_spoof: instance created, spoofing active "
                "(v1=%p v2=%p v2KHR=%p)\n",
                (void*)next_GetPhysicalDeviceFeatures,
                (void*)next_GetPhysicalDeviceFeatures2,
                (void*)next_GetPhysicalDeviceFeatures2KHR);
        fflush(stderr);
    }
    return r;
}

__attribute__((visibility("default")))
PFN_vkVoidFunction FeatSpoof_GetInstanceProcAddr(VkInstance instance, const char *pName) {
    if (!pName) return NULL;
    if (strcmp(pName, "vkCreateInstance") == 0)
        return (PFN_vkVoidFunction)FeatSpoof_CreateInstance;
    if (strcmp(pName, "vkGetInstanceProcAddr") == 0)
        return (PFN_vkVoidFunction)FeatSpoof_GetInstanceProcAddr;
    if (strcmp(pName, "vkGetPhysicalDeviceFeatures") == 0)
        return (PFN_vkVoidFunction)FeatSpoof_GetPhysicalDeviceFeatures;
    if (strcmp(pName, "vkGetPhysicalDeviceFeatures2") == 0)
        return (PFN_vkVoidFunction)FeatSpoof_GetPhysicalDeviceFeatures2;
    if (strcmp(pName, "vkGetPhysicalDeviceFeatures2KHR") == 0)
        return (PFN_vkVoidFunction)FeatSpoof_GetPhysicalDeviceFeatures2KHR;
    if (strcmp(pName, "vkCreateDevice") == 0)
        return (PFN_vkVoidFunction)FeatSpoof_CreateDevice;
    if (next_GetInstanceProcAddr) return next_GetInstanceProcAddr(instance, pName);
    return NULL;
}

__attribute__((visibility("default")))
PFN_vkVoidFunction FeatSpoof_GetDeviceProcAddr(VkDevice device, const char *pName) {
    /* We don't intercept any device-level functions — forward to the next
     * layer's GetDeviceProcAddr. Returning NULL here was wrong: the loader
     * doesn't "skip" a non-intercepting layer, it propagates the NULL up.
     * Layers above us (BCnLayer in our stack) call through the returned
     * pointer, and NULL → DEP fault in BCnLayer_CreateDevice. */
    if (next_GetDeviceProcAddr) return next_GetDeviceProcAddr(device, pName);
    return NULL;
}
