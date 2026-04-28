/*
 * shim_maintenance5 — ICD-wrapping shim that exposes VK_KHR_maintenance5
 * on top of the leegao bionic-vulkan-wrapper. Forwards every Khronos ICD
 * entrypoint to the real wrapper via dlopen, and intercepts a small set
 * of entries to:
 *
 *   1. Inject "VK_KHR_maintenance5" into vkEnumerateDeviceExtensionProperties
 *   2. Implement the four maintenance5 device entrypoints
 *      (vkCmdBindIndexBuffer2KHR, vkGetRenderingAreaGranularityKHR,
 *       vkGetImageSubresourceLayout2KHR, vkGetDeviceImageSubresourceLayoutKHR)
 *   3. Fill VkPhysicalDeviceMaintenance5FeaturesKHR / PropertiesKHR
 *      structs in the pNext chains of vkGetPhysicalDeviceFeatures2 /
 *      Properties2
 *
 * Implementation strategy: translate maintenance5 entries to their v1
 * equivalents on the underlying wrapper. vkCmdBindIndexBuffer2KHR drops
 * its size parameter (v1 implicitly uses rest-of-buffer); v2 subresource
 * layout queries unwrap to the v1 entry. vkGetDeviceImageSubresourceLayoutKHR
 * is a zero-stub — DXVK doesn't use it on any hot path; real emulation
 * would create+destroy a transient image, which we punt.
 *
 * This shim alone does NOT unlock DXVK 2.7+: 2.7.1 also needs
 * robustBufferAccess2; 2.0–2.6.2 crash in vkCreateShaderModule. Scope
 * here is testbench-level proof of architecture.
 *
 * Use: set the testbench's WRAPPER_TESTBENCH_LIB env var to point at
 * this .so instead of the real wrapper. The shim dlopens the real one
 * by absolute path on first call.
 *
 * Real wrapper path is hardcoded — change WRAPPED_LIB if redeployed
 * elsewhere.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spv_instrumenter.h"

#define WRAPPED_LIB "/data/data/com.mediatek.steamlauncher/files/imagefs_bionic/usr/lib/libvulkan_wrapper.so"

/* Extensions this shim claims on top of the real wrapper.
 *
 * VK_KHR_maintenance5 — entrypoints implemented (with the v1-fallback
 *   compromises documented in the per-shim functions).
 * VK_EXT_robustness2 — partial real implementation:
 *   * nullDescriptor — REAL: per-device standin resources substituted
 *     for VK_NULL_HANDLE in vkUpdateDescriptorSets and the descriptor
 *     update template variants. Real read-zero semantics for read-only
 *     descriptor types (UNIFORM_BUFFER, SAMPLED_IMAGE, etc.).
 *   * robustBufferAccess2 — STILL A LIE. Real impl needs SPIR-V
 *     instrumentation at vkCreateShaderModule time (bucket #3 in
 *     project_strategic_priority_mali_wrapper.md). Feature bit set so
 *     DXVK passes its adapter gate; OOB read behavior is whatever the
 *     wrapper / Mali defaults to.
 *   * robustImageAccess2 — same as robustBufferAccess2, lie.
 *   * Storage-writes to null descriptors — INCOMPLETE: writes go to a
 *     shared standin resource that other "null" reads will see, so
 *     spec-correct write-discard isn't delivered. Same SPIR-V work
 *     would fix this.
 *   Do NOT extend the spoof list to Vulkan13Features (sync2 / dynamicRendering)
 *   without revisiting feature_spoof_layer.c:348-360 — prior attempt
 *   caused _wassert in vkCreateShaderModule (DXVK generated SPIR-V the
 *   wrapper can't consume). */
static const struct { const char *name; uint32_t spec_version; } INJECTED_EXTS[] = {
    { "VK_KHR_maintenance5", 1 },
    { "VK_EXT_robustness2",  1 },
};
#define INJECTED_EXTS_COUNT ((uint32_t)(sizeof(INJECTED_EXTS) / sizeof(INJECTED_EXTS[0])))

static void                                            *g_real_lib = NULL;
static PFN_vkGetInstanceProcAddr                        g_real_gipa = NULL;
/* Real fn pointers captured at resolution time. Non-global functions
 * resolved via the ICD GIPA need a valid instance/device, so we capture
 * them lazily inside the corresponding hook. */
static PFN_vkEnumerateDeviceExtensionProperties         g_real_enum = NULL;
static PFN_vkGetDeviceProcAddr                          g_real_gdpa = NULL;
static PFN_vkGetPhysicalDeviceFeatures2                 g_real_gpdf2 = NULL;
static PFN_vkGetPhysicalDeviceProperties2               g_real_gpdp2 = NULL;
static PFN_vkCmdBindIndexBuffer                         g_real_cmd_bind_index = NULL;
static PFN_vkGetImageSubresourceLayout                  g_real_get_isl = NULL;
static PFN_vkCreateBuffer                               g_real_create_buffer = NULL;
static PFN_vkCreateGraphicsPipelines                    g_real_create_graphics_pipelines = NULL;
static PFN_vkCreateComputePipelines                     g_real_create_compute_pipelines = NULL;
static PFN_vkCreateShaderModule                         g_real_create_shader_module = NULL;
/* Forward declarations used by hooks defined earlier in the file. The
 * full state for nullDescriptor lives further down (search "robustness2
 * nullDescriptor real-implementation"). */
static VkPhysicalDevice                                 g_pdev_for_standins = VK_NULL_HANDLE;

/* Exposed counters so the testbench can prove a fold actually fired,
 * not just that vkCreateBuffer happened to succeed (the wrapper is lax
 * about usage=0, so success-on-create is not a fold-correctness signal
 * by itself). dlsym'd from the testbench. */
__attribute__((visibility("default"))) volatile int shim_m5_buffer_flags2_fold_count = 0;
__attribute__((visibility("default"))) volatile int shim_m5_pipeline_flags2_fold_count = 0;
/* Counts vkUpdateDescriptorSets writes where VK_NULL_HANDLE was
 * substituted with a standin (per descriptor element, not per write). */
__attribute__((visibility("default"))) volatile int shim_m5_null_subst_count = 0;
/* Subset of subst_count: substitutions on writable descriptor types
 * (STORAGE_BUFFER / STORAGE_IMAGE / STORAGE_TEXEL_BUFFER) where the
 * shared-standin lie about write-discard applies. Tracked separately so
 * tests can call out the INCOMPLETE write-semantics gap. */
__attribute__((visibility("default"))) volatile int shim_m5_null_storage_subst_count = 0;

static void open_real_lib_once(void) {
    if (g_real_lib) return;
    g_real_lib = dlopen(WRAPPED_LIB, RTLD_NOW | RTLD_LOCAL);
    if (!g_real_lib) {
        fprintf(stderr, "[shim_maintenance5] dlopen real wrapper failed: %s\n", dlerror());
        return;
    }
    g_real_gipa = (PFN_vkGetInstanceProcAddr)
        dlsym(g_real_lib, "vk_icdGetInstanceProcAddr");
    if (!g_real_gipa) {
        fprintf(stderr, "[shim_maintenance5] dlsym real vk_icdGetInstanceProcAddr failed: %s\n", dlerror());
    }
}

/* --- maintenance5 device-entry shims --- */

/* size param is dropped: the v1 entry implicitly uses rest-of-buffer,
 * which matches maintenance5's VK_WHOLE_SIZE behavior. Non-WHOLE_SIZE
 * sizes are not enforced (the v1 entry has no equivalent), so callers
 * relying on tight size-bounded index reads will see looser behavior. */
