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

    if (!strcmp(pName, "vkCmdBindIndexBuffer2KHR"))
        return (PFN_vkVoidFunction)shim_CmdBindIndexBuffer2KHR;
    if (!strcmp(pName, "vkGetRenderingAreaGranularityKHR"))
        return (PFN_vkVoidFunction)shim_GetRenderingAreaGranularityKHR;
    if (!strcmp(pName, "vkGetImageSubresourceLayout2KHR"))
        return (PFN_vkVoidFunction)shim_GetImageSubresourceLayout2KHR;
    if (!strcmp(pName, "vkGetDeviceImageSubresourceLayoutKHR"))
        return (PFN_vkVoidFunction)shim_GetDeviceImageSubresourceLayoutKHR;

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
