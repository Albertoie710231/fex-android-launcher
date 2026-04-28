/*
 * shim_maintenance5 — minimal ICD-wrapping shim that *claims*
 * VK_KHR_maintenance5 in the device extension enumeration but does
 * NOT implement its entrypoints. Purpose: validate that the testbench
 * accurately distinguishes "claimed by enum" from "actually
 * dispatchable" — the basic correctness check before we invest in
 * a real wrapper-side shim.
 *
 * Architecture: forwards every Khronos ICD entrypoint to the real
 * bionic-vulkan-wrapper.so via dlopen + symbol resolution. The only
 * intercept is vkEnumerateDeviceExtensionProperties (returned through
 * a hooked vkGetInstanceProcAddr), which appends VK_KHR_maintenance5
 * to whatever the real wrapper reported.
 *
 * Use: set the testbench's WRAPPER_TESTBENCH_LIB env var to point at
 * this .so instead of the real wrapper. The shim dlopens the real one
 * by absolute path on first call.
 *
 * Real wrapper path is hardcoded — change WRAPPED_LIB if redeployed
 * elsewhere. Keeping this trivial; a production shim would discover
 * the path at config time.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#define WRAPPED_LIB "/data/data/com.mediatek.steamlauncher/files/imagefs_bionic/usr/lib/libvulkan_wrapper.so"
#define INJECTED_EXT "VK_KHR_maintenance5"
#define INJECTED_SPEC_VERSION 1

static void                            *g_real_lib = NULL;
static PFN_vkGetInstanceProcAddr        g_real_gipa = NULL;
/* Real fn pointer captured at resolution time (when caller has a valid
 * instance). Non-global functions resolved with NULL instance return
 * NULL per spec, so we have to intercept the resolution itself. */
static PFN_vkEnumerateDeviceExtensionProperties g_real_enum = NULL;

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

/* Hooked enum: call the real one, append our injected extension. */
static VkResult shim_EnumerateDeviceExtensionProperties(
    VkPhysicalDevice                            physicalDevice,
    const char                                 *pLayerName,
    uint32_t                                   *pPropertyCount,
    VkExtensionProperties                      *pProperties)
{
    open_real_lib_once();
    PFN_vkEnumerateDeviceExtensionProperties real_enum = g_real_enum;
    if (!real_enum) return VK_ERROR_INITIALIZATION_FAILED;

    /* Query mode: caller wants the count. Add 1 to whatever the real
     * wrapper reports. */
    if (!pProperties) {
        VkResult r = real_enum(physicalDevice, pLayerName, pPropertyCount, NULL);
        if (r != VK_SUCCESS && r != VK_INCOMPLETE) return r;
        *pPropertyCount += 1;
        return VK_SUCCESS;
    }

    /* Fill mode: pass through to the real wrapper, then append our
     * extension if there's room. */
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
    /* No room for our injected entry — return INCOMPLETE per spec. */
    *pPropertyCount = real_filled;
    return VK_INCOMPLETE;
}

/* Hooked vkGetInstanceProcAddr: return our shim for the enum call,
 * pass through everything else. */
static PFN_vkVoidFunction shim_GetInstanceProcAddr(
    VkInstance instance, const char *pName)
{
    open_real_lib_once();
    if (!g_real_gipa) return NULL;
    if (pName && !strcmp(pName, "vkEnumerateDeviceExtensionProperties")) {
        /* Capture the real function pointer using the caller's instance
         * (NULL won't work per spec for non-global functions). Stash it
         * for the shim to use later, then return our hook. */
        if (instance != VK_NULL_HANDLE && !g_real_enum) {
            g_real_enum = (PFN_vkEnumerateDeviceExtensionProperties)
                g_real_gipa(instance, pName);
        }
        return (PFN_vkVoidFunction)shim_EnumerateDeviceExtensionProperties;
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