static void VKAPI_PTR shim_CmdBindIndexBuffer2KHR(
    VkCommandBuffer  commandBuffer,
    VkBuffer         buffer,
    VkDeviceSize     offset,
    VkDeviceSize     size,
    VkIndexType      indexType)
{
    (void)size;
    if (g_real_cmd_bind_index)
        g_real_cmd_bind_index(commandBuffer, buffer, offset, indexType);
}

/* Always-safe minimum granularity. Real Mali tilers report larger
 * granularity for tile-aligned rendering, but {1,1} is universally
 * spec-conformant — engines just won't get the tile-aligned hint. */
static void VKAPI_PTR shim_GetRenderingAreaGranularityKHR(
    VkDevice                          device,
    const VkRenderingAreaInfoKHR     *pRenderingAreaInfo,
    VkExtent2D                       *pGranularity)
{
    (void)device; (void)pRenderingAreaInfo;
    if (pGranularity) { pGranularity->width = 1; pGranularity->height = 1; }
}

static void VKAPI_PTR shim_GetImageSubresourceLayout2KHR(
    VkDevice                              device,
    VkImage                               image,
    const VkImageSubresource2KHR         *pSubresource,
    VkSubresourceLayout2KHR              *pLayout)
{
    if (!g_real_get_isl || !pSubresource || !pLayout) return;
    g_real_get_isl(device, image, &pSubresource->imageSubresource,
                   &pLayout->subresourceLayout);
}

/* Zero-stub: real emulation would vkCreateImage(pInfo->pCreateInfo),
 * query its layout, vkDestroyImage. DXVK doesn't call this on any hot
 * path, so leave it returning zeros until a caller is observed in
 * practice. */
static void VKAPI_PTR shim_GetDeviceImageSubresourceLayoutKHR(
    VkDevice                              device,
    const VkDeviceImageSubresourceInfoKHR *pInfo,
    VkSubresourceLayout2KHR              *pLayout)
{
    (void)device; (void)pInfo;
    if (pLayout) memset(&pLayout->subresourceLayout, 0,
                        sizeof(pLayout->subresourceLayout));
}

/* --- features/properties pNext-chain fillers --- */

static void VKAPI_PTR shim_GetPhysicalDeviceFeatures2(
    VkPhysicalDevice            physicalDevice,
    VkPhysicalDeviceFeatures2  *pFeatures)
{
    /* Capture the physical device for later standin construction. We
     * don't intercept vkCreateDevice; this is the earliest pdev-bearing
     * call we already hook, and DXVK / the testbench call it before
     * device creation. */
    g_pdev_for_standins = physicalDevice;
    if (g_real_gpdf2) g_real_gpdf2(physicalDevice, pFeatures);
    if (!pFeatures) return;
    VkBaseOutStructure *p = (VkBaseOutStructure *)pFeatures->pNext;
    while (p) {
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR) {
            VkPhysicalDeviceMaintenance5FeaturesKHR *m =
                (VkPhysicalDeviceMaintenance5FeaturesKHR *)p;
            m->maintenance5 = VK_TRUE;
        } else if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT) {
            /* Same lie as feature_spoof_layer.c:337-340. The wrapper can't
             * actually deliver robustness2 OOB-zero semantics, but DXVK
             * 2.7.1 gates adapter acceptance on these bits. Mali tolerates
             * the lie for content typical of our games. */
            VkPhysicalDeviceRobustness2FeaturesEXT *r =
                (VkPhysicalDeviceRobustness2FeaturesEXT *)p;
            r->robustBufferAccess2 = VK_TRUE;
            r->robustImageAccess2  = VK_TRUE;
            r->nullDescriptor      = VK_TRUE;
        }
        p = p->pNext;
    }
}

static void VKAPI_PTR shim_GetPhysicalDeviceProperties2(
    VkPhysicalDevice              physicalDevice,
    VkPhysicalDeviceProperties2  *pProperties)
{
    if (g_real_gpdp2) g_real_gpdp2(physicalDevice, pProperties);
    if (!pProperties) return;
    VkBaseOutStructure *p = (VkBaseOutStructure *)pProperties->pNext;
    while (p) {
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_PROPERTIES_KHR) {
            VkPhysicalDeviceMaintenance5PropertiesKHR *m =
                (VkPhysicalDeviceMaintenance5PropertiesKHR *)p;
            m->earlyFragmentMultisampleCoverageAfterSampleCounting = VK_FALSE;
            m->earlyFragmentSampleMaskTestBeforeSampleCounting     = VK_FALSE;
            m->depthStencilSwizzleOneSupport                       = VK_FALSE;
            m->polygonModePointSize                                = VK_FALSE;
            m->nonStrictSinglePixelWideLinesUseParallelogram       = VK_FALSE;
            m->nonStrictWideLinesUseParallelogram                  = VK_FALSE;
        } else if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_PROPERTIES_EXT) {
            /* Typical PC-driver values; DXVK uses these as alignment
             * guarantees for OOB-zeroing math. Setting 4 means "any
             * 4-byte-aligned access is robustness-protected". */
            VkPhysicalDeviceRobustness2PropertiesEXT *r =
                (VkPhysicalDeviceRobustness2PropertiesEXT *)p;
            r->robustStorageBufferAccessSizeAlignment = 4;
            r->robustUniformBufferAccessSizeAlignment = 4;
        }
        p = p->pNext;
    }
}

/* --- maintenance5 pNext folding for flags2 structs ---
 *
 * DXVK 2.7+ extends VkBufferCreateInfo / VkGraphicsPipelineCreateInfo /
 * VkComputePipelineCreateInfo with a flags2 pNext struct that carries
 * a 64-bit usage/flags bitmask. On a wrapper without maintenance5 the
 * struct's sType is unknown, so the wrapper silently ignores it and
 * the create call only sees v1.usage / v1.flags — losing whatever
 * bits were supplied via flags2.
 *
 * Strategy: copy the create-info struct locally, OR the flags2 low-32
 * bits into the local copy's v1 field, forward the modified copy with
 * the *original* pNext chain unchanged. The flags2 struct stays in
 * the chain — the wrapper still ignores it, but the bits already
 * landed in the v1 field, which the wrapper does honor.
 *
 * Limitation: flags2 bits beyond bit 31 (rare; mostly future extension
 * space) are dropped by the (uint32_t) cast. None of the currently
 * defined bits live beyond bit 31. */

static VkBufferUsageFlags fold_buffer_flags2(const void *pNext, VkBufferUsageFlags fallback) {
    const VkBaseInStructure *p = (const VkBaseInStructure *)pNext;
    while (p) {
        if (p->sType == VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR) {
            const VkBufferUsageFlags2CreateInfoKHR *f2 =
                (const VkBufferUsageFlags2CreateInfoKHR *)p;
            return (VkBufferUsageFlags)(f2->usage & 0xFFFFFFFFu);
        }
        p = p->pNext;
    }
    return fallback;
}

static VkPipelineCreateFlags fold_pipeline_flags2(const void *pNext, VkPipelineCreateFlags fallback) {
    const VkBaseInStructure *p = (const VkBaseInStructure *)pNext;
    while (p) {
        if (p->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR) {
            const VkPipelineCreateFlags2CreateInfoKHR *f2 =
                (const VkPipelineCreateFlags2CreateInfoKHR *)p;
            return (VkPipelineCreateFlags)(f2->flags & 0xFFFFFFFFu);
        }
        p = p->pNext;
    }
    return fallback;
}

