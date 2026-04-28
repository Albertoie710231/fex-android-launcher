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

#define WRAPPED_LIB "/data/data/com.mediatek.steamlauncher/files/imagefs_bionic/usr/lib/libvulkan_wrapper.so"
#define INJECTED_EXT "VK_KHR_maintenance5"
#define INJECTED_SPEC_VERSION 1

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

/* Exposed counters so the testbench can prove a fold actually fired,
 * not just that vkCreateBuffer happened to succeed (the wrapper is lax
 * about usage=0, so success-on-create is not a fold-correctness signal
 * by itself). dlsym'd from the testbench. */
__attribute__((visibility("default"))) volatile int shim_m5_buffer_flags2_fold_count = 0;
__attribute__((visibility("default"))) volatile int shim_m5_pipeline_flags2_fold_count = 0;

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
    if (g_real_gpdf2) g_real_gpdf2(physicalDevice, pFeatures);
    if (!pFeatures) return;
    VkBaseOutStructure *p = (VkBaseOutStructure *)pFeatures->pNext;
    while (p) {
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR) {
            VkPhysicalDeviceMaintenance5FeaturesKHR *m =
                (VkPhysicalDeviceMaintenance5FeaturesKHR *)p;
            m->maintenance5 = VK_TRUE;
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
        *pPropertyCount += 1;
        return VK_SUCCESS;
    }

    uint32_t cap = *pPropertyCount;
    uint32_t real_cap = (cap > 0) ? cap - 1 : 0;
    uint32_t real_filled = real_cap;
    VkResult r = real_enum(physicalDevice, pLayerName, &real_filled, pProperties);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) return r;

    if (real_filled < cap) {
        VkExtensionProperties *slot = &pProperties[real_filled];
        memset(slot, 0, sizeof(*slot));
        strncpy(slot->extensionName, INJECTED_EXT, VK_MAX_EXTENSION_NAME_SIZE - 1);
        slot->specVersion = INJECTED_SPEC_VERSION;
        *pPropertyCount = real_filled + 1;
        return VK_SUCCESS;
    }
    *pPropertyCount = real_filled;
    return VK_INCOMPLETE;
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