/* SPIR-V instrumentation hook. Routes the input SPIR-V through
 * spv_instrumenter (C++ side, links SPIRV-Tools) and forwards the
 * possibly-modified module to the real wrapper. Currently a passthrough
 * (parse + reserialize, no transform) — verifies the link works
 * end-to-end before the real robustBufferAccess2 / robustImageAccess2
 * pass is wired in. */
static VkResult VKAPI_PTR shim_CreateShaderModule(
    VkDevice                            device,
    const VkShaderModuleCreateInfo     *pCreateInfo,
    const VkAllocationCallbacks        *pAllocator,
    VkShaderModule                     *pShaderModule)
{
    if (!g_real_create_shader_module) return VK_ERROR_INITIALIZATION_FAILED;
    if (!pCreateInfo || !pCreateInfo->pCode || pCreateInfo->codeSize == 0) {
        return g_real_create_shader_module(device, pCreateInfo, pAllocator, pShaderModule);
    }

    uint32_t *new_code = NULL;
    size_t    new_size = 0;
    int ok = shim_spv_instrument(pCreateInfo->pCode, pCreateInfo->codeSize,
                                 &new_code, &new_size);
    if (!ok || !new_code) {
        /* Instrumenter declined (parse failure, OOM, etc.) — fall through
         * to forwarding the original. The wrapper sees what the caller
         * sent, no behavior change. */
        return g_real_create_shader_module(device, pCreateInfo, pAllocator, pShaderModule);
    }

    VkShaderModuleCreateInfo modified = *pCreateInfo;
    modified.pCode    = new_code;
    modified.codeSize = new_size;
    VkResult r = g_real_create_shader_module(device, &modified, pAllocator, pShaderModule);
    shim_spv_free(new_code);
    return r;
}

static VkResult VKAPI_PTR shim_CreateBuffer(
    VkDevice                      device,
    const VkBufferCreateInfo     *pCreateInfo,
    const VkAllocationCallbacks  *pAllocator,
    VkBuffer                     *pBuffer)
{
    if (!g_real_create_buffer || !pCreateInfo)
        return VK_ERROR_INITIALIZATION_FAILED;
    /* If a flags2 pNext is present, OR its low-32 bits into a copy of
     * the create-info. If v1.usage was 0 (DXVK supplying usage only via
     * flags2), the copy now carries real usage. */
    VkBufferUsageFlags folded = fold_buffer_flags2(pCreateInfo->pNext, pCreateInfo->usage);
    if (folded == pCreateInfo->usage)
        return g_real_create_buffer(device, pCreateInfo, pAllocator, pBuffer);
    __atomic_fetch_add(&shim_m5_buffer_flags2_fold_count, 1, __ATOMIC_RELAXED);
    VkBufferCreateInfo local = *pCreateInfo;
    local.usage |= folded;
    return g_real_create_buffer(device, &local, pAllocator, pBuffer);
}

static VkResult VKAPI_PTR shim_CreateGraphicsPipelines(
    VkDevice                              device,
    VkPipelineCache                       pipelineCache,
    uint32_t                              createInfoCount,
    const VkGraphicsPipelineCreateInfo   *pCreateInfos,
    const VkAllocationCallbacks          *pAllocator,
    VkPipeline                           *pPipelines)
{
    if (!g_real_create_graphics_pipelines || !pCreateInfos)
        return VK_ERROR_INITIALIZATION_FAILED;
    /* Materialize a folded copy only if any pCreateInfo carries flags2.
     * Most DXVK pipeline batches will have flags2 on every entry, so the
     * fast path is the all-folded copy. */
    VkGraphicsPipelineCreateInfo *local = NULL;
    int needs_fold = 0;
    for (uint32_t i = 0; i < createInfoCount; i++) {
        VkPipelineCreateFlags folded = fold_pipeline_flags2(pCreateInfos[i].pNext, pCreateInfos[i].flags);
        if (folded != pCreateInfos[i].flags) { needs_fold = 1; break; }
    }
    if (!needs_fold)
        return g_real_create_graphics_pipelines(device, pipelineCache, createInfoCount,
                                                pCreateInfos, pAllocator, pPipelines);
    __atomic_fetch_add(&shim_m5_pipeline_flags2_fold_count, 1, __ATOMIC_RELAXED);
    local = (VkGraphicsPipelineCreateInfo *)malloc(sizeof(*local) * createInfoCount);
    if (!local) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy(local, pCreateInfos, sizeof(*local) * createInfoCount);
    for (uint32_t i = 0; i < createInfoCount; i++)
        local[i].flags |= fold_pipeline_flags2(pCreateInfos[i].pNext, 0);
    VkResult r = g_real_create_graphics_pipelines(device, pipelineCache, createInfoCount,
                                                  local, pAllocator, pPipelines);
    free(local);
    return r;
}

static VkResult VKAPI_PTR shim_CreateComputePipelines(
    VkDevice                              device,
    VkPipelineCache                       pipelineCache,
    uint32_t                              createInfoCount,
    const VkComputePipelineCreateInfo    *pCreateInfos,
    const VkAllocationCallbacks          *pAllocator,
    VkPipeline                           *pPipelines)
{
    if (!g_real_create_compute_pipelines || !pCreateInfos)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkComputePipelineCreateInfo *local = NULL;
    int needs_fold = 0;
    for (uint32_t i = 0; i < createInfoCount; i++) {
        VkPipelineCreateFlags folded = fold_pipeline_flags2(pCreateInfos[i].pNext, pCreateInfos[i].flags);
        if (folded != pCreateInfos[i].flags) { needs_fold = 1; break; }
    }
    if (!needs_fold)
        return g_real_create_compute_pipelines(device, pipelineCache, createInfoCount,
                                               pCreateInfos, pAllocator, pPipelines);
    __atomic_fetch_add(&shim_m5_pipeline_flags2_fold_count, 1, __ATOMIC_RELAXED);
    local = (VkComputePipelineCreateInfo *)malloc(sizeof(*local) * createInfoCount);
    if (!local) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy(local, pCreateInfos, sizeof(*local) * createInfoCount);
    for (uint32_t i = 0; i < createInfoCount; i++)
        local[i].flags |= fold_pipeline_flags2(pCreateInfos[i].pNext, 0);
    VkResult r = g_real_create_compute_pipelines(device, pipelineCache, createInfoCount,
                                                 local, pAllocator, pPipelines);
    free(local);
    return r;
}

/* --- robustness2 nullDescriptor real-implementation ---
 *
 * Per-device set of "standin" resources. When a descriptor write
 * supplies VK_NULL_HANDLE for buffer / imageView / bufferView / sampler,
 * substitute the corresponding standin handle. Shaders read zeros from
 * the standins (they're zero-initialized once at creation), delivering
 * real read-zero semantics for every read-only descriptor type.
 *
 * Limitation called out in the file header: write-discard on storage
 * descriptor types is NOT spec-correct. All "null" storage writes go
 * to a shared standin; subsequent reads from another null storage
 * descriptor would see those writes instead of zeros. Real fix needs
 * SPIR-V instrumentation (bucket #3). Counter
 * shim_m5_null_storage_subst_count tracks how often this case fires so
 * the testbench can report the gap as INCOMPLETE.
 *
 * Standins are device-scoped (allocated lazily on first null-substitution
 * for a given device) and never freed during the device's lifetime.
 * This is the only memory-pressure-safe design: each VkImageView costs
 * ~1 mali0 mmap, so per-update creation would intersect the
 * vm.max_map_count failure mode (state_stack_wrapper.md:30). */

#include <pthread.h>

typedef struct {
    VkDevice         device;
    VkBuffer         buffer;     /* 64 KiB, zero-initialized */
    VkDeviceMemory   buffer_mem;
    VkBufferView     buffer_view;
    VkImage          image;      /* 1x1 R8G8B8A8_UNORM, zero-initialized */
    VkDeviceMemory   image_mem;
    VkImageView      image_view;
    VkSampler        sampler;
    int              ready;
} ShimStandins;

#define MAX_DEVICES 8
static ShimStandins   g_standins[MAX_DEVICES];
static uint32_t       g_standins_count = 0;
static pthread_mutex_t g_standins_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Real fn pointers needed for standin creation. Captured lazily inside
 * the GDPA hook, like the rest. */
static PFN_vkAllocateMemory                  g_real_alloc_memory = NULL;
static PFN_vkBindBufferMemory                g_real_bind_buffer_memory = NULL;
static PFN_vkBindImageMemory                 g_real_bind_image_memory = NULL;
static PFN_vkCreateBufferView                g_real_create_buffer_view = NULL;
static PFN_vkCreateImage                     g_real_create_image = NULL;
static PFN_vkCreateImageView                 g_real_create_image_view = NULL;
static PFN_vkCreateSampler                   g_real_create_sampler = NULL;
static PFN_vkGetBufferMemoryRequirements     g_real_get_buffer_mreq = NULL;
static PFN_vkGetImageMemoryRequirements      g_real_get_image_mreq = NULL;
static PFN_vkMapMemory                       g_real_map_memory = NULL;
static PFN_vkUnmapMemory                     g_real_unmap_memory = NULL;
static PFN_vkGetPhysicalDeviceMemoryProperties g_real_get_pdev_mem_props = NULL;
static PFN_vkUpdateDescriptorSets            g_real_update_descriptor_sets = NULL;
static PFN_vkCreateDescriptorUpdateTemplate  g_real_create_desc_template = NULL;
static PFN_vkDestroyDescriptorUpdateTemplate g_real_destroy_desc_template = NULL;
static PFN_vkUpdateDescriptorSetWithTemplate g_real_update_desc_with_template = NULL;
/* g_pdev_for_standins is forward-declared near the other g_real_*
 * pointers (shim_GetPhysicalDeviceFeatures2 captures into it before
 * this section's standin builder reads it). */

static int find_memory_type(VkPhysicalDevice phys, uint32_t type_bits, VkMemoryPropertyFlags want) {
    if (!g_real_get_pdev_mem_props) return -1;
    VkPhysicalDeviceMemoryProperties mp;
    g_real_get_pdev_mem_props(phys, &mp);
    /* Prefer types with all 'want' bits; fall back to any matching the bitmask. */
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if (!(type_bits & (1u << i))) continue;
        if ((mp.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    }
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if (type_bits & (1u << i)) return (int)i;
    }
    return -1;
}

static ShimStandins *find_or_create_standins_slot(VkDevice device) {
    /* Caller holds g_standins_mutex. */
    for (uint32_t i = 0; i < g_standins_count; i++)
        if (g_standins[i].device == device) return &g_standins[i];
    if (g_standins_count >= MAX_DEVICES) return NULL;
    ShimStandins *s = &g_standins[g_standins_count++];
    memset(s, 0, sizeof(*s));
    s->device = device;
    return s;
}

/* Build all standin resources for the given device. Assumes the real
 * Vulkan fn pointers have been captured. Returns 1 on success, 0 on
 * any failure (in which case nullDescriptor substitution will fall
 * through to passing VK_NULL_HANDLE through, and the wrapper will
 * presumably fault — same as no shim). */
static int build_standins(ShimStandins *s) {
    VkPhysicalDevice phys = g_pdev_for_standins;
    if (phys == VK_NULL_HANDLE || !g_real_create_buffer || !g_real_alloc_memory ||
        !g_real_bind_buffer_memory || !g_real_create_buffer_view ||
        !g_real_create_image || !g_real_bind_image_memory || !g_real_create_image_view ||
        !g_real_create_sampler || !g_real_get_buffer_mreq || !g_real_get_image_mreq) {
        fprintf(stderr, "[shim_maintenance5] standin init: missing real fn pointers\n");
        return 0;
    }
    VkDevice device = s->device;

    /* Buffer: 64 KiB, all-bits usage, host-visible so we can zero it. */
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 64 * 1024,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (g_real_create_buffer(device, &bci, NULL, &s->buffer) != VK_SUCCESS) {
        fprintf(stderr, "[shim_maintenance5] standin buffer create failed\n");
        return 0;
    }
    VkMemoryRequirements breq;
    g_real_get_buffer_mreq(device, s->buffer, &breq);
    int bmt = find_memory_type(phys, breq.memoryTypeBits,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (bmt < 0) bmt = find_memory_type(phys, breq.memoryTypeBits, 0);
    if (bmt < 0) { fprintf(stderr, "[shim_maintenance5] no memory type for standin buffer\n"); return 0; }
    VkMemoryAllocateInfo bmai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                  .allocationSize = breq.size, .memoryTypeIndex = (uint32_t)bmt };
    if (g_real_alloc_memory(device, &bmai, NULL, &s->buffer_mem) != VK_SUCCESS) {
        fprintf(stderr, "[shim_maintenance5] standin buffer memory alloc failed\n"); return 0;
    }
    g_real_bind_buffer_memory(device, s->buffer, s->buffer_mem, 0);
    /* Zero the memory if mappable. If it isn't, the buffer is whatever
     * the driver leaves it as — usually zero from VK_MEMORY_PROPERTY_*
     * spec, but not guaranteed. */
    if (g_real_map_memory && g_real_unmap_memory) {
        void *p = NULL;
        if (g_real_map_memory(device, s->buffer_mem, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS && p) {
            memset(p, 0, (size_t)breq.size);
            g_real_unmap_memory(device, s->buffer_mem);
        }
    }
    /* Texel buffer view onto the same buffer (R32_UINT — innocuous format
     * that's universally supported). */
    VkBufferViewCreateInfo bvci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
        .buffer = s->buffer, .format = VK_FORMAT_R32_UINT, .offset = 0, .range = VK_WHOLE_SIZE,
    };
    if (g_real_create_buffer_view(device, &bvci, NULL, &s->buffer_view) != VK_SUCCESS) {
        fprintf(stderr, "[shim_maintenance5] standin buffer view create failed\n"); return 0;
    }

    /* Image: 1x1 R8G8B8A8_UNORM, SAMPLED|STORAGE|TRANSFER_DST so it can
     * substitute for any image descriptor type. Layout is UNDEFINED
     * after creation; for SAMPLED we'd ordinarily want SHADER_READ_ONLY,
     * but we never transition the image — most drivers tolerate
     * sampling from UNDEFINED with the result being implementation-defined
     * (typically zero on Mali). For real correctness we'd record a
     * one-shot transfer command buffer to clear it; deferring that to a
     * future pass. */
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { 1, 1, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (g_real_create_image(device, &ici, NULL, &s->image) != VK_SUCCESS) {
        fprintf(stderr, "[shim_maintenance5] standin image create failed\n"); return 0;
    }
    VkMemoryRequirements ireq;
    g_real_get_image_mreq(device, s->image, &ireq);
    int imt = find_memory_type(phys, ireq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (imt < 0) imt = find_memory_type(phys, ireq.memoryTypeBits, 0);
    if (imt < 0) { fprintf(stderr, "[shim_maintenance5] no memory type for standin image\n"); return 0; }
    VkMemoryAllocateInfo imai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                  .allocationSize = ireq.size, .memoryTypeIndex = (uint32_t)imt };
    if (g_real_alloc_memory(device, &imai, NULL, &s->image_mem) != VK_SUCCESS) {
        fprintf(stderr, "[shim_maintenance5] standin image memory alloc failed\n"); return 0;
    }
    g_real_bind_image_memory(device, s->image, s->image_mem, 0);
    VkImageViewCreateInfo ivci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = s->image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    if (g_real_create_image_view(device, &ivci, NULL, &s->image_view) != VK_SUCCESS) {
        fprintf(stderr, "[shim_maintenance5] standin image view create failed\n"); return 0;
    }

    /* Default sampler — identity. */
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
    };
    if (g_real_create_sampler(device, &sci, NULL, &s->sampler) != VK_SUCCESS) {
        fprintf(stderr, "[shim_maintenance5] standin sampler create failed\n"); return 0;
    }

    s->ready = 1;
    return 1;
}

static ShimStandins *get_standins(VkDevice device) {
    pthread_mutex_lock(&g_standins_mutex);
    ShimStandins *s = find_or_create_standins_slot(device);
    if (s && !s->ready) build_standins(s);
    pthread_mutex_unlock(&g_standins_mutex);
    return (s && s->ready) ? s : NULL;
}

/* Per-template entry mirror: copied at vkCreateDescriptorUpdateTemplate
 * time so we know where to look in the data blob at update time. */
typedef struct ShimTemplateMirror {
    VkDescriptorUpdateTemplate            handle;
    VkDevice                              device;
    uint32_t                              entry_count;
    VkDescriptorUpdateTemplateEntry      *entries;
    struct ShimTemplateMirror            *next;
} ShimTemplateMirror;

static ShimTemplateMirror *g_templates = NULL;
static pthread_mutex_t     g_templates_mutex = PTHREAD_MUTEX_INITIALIZER;

static int desc_type_is_storage(VkDescriptorType t) {
    return t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
           t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC ||
           t == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
           t == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
}

/* Substitute nulls in-place in a single descriptor element. Returns
 * 1 if a substitution happened. */
static int subst_nulls_in_descriptor(
    VkDescriptorType type,
    void *element /* VkDescriptorImageInfo* / VkDescriptorBufferInfo* / VkBufferView* */,
    ShimStandins *s)
{
    int hit = 0;
    switch (type) {
    case VK_DESCRIPTOR_TYPE_SAMPLER: {
        VkDescriptorImageInfo *ii = (VkDescriptorImageInfo *)element;
        if (ii->sampler == VK_NULL_HANDLE) { ii->sampler = s->sampler; hit = 1; }
    } break;
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: {
        VkDescriptorImageInfo *ii = (VkDescriptorImageInfo *)element;
        if (ii->sampler   == VK_NULL_HANDLE) { ii->sampler   = s->sampler;    hit = 1; }
        if (ii->imageView == VK_NULL_HANDLE) { ii->imageView = s->image_view; hit = 1; }
    } break;
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
    case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: {
        VkDescriptorImageInfo *ii = (VkDescriptorImageInfo *)element;
        if (ii->imageView == VK_NULL_HANDLE) { ii->imageView = s->image_view; hit = 1; }
    } break;
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: {
        VkDescriptorBufferInfo *bi = (VkDescriptorBufferInfo *)element;
        if (bi->buffer == VK_NULL_HANDLE) {
            bi->buffer = s->buffer;
            if (bi->range == 0 || bi->range == VK_WHOLE_SIZE) bi->range = VK_WHOLE_SIZE;
            bi->offset = 0;
            hit = 1;
        }
    } break;
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: {
        VkBufferView *bv = (VkBufferView *)element;
        if (*bv == VK_NULL_HANDLE) { *bv = s->buffer_view; hit = 1; }
    } break;
    default: break;
    }
    if (hit) {
        __atomic_fetch_add(&shim_m5_null_subst_count, 1, __ATOMIC_RELAXED);
        if (desc_type_is_storage(type))
            __atomic_fetch_add(&shim_m5_null_storage_subst_count, 1, __ATOMIC_RELAXED);
    }
    return hit;
}

static void VKAPI_PTR shim_UpdateDescriptorSets(
    VkDevice                          device,
    uint32_t                          descriptorWriteCount,
    const VkWriteDescriptorSet       *pDescriptorWrites,
    uint32_t                          descriptorCopyCount,
    const VkCopyDescriptorSet        *pDescriptorCopies)
{
    if (!g_real_update_descriptor_sets) return;
    if (descriptorWriteCount == 0) {
        g_real_update_descriptor_sets(device, 0, NULL, descriptorCopyCount, pDescriptorCopies);
        return;
    }
    /* Scan to see whether ANY write needs substitution. If not, fast-path
     * straight through. */
    int any_null = 0;
    for (uint32_t w = 0; w < descriptorWriteCount && !any_null; w++) {
        const VkWriteDescriptorSet *W = &pDescriptorWrites[w];
        for (uint32_t e = 0; e < W->descriptorCount && !any_null; e++) {
            switch (W->descriptorType) {
            case VK_DESCRIPTOR_TYPE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: {
                if (!W->pImageInfo) break;
                const VkDescriptorImageInfo *ii = &W->pImageInfo[e];
                if (W->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER && ii->sampler == VK_NULL_HANDLE) any_null = 1;
                else if (W->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
                         (ii->sampler == VK_NULL_HANDLE || ii->imageView == VK_NULL_HANDLE)) any_null = 1;
                else if (ii->imageView == VK_NULL_HANDLE) any_null = 1;
            } break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: {
                if (!W->pBufferInfo) break;
                if (W->pBufferInfo[e].buffer == VK_NULL_HANDLE) any_null = 1;
            } break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: {
                if (!W->pTexelBufferView) break;
                if (W->pTexelBufferView[e] == VK_NULL_HANDLE) any_null = 1;
            } break;
            default: break;
            }
        }
    }
    if (!any_null) {
        g_real_update_descriptor_sets(device, descriptorWriteCount, pDescriptorWrites,
                                       descriptorCopyCount, pDescriptorCopies);
        return;
    }
    ShimStandins *s = get_standins(device);
    if (!s) {
        /* Standin init failed — fall through and let the wrapper deal with the nulls. */
        g_real_update_descriptor_sets(device, descriptorWriteCount, pDescriptorWrites,
                                       descriptorCopyCount, pDescriptorCopies);
        return;
    }
    /* Materialize a deep copy of the writes so we can mutate. Caller's
     * arrays are const; we own the copies. Worst case is one allocation
     * per nullable inner array; coalesce by counting first. */
    VkWriteDescriptorSet *writes = (VkWriteDescriptorSet *)malloc(sizeof(*writes) * descriptorWriteCount);
    if (!writes) {
        g_real_update_descriptor_sets(device, descriptorWriteCount, pDescriptorWrites,
                                       descriptorCopyCount, pDescriptorCopies);
        return;
    }
    memcpy(writes, pDescriptorWrites, sizeof(*writes) * descriptorWriteCount);

    /* For each write that has nullable inner-array entries, allocate and
     * fill a mutable copy. We free all of them at the end. */
    void **owned = (void **)calloc(descriptorWriteCount, sizeof(void *));
    if (!owned) { free(writes); return; }

    for (uint32_t w = 0; w < descriptorWriteCount; w++) {
        VkWriteDescriptorSet *W = &writes[w];
        if (W->descriptorCount == 0) continue;
        switch (W->descriptorType) {
        case VK_DESCRIPTOR_TYPE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: {
            if (!W->pImageInfo) break;
            VkDescriptorImageInfo *copy = (VkDescriptorImageInfo *)malloc(sizeof(*copy) * W->descriptorCount);
            if (!copy) break;
            memcpy(copy, W->pImageInfo, sizeof(*copy) * W->descriptorCount);
            for (uint32_t e = 0; e < W->descriptorCount; e++)
                subst_nulls_in_descriptor(W->descriptorType, &copy[e], s);
            owned[w] = copy;
            W->pImageInfo = copy;
        } break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: {
            if (!W->pBufferInfo) break;
            VkDescriptorBufferInfo *copy = (VkDescriptorBufferInfo *)malloc(sizeof(*copy) * W->descriptorCount);
            if (!copy) break;
            memcpy(copy, W->pBufferInfo, sizeof(*copy) * W->descriptorCount);
            for (uint32_t e = 0; e < W->descriptorCount; e++)
                subst_nulls_in_descriptor(W->descriptorType, &copy[e], s);
            owned[w] = copy;
            W->pBufferInfo = copy;
        } break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: {
            if (!W->pTexelBufferView) break;
            VkBufferView *copy = (VkBufferView *)malloc(sizeof(*copy) * W->descriptorCount);
            if (!copy) break;
            memcpy(copy, W->pTexelBufferView, sizeof(*copy) * W->descriptorCount);
            for (uint32_t e = 0; e < W->descriptorCount; e++)
                subst_nulls_in_descriptor(W->descriptorType, &copy[e], s);
            owned[w] = copy;
            W->pTexelBufferView = copy;
        } break;
        default: break;
        }
    }

    g_real_update_descriptor_sets(device, descriptorWriteCount, writes,
                                  descriptorCopyCount, pDescriptorCopies);

    for (uint32_t w = 0; w < descriptorWriteCount; w++) free(owned[w]);
    free(owned);
    free(writes);
}

static VkResult VKAPI_PTR shim_CreateDescriptorUpdateTemplate(
    VkDevice                                       device,
    const VkDescriptorUpdateTemplateCreateInfo    *pCreateInfo,
    const VkAllocationCallbacks                   *pAllocator,
    VkDescriptorUpdateTemplate                    *pDescriptorUpdateTemplate)
{
    if (!g_real_create_desc_template) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = g_real_create_desc_template(device, pCreateInfo, pAllocator, pDescriptorUpdateTemplate);
    if (r != VK_SUCCESS) return r;
    /* Mirror the entries so we can find descriptors in the data blob at update time. */
    ShimTemplateMirror *m = (ShimTemplateMirror *)calloc(1, sizeof(*m));
    if (!m) return r; /* template still works; null-substitution won't fire for it */
    m->handle = *pDescriptorUpdateTemplate;
    m->device = device;
    m->entry_count = pCreateInfo->descriptorUpdateEntryCount;
    if (m->entry_count > 0) {
        m->entries = (VkDescriptorUpdateTemplateEntry *)malloc(sizeof(*m->entries) * m->entry_count);
        if (!m->entries) { free(m); return r; }
        memcpy(m->entries, pCreateInfo->pDescriptorUpdateEntries, sizeof(*m->entries) * m->entry_count);
    }
    pthread_mutex_lock(&g_templates_mutex);
    m->next = g_templates;
    g_templates = m;
    pthread_mutex_unlock(&g_templates_mutex);
    return r;
}

static void VKAPI_PTR shim_DestroyDescriptorUpdateTemplate(
    VkDevice                              device,
    VkDescriptorUpdateTemplate            descriptorUpdateTemplate,
    const VkAllocationCallbacks          *pAllocator)
{
    pthread_mutex_lock(&g_templates_mutex);
    ShimTemplateMirror **pp = &g_templates;
    while (*pp) {
        if ((*pp)->handle == descriptorUpdateTemplate) {
            ShimTemplateMirror *dead = *pp;
            *pp = dead->next;
            free(dead->entries);
            free(dead);
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_templates_mutex);
    if (g_real_destroy_desc_template)
        g_real_destroy_desc_template(device, descriptorUpdateTemplate, pAllocator);
}

static void VKAPI_PTR shim_UpdateDescriptorSetWithTemplate(
    VkDevice                              device,
    VkDescriptorSet                       descriptorSet,
    VkDescriptorUpdateTemplate            descriptorUpdateTemplate,
    const void                           *pData)
{
    if (!g_real_update_desc_with_template) return;
    /* Find the template's entry mirror. */
    pthread_mutex_lock(&g_templates_mutex);
    ShimTemplateMirror *m = g_templates;
    while (m && m->handle != descriptorUpdateTemplate) m = m->next;
    /* Decide whether any entry has a null. We have to walk the data
     * blob with the entries' offsets. */
    int any_null = 0;
    if (m) {
        for (uint32_t i = 0; i < m->entry_count && !any_null; i++) {
            const VkDescriptorUpdateTemplateEntry *E = &m->entries[i];
            for (uint32_t e = 0; e < E->descriptorCount && !any_null; e++) {
                const char *slot = (const char *)pData + E->offset + e * E->stride;
                switch (E->descriptorType) {
                case VK_DESCRIPTOR_TYPE_SAMPLER: {
                    const VkDescriptorImageInfo *ii = (const VkDescriptorImageInfo *)slot;
                    if (ii->sampler == VK_NULL_HANDLE) any_null = 1;
                } break;
                case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: {
                    const VkDescriptorImageInfo *ii = (const VkDescriptorImageInfo *)slot;
                    if (ii->sampler == VK_NULL_HANDLE || ii->imageView == VK_NULL_HANDLE) any_null = 1;
                } break;
                case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: {
                    const VkDescriptorImageInfo *ii = (const VkDescriptorImageInfo *)slot;
                    if (ii->imageView == VK_NULL_HANDLE) any_null = 1;
                } break;
                case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
                case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: {
                    const VkDescriptorBufferInfo *bi = (const VkDescriptorBufferInfo *)slot;
                    if (bi->buffer == VK_NULL_HANDLE) any_null = 1;
                } break;
                case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: {
                    const VkBufferView *bv = (const VkBufferView *)slot;
                    if (*bv == VK_NULL_HANDLE) any_null = 1;
                } break;
                default: break;
                }
            }
        }
    }
    pthread_mutex_unlock(&g_templates_mutex);

    if (!any_null || !m) {
        g_real_update_desc_with_template(device, descriptorSet, descriptorUpdateTemplate, pData);
        return;
    }
    ShimStandins *s = get_standins(device);
    if (!s) {
        g_real_update_desc_with_template(device, descriptorSet, descriptorUpdateTemplate, pData);
        return;
    }
    /* Need to rewrite the data blob. We don't know its total size from
     * the template alone, so compute it from the highest (offset + stride
     * * descriptorCount) seen across entries. The caller's blob is at
     * least that large. */
    size_t blob_size = 0;
    pthread_mutex_lock(&g_templates_mutex);
    for (uint32_t i = 0; i < m->entry_count; i++) {
        const VkDescriptorUpdateTemplateEntry *E = &m->entries[i];
        size_t entry_extent;
        switch (E->descriptorType) {
        case VK_DESCRIPTOR_TYPE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            entry_extent = sizeof(VkDescriptorImageInfo); break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
            entry_extent = sizeof(VkDescriptorBufferInfo); break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            entry_extent = sizeof(VkBufferView); break;
        default: entry_extent = E->stride; break;
        }
        size_t end = E->offset + (size_t)E->stride * (E->descriptorCount > 0 ? E->descriptorCount - 1 : 0) + entry_extent;
        if (end > blob_size) blob_size = end;
    }
    pthread_mutex_unlock(&g_templates_mutex);

    void *blob = malloc(blob_size);
    if (!blob) {
        g_real_update_desc_with_template(device, descriptorSet, descriptorUpdateTemplate, pData);
        return;
    }
    memcpy(blob, pData, blob_size);
    pthread_mutex_lock(&g_templates_mutex);
    for (uint32_t i = 0; i < m->entry_count; i++) {
        const VkDescriptorUpdateTemplateEntry *E = &m->entries[i];
        for (uint32_t e = 0; e < E->descriptorCount; e++) {
            char *slot = (char *)blob + E->offset + (size_t)e * E->stride;
            subst_nulls_in_descriptor(E->descriptorType, slot, s);
        }
    }
    pthread_mutex_unlock(&g_templates_mutex);

    g_real_update_desc_with_template(device, descriptorSet, descriptorUpdateTemplate, blob);
    free(blob);
}

/* --- enumeration intercept (unchanged from earlier demo) --- */

static VkResult VKAPI_PTR shim_EnumerateDeviceExtensionProperties(
    VkPhysicalDevice                            physicalDevice,
    const char                                 *pLayerName,
    uint32_t                                   *pPropertyCount,
    VkExtensionProperties                      *pProperties)
{
    open_real_lib_once();
    PFN_vkEnumerateDeviceExtensionProperties real_enum = g_real_enum;
    if (!real_enum) return VK_ERROR_INITIALIZATION_FAILED;

    if (!pProperties) {
        VkResult r = real_enum(physicalDevice, pLayerName, pPropertyCount, NULL);
        if (r != VK_SUCCESS && r != VK_INCOMPLETE) return r;
        *pPropertyCount += INJECTED_EXTS_COUNT;
        return VK_SUCCESS;
    }

    uint32_t cap = *pPropertyCount;
    uint32_t real_cap = (cap > INJECTED_EXTS_COUNT) ? cap - INJECTED_EXTS_COUNT : 0;
    uint32_t real_filled = real_cap;
    VkResult r = real_enum(physicalDevice, pLayerName, &real_filled, pProperties);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) return r;

    /* Append injected extensions until we run out of caller-supplied space. */
    uint32_t injected = 0;
    while (injected < INJECTED_EXTS_COUNT && real_filled + injected < cap) {
        VkExtensionProperties *slot = &pProperties[real_filled + injected];
        memset(slot, 0, sizeof(*slot));
        strncpy(slot->extensionName, INJECTED_EXTS[injected].name, VK_MAX_EXTENSION_NAME_SIZE - 1);
        slot->specVersion = INJECTED_EXTS[injected].spec_version;
        injected++;
    }
    *pPropertyCount = real_filled + injected;
    return (injected == INJECTED_EXTS_COUNT) ? VK_SUCCESS : VK_INCOMPLETE;
}

/* --- device-level GPA intercept --- */

static PFN_vkVoidFunction VKAPI_PTR shim_GetDeviceProcAddr(
    VkDevice device, const char *pName)
{
    if (!g_real_gdpa || !pName) return NULL;

    /* Lazy capture of the real entries our shims forward to. */
    if (!g_real_cmd_bind_index)
        g_real_cmd_bind_index = (PFN_vkCmdBindIndexBuffer)
            g_real_gdpa(device, "vkCmdBindIndexBuffer");
    if (!g_real_get_isl)
        g_real_get_isl = (PFN_vkGetImageSubresourceLayout)
            g_real_gdpa(device, "vkGetImageSubresourceLayout");
    if (!g_real_create_buffer)
        g_real_create_buffer = (PFN_vkCreateBuffer)
            g_real_gdpa(device, "vkCreateBuffer");
    if (!g_real_create_graphics_pipelines)
        g_real_create_graphics_pipelines = (PFN_vkCreateGraphicsPipelines)
            g_real_gdpa(device, "vkCreateGraphicsPipelines");
    if (!g_real_create_compute_pipelines)
        g_real_create_compute_pipelines = (PFN_vkCreateComputePipelines)
            g_real_gdpa(device, "vkCreateComputePipelines");
    if (!g_real_create_shader_module)
        g_real_create_shader_module = (PFN_vkCreateShaderModule)
            g_real_gdpa(device, "vkCreateShaderModule");
    /* Real fn pointers for standin construction + descriptor-write substitution. */
    if (!g_real_alloc_memory)
        g_real_alloc_memory = (PFN_vkAllocateMemory)g_real_gdpa(device, "vkAllocateMemory");
    if (!g_real_bind_buffer_memory)
        g_real_bind_buffer_memory = (PFN_vkBindBufferMemory)g_real_gdpa(device, "vkBindBufferMemory");
    if (!g_real_bind_image_memory)
        g_real_bind_image_memory = (PFN_vkBindImageMemory)g_real_gdpa(device, "vkBindImageMemory");
    if (!g_real_create_buffer_view)
        g_real_create_buffer_view = (PFN_vkCreateBufferView)g_real_gdpa(device, "vkCreateBufferView");
    if (!g_real_create_image)
        g_real_create_image = (PFN_vkCreateImage)g_real_gdpa(device, "vkCreateImage");
    if (!g_real_create_image_view)
        g_real_create_image_view = (PFN_vkCreateImageView)g_real_gdpa(device, "vkCreateImageView");
    if (!g_real_create_sampler)
        g_real_create_sampler = (PFN_vkCreateSampler)g_real_gdpa(device, "vkCreateSampler");
    if (!g_real_get_buffer_mreq)
        g_real_get_buffer_mreq = (PFN_vkGetBufferMemoryRequirements)g_real_gdpa(device, "vkGetBufferMemoryRequirements");
    if (!g_real_get_image_mreq)
        g_real_get_image_mreq = (PFN_vkGetImageMemoryRequirements)g_real_gdpa(device, "vkGetImageMemoryRequirements");
    if (!g_real_map_memory)
        g_real_map_memory = (PFN_vkMapMemory)g_real_gdpa(device, "vkMapMemory");
    if (!g_real_unmap_memory)
        g_real_unmap_memory = (PFN_vkUnmapMemory)g_real_gdpa(device, "vkUnmapMemory");
    if (!g_real_update_descriptor_sets)
        g_real_update_descriptor_sets = (PFN_vkUpdateDescriptorSets)g_real_gdpa(device, "vkUpdateDescriptorSets");
    if (!g_real_create_desc_template)
        g_real_create_desc_template = (PFN_vkCreateDescriptorUpdateTemplate)
            g_real_gdpa(device, "vkCreateDescriptorUpdateTemplate");
    if (!g_real_destroy_desc_template)
        g_real_destroy_desc_template = (PFN_vkDestroyDescriptorUpdateTemplate)
            g_real_gdpa(device, "vkDestroyDescriptorUpdateTemplate");
    if (!g_real_update_desc_with_template)
        g_real_update_desc_with_template = (PFN_vkUpdateDescriptorSetWithTemplate)
            g_real_gdpa(device, "vkUpdateDescriptorSetWithTemplate");

    if (!strcmp(pName, "vkCmdBindIndexBuffer2KHR"))
        return (PFN_vkVoidFunction)shim_CmdBindIndexBuffer2KHR;
    if (!strcmp(pName, "vkGetRenderingAreaGranularityKHR"))
        return (PFN_vkVoidFunction)shim_GetRenderingAreaGranularityKHR;
    if (!strcmp(pName, "vkGetImageSubresourceLayout2KHR"))
        return (PFN_vkVoidFunction)shim_GetImageSubresourceLayout2KHR;
    if (!strcmp(pName, "vkGetDeviceImageSubresourceLayoutKHR"))
        return (PFN_vkVoidFunction)shim_GetDeviceImageSubresourceLayoutKHR;
    if (!strcmp(pName, "vkCreateBuffer"))
        return (PFN_vkVoidFunction)shim_CreateBuffer;
    if (!strcmp(pName, "vkCreateGraphicsPipelines"))
        return (PFN_vkVoidFunction)shim_CreateGraphicsPipelines;
    if (!strcmp(pName, "vkCreateComputePipelines"))
        return (PFN_vkVoidFunction)shim_CreateComputePipelines;
    if (!strcmp(pName, "vkCreateShaderModule"))
        return (PFN_vkVoidFunction)shim_CreateShaderModule;
    if (!strcmp(pName, "vkUpdateDescriptorSets"))
        return (PFN_vkVoidFunction)shim_UpdateDescriptorSets;
    if (!strcmp(pName, "vkCreateDescriptorUpdateTemplate") ||
        !strcmp(pName, "vkCreateDescriptorUpdateTemplateKHR"))
        return (PFN_vkVoidFunction)shim_CreateDescriptorUpdateTemplate;
    if (!strcmp(pName, "vkDestroyDescriptorUpdateTemplate") ||
        !strcmp(pName, "vkDestroyDescriptorUpdateTemplateKHR"))
        return (PFN_vkVoidFunction)shim_DestroyDescriptorUpdateTemplate;
    if (!strcmp(pName, "vkUpdateDescriptorSetWithTemplate") ||
        !strcmp(pName, "vkUpdateDescriptorSetWithTemplateKHR"))
        return (PFN_vkVoidFunction)shim_UpdateDescriptorSetWithTemplate;

    return g_real_gdpa(device, pName);
}

/* --- instance-level GPA intercept --- */

static PFN_vkVoidFunction shim_GetInstanceProcAddr(
    VkInstance instance, const char *pName)
{
    open_real_lib_once();
    if (!g_real_gipa || !pName) return NULL;

    if (!strcmp(pName, "vkEnumerateDeviceExtensionProperties")) {
        if (instance != VK_NULL_HANDLE && !g_real_enum) {
            g_real_enum = (PFN_vkEnumerateDeviceExtensionProperties)
                g_real_gipa(instance, pName);
        }
        return (PFN_vkVoidFunction)shim_EnumerateDeviceExtensionProperties;
    }

    if (!strcmp(pName, "vkGetDeviceProcAddr")) {
        if (instance != VK_NULL_HANDLE && !g_real_gdpa) {
            g_real_gdpa = (PFN_vkGetDeviceProcAddr)
                g_real_gipa(instance, pName);
        }
        return (PFN_vkVoidFunction)shim_GetDeviceProcAddr;
    }

    if (!strcmp(pName, "vkGetPhysicalDeviceFeatures2") ||
        !strcmp(pName, "vkGetPhysicalDeviceFeatures2KHR")) {
        if (instance != VK_NULL_HANDLE && !g_real_gpdf2) {
            g_real_gpdf2 = (PFN_vkGetPhysicalDeviceFeatures2)
                g_real_gipa(instance, pName);
        }
        return (PFN_vkVoidFunction)shim_GetPhysicalDeviceFeatures2;
    }

    if (!strcmp(pName, "vkGetPhysicalDeviceProperties2") ||
        !strcmp(pName, "vkGetPhysicalDeviceProperties2KHR")) {
        if (instance != VK_NULL_HANDLE && !g_real_gpdp2) {
            g_real_gpdp2 = (PFN_vkGetPhysicalDeviceProperties2)
                g_real_gipa(instance, pName);
        }
        return (PFN_vkVoidFunction)shim_GetPhysicalDeviceProperties2;
    }

    /* Pass-through with capture: standin construction needs this fn at
     * device-creation time but we never intercept its behavior. */
    if (!strcmp(pName, "vkGetPhysicalDeviceMemoryProperties")) {
        if (instance != VK_NULL_HANDLE && !g_real_get_pdev_mem_props)
            g_real_get_pdev_mem_props = (PFN_vkGetPhysicalDeviceMemoryProperties)
                g_real_gipa(instance, pName);
        return (PFN_vkVoidFunction)g_real_get_pdev_mem_props;
    }

    /* Per Vulkan spec, instance-level GIPA also returns device entries.
     * Some callers (DXVK among them) resolve device entries via instance
     * GIPA rather than vkGetDeviceProcAddr. */
    if (!strcmp(pName, "vkCmdBindIndexBuffer2KHR"))
        return (PFN_vkVoidFunction)shim_CmdBindIndexBuffer2KHR;
    if (!strcmp(pName, "vkGetRenderingAreaGranularityKHR"))
        return (PFN_vkVoidFunction)shim_GetRenderingAreaGranularityKHR;
    if (!strcmp(pName, "vkGetImageSubresourceLayout2KHR"))
        return (PFN_vkVoidFunction)shim_GetImageSubresourceLayout2KHR;
    if (!strcmp(pName, "vkGetDeviceImageSubresourceLayoutKHR"))
        return (PFN_vkVoidFunction)shim_GetDeviceImageSubresourceLayoutKHR;
    if (!strcmp(pName, "vkCreateBuffer")) {
        if (instance != VK_NULL_HANDLE && !g_real_create_buffer)
            g_real_create_buffer = (PFN_vkCreateBuffer)g_real_gipa(instance, pName);
        return (PFN_vkVoidFunction)shim_CreateBuffer;
    }
    if (!strcmp(pName, "vkCreateGraphicsPipelines")) {
        if (instance != VK_NULL_HANDLE && !g_real_create_graphics_pipelines)
            g_real_create_graphics_pipelines = (PFN_vkCreateGraphicsPipelines)g_real_gipa(instance, pName);
        return (PFN_vkVoidFunction)shim_CreateGraphicsPipelines;
    }
    if (!strcmp(pName, "vkCreateComputePipelines")) {
        if (instance != VK_NULL_HANDLE && !g_real_create_compute_pipelines)
            g_real_create_compute_pipelines = (PFN_vkCreateComputePipelines)g_real_gipa(instance, pName);
        return (PFN_vkVoidFunction)shim_CreateComputePipelines;
    }
    if (!strcmp(pName, "vkCreateShaderModule")) {
        if (instance != VK_NULL_HANDLE && !g_real_create_shader_module)
            g_real_create_shader_module = (PFN_vkCreateShaderModule)g_real_gipa(instance, pName);
        return (PFN_vkVoidFunction)shim_CreateShaderModule;
    }

    return g_real_gipa(instance, pName);
}

/* ---- exported ICD entrypoints ---- */

__attribute__((visibility("default")))
VkResult vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pSupportedVersion)
{
    open_real_lib_once();
    if (!g_real_lib) return VK_ERROR_INITIALIZATION_FAILED;
    typedef VkResult (*PFN_neg)(uint32_t *);
    PFN_neg neg = (PFN_neg)dlsym(g_real_lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
    if (!neg) return VK_ERROR_INITIALIZATION_FAILED;
    return neg(pSupportedVersion);
}

__attribute__((visibility("default")))
PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName)
{
    return shim_GetInstanceProcAddr(instance, pName);
}

__attribute__((visibility("default")))
PFN_vkVoidFunction vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char *pName)
{
    open_real_lib_once();
    if (!g_real_lib) return NULL;
    typedef PFN_vkVoidFunction (*PFN_pdpa)(VkInstance, const char *);
    PFN_pdpa pdpa = (PFN_pdpa)dlsym(g_real_lib, "vk_icdGetPhysicalDeviceProcAddr");
    if (!pdpa) return NULL;
    return pdpa(instance, pName);
}
