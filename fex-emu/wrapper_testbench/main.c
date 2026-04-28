/*
 * wrapper_testbench — direct-against-leegao Vulkan harness.
 *
 * Goal: iterate on extension shims in bionic-vulkan-wrapper without
 * launching a game, wine, or DXVK. Sub-second feedback loop.
 *
 * Each test prints one of:
 *   [PASS] name: detail        — feature works as advertised
 *   [FAIL] name: detail        — regression / silent-ignore detected
 *   [SKIP] name: detail        — precondition not met (extension absent)
 *   [INCOMPLETE] name: detail  — shim ran, but covers only a subset of
 *                                spec semantics (called out so it can't
 *                                hide as a PASS later)
 *
 * Exit status = FAIL count. INCOMPLETE is informational and does NOT
 * gate the run, so we can ratchet incrementally — but every INCOMPLETE
 * is a TODO that needs to be either lifted to PASS (real impl) or
 * dropped to FAIL (caller observed in practice).
 *
 * Sentinel-pattern checks: where a wrapper might silently ignore a
 * pNext struct rather than fill it, the test pre-fills the struct with
 * a recognizable non-zero pattern and verifies the pattern was
 * overwritten. This separates "shim ran" from "wrapper saw unknown
 * sType and skipped".
 *
 * Build: build.sh (cross-compile with NDK clang, push to device, run).
 * Runtime deps: libvulkan_wrapper.so on LD_LIBRARY_PATH (typically
 * pushed alongside this binary to /data/local/tmp).
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "oob_probe_spv.h"
#include "oob_image_probe_spv.h"
#include "oob_fetch_probe_spv.h"

/* Counters shared across tests. */
static int g_pass = 0;
static int g_fail = 0;
static int g_inc  = 0;
static int g_skip = 0;

#define PASS(name, fmt, ...)       do { ++g_pass; printf("[PASS] %s: " fmt "\n", name, ##__VA_ARGS__); } while (0)
#define FAIL(name, fmt, ...)       do { ++g_fail; printf("[FAIL] %s: " fmt "\n", name, ##__VA_ARGS__); } while (0)
#define INCOMPLETE(name, fmt, ...) do { ++g_inc;  printf("[INCOMPLETE] %s: " fmt "\n", name, ##__VA_ARGS__); } while (0)
#define SKIP(name, fmt, ...)       do { ++g_skip; printf("[SKIP] %s: " fmt "\n", name, ##__VA_ARGS__); } while (0)

/* Globals filled in by setup_vulkan() and reused across tests. */
static void                            *g_lib = NULL;
static PFN_vkGetInstanceProcAddr        g_vkGetInstanceProcAddr = NULL;
static VkInstance                       g_instance = VK_NULL_HANDLE;
static VkPhysicalDevice                 g_phys = VK_NULL_HANDLE;
static VkDevice                         g_device = VK_NULL_HANDLE;
static uint32_t                         g_gfx_qfam = 0;

/* Resolve an instance-level entrypoint or die. */
#define LOAD_INST(fn) PFN_##fn fn = (PFN_##fn)g_vkGetInstanceProcAddr(g_instance, #fn)
/* Resolve a device-level entrypoint via vkGetDeviceProcAddr. */
#define LOAD_DEV(fn) PFN_##fn fn = (PFN_##fn)pfn_GetDeviceProcAddr(g_device, #fn)

static const char *vkresult_str(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
    default: return "VK_ERROR_<unmapped>";
    }
}

/* Open the wrapper, resolve vkGetInstanceProcAddr, create instance + device.
 * Returns 0 on success, non-zero on failure (reported via FAIL). */
static int setup_vulkan(void) {
    /* Caller can override the wrapper path via env so we can A/B test
     * patched vs vanilla without rebuilding. Default = the imagefs_bionic
     * deployment that wine uses, so testbench results match game results. */
    const char *wrapper_path = getenv("WRAPPER_TESTBENCH_LIB");
    if (!wrapper_path || !*wrapper_path)
        wrapper_path = "libvulkan_wrapper.so";
    g_lib = dlopen(wrapper_path, RTLD_NOW | RTLD_LOCAL);
    if (!g_lib) {
        FAIL("setup", "dlopen(%s): %s", wrapper_path, dlerror());
        return 1;
    }
    printf("        loaded wrapper: %s\n", wrapper_path);
    /* The wrapper is a Khronos ICD; it exports vk_icdGetInstanceProcAddr,
     * not the loader-facing vkGetInstanceProcAddr. Bypass the Vulkan
     * loader entirely and call the ICD directly so we test the wrapper
     * raw. The function signature matches PFN_vkGetInstanceProcAddr. */
    typedef VkResult (*PFN_NegotiateVer)(uint32_t *);
    PFN_NegotiateVer negotiate = (PFN_NegotiateVer)
        dlsym(g_lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
    if (negotiate) {
        uint32_t ver = 6;
        VkResult nr = negotiate(&ver);
        printf("        ICD interface version negotiated: %u (result=%s)\n",
               ver, vkresult_str(nr));
    }
    g_vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)
        dlsym(g_lib, "vk_icdGetInstanceProcAddr");
    if (!g_vkGetInstanceProcAddr) {
        FAIL("setup", "dlsym(vk_icdGetInstanceProcAddr): %s", dlerror());
        return 1;
    }
    PFN_vkCreateInstance vkCreateInstance =
        (PFN_vkCreateInstance)g_vkGetInstanceProcAddr(NULL, "vkCreateInstance");
    if (!vkCreateInstance) {
        FAIL("setup", "vkGetInstanceProcAddr(vkCreateInstance) returned NULL");
        return 1;
    }
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "wrapper_testbench",
        .applicationVersion = 1,
        .pEngineName = "wrapper_testbench",
        .engineVersion = 1,
        .apiVersion = VK_API_VERSION_1_3,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    VkResult r = vkCreateInstance(&ici, NULL, &g_instance);
    if (r != VK_SUCCESS) {
        FAIL("setup", "vkCreateInstance: %s", vkresult_str(r));
        return 1;
    }
    LOAD_INST(vkEnumeratePhysicalDevices);
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_instance, &n, NULL);
    if (n == 0) {
        FAIL("setup", "no physical devices");
        return 1;
    }
    VkPhysicalDevice phys[8];
    if (n > 8) n = 8;
    vkEnumeratePhysicalDevices(g_instance, &n, phys);
    g_phys = phys[0];

    LOAD_INST(vkGetPhysicalDeviceQueueFamilyProperties);
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &nq, NULL);
    VkQueueFamilyProperties qf[16];
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &nq, qf);
    int picked = -1;
    for (uint32_t i = 0; i < nq; i++) {
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            picked = (int)i;
            break;
        }
    }
    if (picked < 0) {
        FAIL("setup", "no graphics queue family");
        return 1;
    }
    g_gfx_qfam = (uint32_t)picked;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = g_gfx_qfam,
        .queueCount = 1,
        .pQueuePriorities = &prio,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
    };
    LOAD_INST(vkCreateDevice);
    r = vkCreateDevice(g_phys, &dci, NULL, &g_device);
    if (r != VK_SUCCESS) {
        FAIL("setup", "vkCreateDevice: %s", vkresult_str(r));
        return 1;
    }
    PASS("setup", "instance + device created (qfam=%u)", g_gfx_qfam);
    return 0;
}

/* ----- tests ----- */

static int pick_memory_type(uint32_t type_bits, VkMemoryPropertyFlags want) {
    LOAD_INST(vkGetPhysicalDeviceMemoryProperties);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if (!(type_bits & (1u << i))) continue;
        if ((mp.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    }
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if (type_bits & (1u << i)) return (int)i;
    }
    return -1;
}

/* Dump apiVersion + driver info + every reported device extension. Useful
 * baseline so future runs can diff against this. */
static void test_enum_extensions(void) {
    LOAD_INST(vkGetPhysicalDeviceProperties);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g_phys, &props);
    printf("        deviceName=\"%s\" apiVersion=%u.%u.%u driverVersion=0x%x\n",
           props.deviceName,
           VK_API_VERSION_MAJOR(props.apiVersion),
           VK_API_VERSION_MINOR(props.apiVersion),
           VK_API_VERSION_PATCH(props.apiVersion),
           props.driverVersion);

    LOAD_INST(vkEnumerateDeviceExtensionProperties);
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(g_phys, NULL, &n, NULL);
    if (n == 0) {
        FAIL("enum_extensions", "0 device extensions reported");
        return;
    }
    VkExtensionProperties *ex = calloc(n, sizeof(*ex));
    vkEnumerateDeviceExtensionProperties(g_phys, NULL, &n, ex);
    PASS("enum_extensions", "%u extensions reported", n);
    for (uint32_t i = 0; i < n; i++) {
        printf("        %-50s spec=%u\n", ex[i].extensionName, ex[i].specVersion);
    }
    free(ex);
}

/* Spec-claim flags for the four extensions DXVK 2.x needs. We don't try
 * to use them yet — just report whether the wrapper claims them. This is
 * the baseline that future shim work moves the needle on. */
static void test_dxvk2_extensions_present(void) {
    static const char *wanted[] = {
        "VK_KHR_maintenance5",
        "VK_EXT_descriptor_buffer",
        "VK_EXT_robustness2",
        "VK_KHR_pipeline_library",
    };
    LOAD_INST(vkEnumerateDeviceExtensionProperties);
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(g_phys, NULL, &n, NULL);
    VkExtensionProperties *ex = calloc(n, sizeof(*ex));
    vkEnumerateDeviceExtensionProperties(g_phys, NULL, &n, ex);
    int present = 0;
    for (size_t w = 0; w < sizeof(wanted) / sizeof(*wanted); w++) {
        int found = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (strcmp(ex[i].extensionName, wanted[w]) == 0) { found = 1; break; }
        }
        printf("        %-32s %s\n", wanted[w], found ? "PRESENT" : "MISSING");
        if (found) present++;
    }
    free(ex);
    if (present == 4)        PASS("dxvk2_extensions_present", "all 4 present (DXVK 2.x ext set complete)");
    else if (present == 0)   FAIL("dxvk2_extensions_present", "0/4 present (current baseline)");
    else                     INCOMPLETE("dxvk2_extensions_present", "%d/4 present — DXVK 2.x needs all 4", present);
}

/* End-to-end smoke: allocate a small buffer, free it. Confirms basic
 * device dispatch works. If the wrapper is broken, this fails before we
 * waste time on extension tests. */
static void test_buffer_alloc_smoke(void) {
    PFN_vkGetDeviceProcAddr pfn_GetDeviceProcAddr =
        (PFN_vkGetDeviceProcAddr)g_vkGetInstanceProcAddr(g_instance, "vkGetDeviceProcAddr");
    LOAD_DEV(vkCreateBuffer);
    LOAD_DEV(vkGetBufferMemoryRequirements);
    LOAD_DEV(vkAllocateMemory);
    LOAD_DEV(vkBindBufferMemory);
    LOAD_DEV(vkFreeMemory);
    LOAD_DEV(vkDestroyBuffer);
    LOAD_INST(vkGetPhysicalDeviceMemoryProperties);

    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 4096,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buf = VK_NULL_HANDLE;
    VkResult r = vkCreateBuffer(g_device, &bci, NULL, &buf);
    if (r != VK_SUCCESS) { FAIL("buffer_alloc_smoke", "vkCreateBuffer: %s", vkresult_str(r)); return; }

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_device, buf, &req);

    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_phys, &mp);
    int mt = -1;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if (req.memoryTypeBits & (1u << i)) { mt = (int)i; break; }
    }
    if (mt < 0) { FAIL("buffer_alloc_smoke", "no memory type for buffer"); vkDestroyBuffer(g_device, buf, NULL); return; }

    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = (uint32_t)mt,
    };
    VkDeviceMemory mem = VK_NULL_HANDLE;
    r = vkAllocateMemory(g_device, &mai, NULL, &mem);
    if (r != VK_SUCCESS) { FAIL("buffer_alloc_smoke", "vkAllocateMemory: %s", vkresult_str(r)); vkDestroyBuffer(g_device, buf, NULL); return; }

    r = vkBindBufferMemory(g_device, buf, mem, 0);
    if (r != VK_SUCCESS) { FAIL("buffer_alloc_smoke", "vkBindBufferMemory: %s", vkresult_str(r)); vkFreeMemory(g_device, mem, NULL); vkDestroyBuffer(g_device, buf, NULL); return; }

    vkFreeMemory(g_device, mem, NULL);
    vkDestroyBuffer(g_device, buf, NULL);
    PASS("buffer_alloc_smoke", "create/alloc/bind/free 4 KiB buffer ok (mtype=%d)", mt);
}

/* If maintenance5 is exposed, try its new entry vkCmdBindIndexBuffer2KHR
 * resolution. We don't actually issue it — just confirm the dispatch
 * table returns a non-NULL pointer. (Once we shim the extension, this
 * starts passing instead of being skipped.) */
static void test_maintenance5_dispatch(void) {
    LOAD_INST(vkEnumerateDeviceExtensionProperties);
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(g_phys, NULL, &n, NULL);
    VkExtensionProperties *ex = calloc(n, sizeof(*ex));
    vkEnumerateDeviceExtensionProperties(g_phys, NULL, &n, ex);
    int has = 0;
    for (uint32_t i = 0; i < n; i++) if (!strcmp(ex[i].extensionName, "VK_KHR_maintenance5")) { has = 1; break; }
    free(ex);
    if (!has) {
        SKIP("maintenance5_dispatch", "extension not present (expected on current wrapper)");
        return;
    }
    PFN_vkGetDeviceProcAddr pfn_GetDeviceProcAddr =
        (PFN_vkGetDeviceProcAddr)g_vkGetInstanceProcAddr(g_instance, "vkGetDeviceProcAddr");
    void *bind2     = (void *)pfn_GetDeviceProcAddr(g_device, "vkCmdBindIndexBuffer2KHR");
    void *gran      = (void *)pfn_GetDeviceProcAddr(g_device, "vkGetRenderingAreaGranularityKHR");
    void *isl2      = (void *)pfn_GetDeviceProcAddr(g_device, "vkGetImageSubresourceLayout2KHR");
    void *dev_isl   = (void *)pfn_GetDeviceProcAddr(g_device, "vkGetDeviceImageSubresourceLayoutKHR");
    int resolved = (bind2 ? 1 : 0) + (gran ? 1 : 0) + (isl2 ? 1 : 0) + (dev_isl ? 1 : 0);
    if (resolved == 4) PASS("maintenance5_dispatch", "all 4 entrypoints resolved (resolution only — see behavior tests below)");
    else FAIL("maintenance5_dispatch", "%d/4 entrypoints resolved (bind2=%p gran=%p isl2=%p dev_isl=%p)",
              resolved, bind2, gran, isl2, dev_isl);
}

/* Behavior test: record vkCmdBindIndexBuffer2KHR into a real command
 * buffer with a real index buffer, end the command buffer, verify no
 * error. Skipped when the extension isn't present. This is the actual
 * "did we move the needle" gauge — the dispatch test only checks that
 * a non-NULL pointer comes back. */
static void test_maintenance5_record(void) {
    LOAD_INST(vkEnumerateDeviceExtensionProperties);
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(g_phys, NULL, &n, NULL);
    VkExtensionProperties *ex = calloc(n, sizeof(*ex));
    vkEnumerateDeviceExtensionProperties(g_phys, NULL, &n, ex);
    int has = 0;
    for (uint32_t i = 0; i < n; i++) if (!strcmp(ex[i].extensionName, "VK_KHR_maintenance5")) { has = 1; break; }
    free(ex);
    if (!has) {
        SKIP("maintenance5_record", "extension not present");
        return;
    }

    PFN_vkGetDeviceProcAddr pfn_GetDeviceProcAddr =
        (PFN_vkGetDeviceProcAddr)g_vkGetInstanceProcAddr(g_instance, "vkGetDeviceProcAddr");
    LOAD_DEV(vkCreateCommandPool);
    LOAD_DEV(vkDestroyCommandPool);
    LOAD_DEV(vkAllocateCommandBuffers);
    LOAD_DEV(vkBeginCommandBuffer);
    LOAD_DEV(vkEndCommandBuffer);
    LOAD_DEV(vkCreateBuffer);
    LOAD_DEV(vkDestroyBuffer);
    LOAD_DEV(vkAllocateMemory);
    LOAD_DEV(vkFreeMemory);
    LOAD_DEV(vkBindBufferMemory);
    LOAD_DEV(vkGetBufferMemoryRequirements);
    LOAD_INST(vkGetPhysicalDeviceMemoryProperties);
    PFN_vkCmdBindIndexBuffer2KHR vkCmdBindIndexBuffer2KHR =
        (PFN_vkCmdBindIndexBuffer2KHR)pfn_GetDeviceProcAddr(g_device, "vkCmdBindIndexBuffer2KHR");
    if (!vkCmdBindIndexBuffer2KHR) {
        FAIL("maintenance5_record", "shim returned NULL for vkCmdBindIndexBuffer2KHR");
        return;
    }

    /* Create an index buffer (small, host-visible). */
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 4096,
        .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buf = VK_NULL_HANDLE;
    VkResult r = vkCreateBuffer(g_device, &bci, NULL, &buf);
    if (r != VK_SUCCESS) { FAIL("maintenance5_record", "vkCreateBuffer: %s", vkresult_str(r)); return; }

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_device, buf, &req);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_phys, &mp);
    int mt = -1;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if (req.memoryTypeBits & (1u << i)) { mt = (int)i; break; }
    if (mt < 0) { FAIL("maintenance5_record", "no memory type"); vkDestroyBuffer(g_device, buf, NULL); return; }

    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = (uint32_t)mt,
    };
    VkDeviceMemory mem = VK_NULL_HANDLE;
    r = vkAllocateMemory(g_device, &mai, NULL, &mem);
    if (r != VK_SUCCESS) { FAIL("maintenance5_record", "vkAllocateMemory: %s", vkresult_str(r)); vkDestroyBuffer(g_device, buf, NULL); return; }
    vkBindBufferMemory(g_device, buf, mem, 0);

    /* Command pool + buffer. */
    VkCommandPoolCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = g_gfx_qfam,
    };
    VkCommandPool pool = VK_NULL_HANDLE;
    r = vkCreateCommandPool(g_device, &cpci, NULL, &pool);
    if (r != VK_SUCCESS) {
        FAIL("maintenance5_record", "vkCreateCommandPool: %s", vkresult_str(r));
        vkFreeMemory(g_device, mem, NULL); vkDestroyBuffer(g_device, buf, NULL); return;
    }
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cb = VK_NULL_HANDLE;
    r = vkAllocateCommandBuffers(g_device, &cbai, &cb);
    if (r != VK_SUCCESS) {
        FAIL("maintenance5_record", "vkAllocateCommandBuffers: %s", vkresult_str(r));
        vkDestroyCommandPool(g_device, pool, NULL);
        vkFreeMemory(g_device, mem, NULL); vkDestroyBuffer(g_device, buf, NULL); return;
    }

    VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    r = vkBeginCommandBuffer(cb, &cbbi);
    if (r != VK_SUCCESS) {
        FAIL("maintenance5_record", "vkBeginCommandBuffer: %s", vkresult_str(r));
        vkDestroyCommandPool(g_device, pool, NULL);
        vkFreeMemory(g_device, mem, NULL); vkDestroyBuffer(g_device, buf, NULL); return;
    }

    /* The actual call under test. Recording errors only surface at End,
     * so we proceed and check there. */
    vkCmdBindIndexBuffer2KHR(cb, buf, 0, VK_WHOLE_SIZE, VK_INDEX_TYPE_UINT16);

    r = vkEndCommandBuffer(cb);
    if (r == VK_SUCCESS) PASS("maintenance5_record", "vkCmdBindIndexBuffer2KHR(VK_WHOLE_SIZE,UINT16) recorded + End ok");
    else                 FAIL("maintenance5_record", "vkEndCommandBuffer: %s", vkresult_str(r));

    vkDestroyCommandPool(g_device, pool, NULL);
    vkFreeMemory(g_device, mem, NULL);
    vkDestroyBuffer(g_device, buf, NULL);
}

/* --- Per-entrypoint behavior tests (sharper than maintenance5_dispatch) --- */

/* Helper: returns 1 if the named device extension is in the ext list. */
static int device_ext_present(const char *want) {
    PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties =
        (PFN_vkEnumerateDeviceExtensionProperties)
        g_vkGetInstanceProcAddr(g_instance, "vkEnumerateDeviceExtensionProperties");
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(g_phys, NULL, &n, NULL);
    VkExtensionProperties *ex = calloc(n, sizeof(*ex));
    vkEnumerateDeviceExtensionProperties(g_phys, NULL, &n, ex);
    int has = 0;
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(ex[i].extensionName, want)) { has = 1; break; }
    free(ex);
    return has;
}

/* Helper: returns 1 if VK_KHR_maintenance5 is in the device ext list. */
static int maintenance5_present(void) {
    PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties =
        (PFN_vkEnumerateDeviceExtensionProperties)
        g_vkGetInstanceProcAddr(g_instance, "vkEnumerateDeviceExtensionProperties");
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(g_phys, NULL, &n, NULL);
    VkExtensionProperties *ex = calloc(n, sizeof(*ex));
    vkEnumerateDeviceExtensionProperties(g_phys, NULL, &n, ex);
    int has = 0;
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(ex[i].extensionName, "VK_KHR_maintenance5")) { has = 1; break; }
    free(ex);
    return has;
}

/* maintenance5's defining behavioral delta for vkCmdBindIndexBuffer2KHR
 * is per-bind size bounding (out-of-range index reads return zero
 * instead of UB). Our shim drops `size` and forwards to v1, which has
 * no size bound. Recording succeeds either way; the bound is only
 * observable at execution. So this test records with an explicit
 * non-WHOLE_SIZE bound and reports INCOMPLETE — flagging that the shim
 * does not honor the bound, even though "the call recorded fine". */
static void test_maintenance5_bounded_size(void) {
    if (!maintenance5_present()) { SKIP("maintenance5_bounded_size", "extension not present"); return; }
    PFN_vkGetDeviceProcAddr pfn_GetDeviceProcAddr =
        (PFN_vkGetDeviceProcAddr)g_vkGetInstanceProcAddr(g_instance, "vkGetDeviceProcAddr");
    LOAD_DEV(vkCreateBuffer);
    LOAD_DEV(vkDestroyBuffer);
    LOAD_DEV(vkAllocateMemory);
    LOAD_DEV(vkFreeMemory);
    LOAD_DEV(vkBindBufferMemory);
    LOAD_DEV(vkGetBufferMemoryRequirements);
    LOAD_DEV(vkCreateCommandPool);
    LOAD_DEV(vkDestroyCommandPool);
    LOAD_DEV(vkAllocateCommandBuffers);
    LOAD_DEV(vkBeginCommandBuffer);
    LOAD_DEV(vkEndCommandBuffer);
    LOAD_INST(vkGetPhysicalDeviceMemoryProperties);
    PFN_vkCmdBindIndexBuffer2KHR vkCmdBindIndexBuffer2KHR =
        (PFN_vkCmdBindIndexBuffer2KHR)pfn_GetDeviceProcAddr(g_device, "vkCmdBindIndexBuffer2KHR");
    if (!vkCmdBindIndexBuffer2KHR) { FAIL("maintenance5_bounded_size", "shim returned NULL"); return; }

    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 4096, .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer buf = VK_NULL_HANDLE;
    if (vkCreateBuffer(g_device, &bci, NULL, &buf) != VK_SUCCESS) {
        FAIL("maintenance5_bounded_size", "vkCreateBuffer"); return;
    }
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(g_device, buf, &req);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(g_phys, &mp);
    int mt = -1;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if (req.memoryTypeBits & (1u << i)) { mt = (int)i; break; }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = (uint32_t)mt };
    VkDeviceMemory mem = VK_NULL_HANDLE;
    vkAllocateMemory(g_device, &mai, NULL, &mem);
    vkBindBufferMemory(g_device, buf, mem, 0);

    VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = g_gfx_qfam };
    VkCommandPool pool; vkCreateCommandPool(g_device, &cpci, NULL, &pool);
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb; vkAllocateCommandBuffers(g_device, &cbai, &cb);
    VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cb, &cbbi);
    /* Bound size: half the buffer. Shim drops this, falls through to v1
     * which uses rest-of-buffer. Recording succeeds either way. */
    vkCmdBindIndexBuffer2KHR(cb, buf, 0, 2048, VK_INDEX_TYPE_UINT16);
    VkResult r = vkEndCommandBuffer(cb);
    if (r != VK_SUCCESS) FAIL("maintenance5_bounded_size", "vkEndCommandBuffer: %s", vkresult_str(r));
    else INCOMPLETE("maintenance5_bounded_size",
                    "size=2048 recorded but shim drops the bound; v1 fallback uses rest-of-buffer (no OOB-read protection)");
    vkDestroyCommandPool(g_device, pool, NULL);
    vkFreeMemory(g_device, mem, NULL);
    vkDestroyBuffer(g_device, buf, NULL);
}

/* maintenance5 implicitly enables VK_INDEX_TYPE_UINT8_KHR. The wrapper
 * also exposes VK_EXT_index_type_uint8 directly (per the ext-list dump),
 * so the v1 fallback should accept UINT8 indices. PASS = recorded + End
 * succeeds; FAIL = wrapper rejected at End. */
static void test_maintenance5_index_uint8(void) {
    if (!maintenance5_present()) { SKIP("maintenance5_index_uint8", "extension not present"); return; }
    PFN_vkGetDeviceProcAddr pfn_GetDeviceProcAddr =
        (PFN_vkGetDeviceProcAddr)g_vkGetInstanceProcAddr(g_instance, "vkGetDeviceProcAddr");
    LOAD_DEV(vkCreateBuffer);
    LOAD_DEV(vkDestroyBuffer);
    LOAD_DEV(vkAllocateMemory);
    LOAD_DEV(vkFreeMemory);
    LOAD_DEV(vkBindBufferMemory);
    LOAD_DEV(vkGetBufferMemoryRequirements);
    LOAD_DEV(vkCreateCommandPool);
    LOAD_DEV(vkDestroyCommandPool);
    LOAD_DEV(vkAllocateCommandBuffers);
    LOAD_DEV(vkBeginCommandBuffer);
    LOAD_DEV(vkEndCommandBuffer);
    LOAD_INST(vkGetPhysicalDeviceMemoryProperties);
    PFN_vkCmdBindIndexBuffer2KHR vkCmdBindIndexBuffer2KHR =
        (PFN_vkCmdBindIndexBuffer2KHR)pfn_GetDeviceProcAddr(g_device, "vkCmdBindIndexBuffer2KHR");
    if (!vkCmdBindIndexBuffer2KHR) { FAIL("maintenance5_index_uint8", "shim returned NULL"); return; }

    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 4096, .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer buf; vkCreateBuffer(g_device, &bci, NULL, &buf);
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(g_device, buf, &req);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(g_phys, &mp);
    int mt = -1;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if (req.memoryTypeBits & (1u << i)) { mt = (int)i; break; }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = (uint32_t)mt };
    VkDeviceMemory mem; vkAllocateMemory(g_device, &mai, NULL, &mem);
    vkBindBufferMemory(g_device, buf, mem, 0);

    VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = g_gfx_qfam };
    VkCommandPool pool; vkCreateCommandPool(g_device, &cpci, NULL, &pool);
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb; vkAllocateCommandBuffers(g_device, &cbai, &cb);
    VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cb, &cbbi);
    vkCmdBindIndexBuffer2KHR(cb, buf, 0, VK_WHOLE_SIZE, VK_INDEX_TYPE_UINT8_EXT);
    VkResult r = vkEndCommandBuffer(cb);
    if (r == VK_SUCCESS) PASS("maintenance5_index_uint8", "UINT8 index recorded + End ok");
    else                 FAIL("maintenance5_index_uint8", "vkEndCommandBuffer: %s (UINT8 not accepted)", vkresult_str(r));
    vkDestroyCommandPool(g_device, pool, NULL);
    vkFreeMemory(g_device, mem, NULL);
    vkDestroyBuffer(g_device, buf, NULL);
}

/* Sentinel pattern: pre-fill the granularity output with a recognizable
 * non-zero value. After the shim runs, the output is whatever the shim
 * wrote. Our shim hardcodes {1,1}. Mark INCOMPLETE because that is a
 * conservative minimum, not the device's actual tile granularity. */
static void test_maintenance5_granularity(void) {
    if (!maintenance5_present()) { SKIP("maintenance5_granularity", "extension not present"); return; }
    PFN_vkGetDeviceProcAddr pfn_GetDeviceProcAddr =
        (PFN_vkGetDeviceProcAddr)g_vkGetInstanceProcAddr(g_instance, "vkGetDeviceProcAddr");
    PFN_vkGetRenderingAreaGranularityKHR fn =
        (PFN_vkGetRenderingAreaGranularityKHR)pfn_GetDeviceProcAddr(g_device, "vkGetRenderingAreaGranularityKHR");
    if (!fn) { FAIL("maintenance5_granularity", "shim returned NULL"); return; }
    VkRenderingAreaInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_AREA_INFO_KHR,
        .colorAttachmentCount = 0,
        .depthAttachmentFormat = VK_FORMAT_UNDEFINED,
        .stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
    };
    VkExtent2D gran = { .width = 0xDEADBEEF, .height = 0xCAFEBABE };
    fn(g_device, &info, &gran);
    if (gran.width == 0xDEADBEEF || gran.height == 0xCAFEBABE) {
        FAIL("maintenance5_granularity", "output not written (sentinel persisted: w=0x%x h=0x%x)", gran.width, gran.height);
        return;
    }
    if (gran.width == 1 && gran.height == 1)
        INCOMPLETE("maintenance5_granularity", "shim returns {1,1} (always-safe minimum, not real tile granularity)");
    else
        PASS("maintenance5_granularity", "device-aware granularity {%u,%u}", gran.width, gran.height);
}

/* Create a real linear image, query its v1 layout directly via
 * vkGetImageSubresourceLayout, then call the v2 shim and compare
 * subresource layouts byte-for-byte. PASS = layouts agree; FAIL =
 * shim returns different / zero layout. */
static void test_maintenance5_isl2_agreement(void) {
    if (!maintenance5_present()) { SKIP("maintenance5_isl2_agreement", "extension not present"); return; }
    PFN_vkGetDeviceProcAddr pfn_GetDeviceProcAddr =
        (PFN_vkGetDeviceProcAddr)g_vkGetInstanceProcAddr(g_instance, "vkGetDeviceProcAddr");
    LOAD_DEV(vkCreateImage);
    LOAD_DEV(vkDestroyImage);
    LOAD_DEV(vkGetImageSubresourceLayout);
    PFN_vkGetImageSubresourceLayout2KHR fn =
        (PFN_vkGetImageSubresourceLayout2KHR)pfn_GetDeviceProcAddr(g_device, "vkGetImageSubresourceLayout2KHR");
    if (!fn) { FAIL("maintenance5_isl2_agreement", "shim returned NULL"); return; }

    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { 16, 16, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage img;
    VkResult r = vkCreateImage(g_device, &ici, NULL, &img);
    if (r != VK_SUCCESS) {
        SKIP("maintenance5_isl2_agreement", "vkCreateImage(LINEAR R8G8B8A8) not supported: %s", vkresult_str(r));
        return;
    }
    VkImageSubresource sr = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .arrayLayer = 0 };
    VkSubresourceLayout v1 = {0};
    vkGetImageSubresourceLayout(g_device, img, &sr, &v1);

    VkImageSubresource2KHR sr2 = { .sType = VK_STRUCTURE_TYPE_IMAGE_SUBRESOURCE_2_KHR, .imageSubresource = sr };
    VkSubresourceLayout2KHR v2 = { .sType = VK_STRUCTURE_TYPE_SUBRESOURCE_LAYOUT_2_KHR };
    /* sentinel — if shim does nothing, this remains and we'll detect it */
    v2.subresourceLayout.offset    = 0xDEADBEEFDEADBEEFull;
    v2.subresourceLayout.size      = 0xDEADBEEFDEADBEEFull;
    v2.subresourceLayout.rowPitch  = 0xDEADBEEFDEADBEEFull;
    fn(g_device, img, &sr2, &v2);
    int sentinel_persisted = (v2.subresourceLayout.offset    == 0xDEADBEEFDEADBEEFull) ||
                             (v2.subresourceLayout.rowPitch  == 0xDEADBEEFDEADBEEFull);
    if (sentinel_persisted) FAIL("maintenance5_isl2_agreement", "shim did not write subresource layout");
    else if (memcmp(&v1, &v2.subresourceLayout, sizeof(VkSubresourceLayout)) == 0)
        PASS("maintenance5_isl2_agreement", "v2 layout matches v1 (size=%llu rowPitch=%llu)",
             (unsigned long long)v1.size, (unsigned long long)v1.rowPitch);
    else FAIL("maintenance5_isl2_agreement",
              "v1 vs v2 mismatch: v1 size=%llu rp=%llu off=%llu | v2 size=%llu rp=%llu off=%llu",
              (unsigned long long)v1.size, (unsigned long long)v1.rowPitch, (unsigned long long)v1.offset,
              (unsigned long long)v2.subresourceLayout.size, (unsigned long long)v2.subresourceLayout.rowPitch,
              (unsigned long long)v2.subresourceLayout.offset);
    vkDestroyImage(g_device, img, NULL);
}

/* vkGetDeviceImageSubresourceLayoutKHR is a zero-stub in the shim
 * (no transient-image emulation yet). Mark INCOMPLETE so it can't be
 * confused with a real implementation. */
static void test_maintenance5_dev_isl_stub(void) {
    if (!maintenance5_present()) { SKIP("maintenance5_dev_isl_stub", "extension not present"); return; }
    PFN_vkGetDeviceProcAddr pfn_GetDeviceProcAddr =
        (PFN_vkGetDeviceProcAddr)g_vkGetInstanceProcAddr(g_instance, "vkGetDeviceProcAddr");
    PFN_vkGetDeviceImageSubresourceLayoutKHR fn =
        (PFN_vkGetDeviceImageSubresourceLayoutKHR)pfn_GetDeviceProcAddr(g_device, "vkGetDeviceImageSubresourceLayoutKHR");
    if (!fn) { FAIL("maintenance5_dev_isl_stub", "shim returned NULL"); return; }

    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { 16, 16, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImageSubresource2KHR sr2 = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_SUBRESOURCE_2_KHR,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 },
    };
    VkDeviceImageSubresourceInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_SUBRESOURCE_INFO_KHR,
        .pCreateInfo = &ici,
        .pSubresource = &sr2,
    };
    VkSubresourceLayout2KHR out = { .sType = VK_STRUCTURE_TYPE_SUBRESOURCE_LAYOUT_2_KHR };
    out.subresourceLayout.size     = 0xDEADBEEFDEADBEEFull;
    out.subresourceLayout.rowPitch = 0xDEADBEEFDEADBEEFull;
    fn(g_device, &info, &out);
    if (out.subresourceLayout.size == 0 && out.subresourceLayout.rowPitch == 0)
        INCOMPLETE("maintenance5_dev_isl_stub", "shim is a zero-stub (no transient-image emulation)");
    else if (out.subresourceLayout.size == 0xDEADBEEFDEADBEEFull)
        FAIL("maintenance5_dev_isl_stub", "shim did not write output");
    else
        PASS("maintenance5_dev_isl_stub", "shim wrote real layout (size=%llu rowPitch=%llu)",
             (unsigned long long)out.subresourceLayout.size,
             (unsigned long long)out.subresourceLayout.rowPitch);
}

/* Build a Features2 query with a maintenance5 features pNext, sentinel
 * pre-fill the maintenance5 field to detect "wrapper saw unknown sType
 * and skipped". After the shim runs, maintenance5 must equal VK_TRUE. */
static void test_maintenance5_features2(void) {
    if (!maintenance5_present()) { SKIP("maintenance5_features2", "extension not present"); return; }
    PFN_vkGetPhysicalDeviceFeatures2 fn =
        (PFN_vkGetPhysicalDeviceFeatures2)g_vkGetInstanceProcAddr(g_instance, "vkGetPhysicalDeviceFeatures2");
    if (!fn) { FAIL("maintenance5_features2", "vkGetPhysicalDeviceFeatures2 not exposed"); return; }

    VkPhysicalDeviceMaintenance5FeaturesKHR m5 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR,
        .pNext = NULL,
        .maintenance5 = 0xDEADBEEF, /* sentinel */
    };
    VkPhysicalDeviceFeatures2 f2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &m5,
    };
    fn(g_phys, &f2);
    if (m5.maintenance5 == 0xDEADBEEF)
        FAIL("maintenance5_features2", "wrapper ignored maintenance5 features struct (sentinel persisted)");
    else if (m5.maintenance5 == VK_TRUE)
        PASS("maintenance5_features2", "maintenance5 = VK_TRUE");
    else
        FAIL("maintenance5_features2", "maintenance5 = %u (expected VK_TRUE=1)", m5.maintenance5);
}

/* Same pattern for Properties2: build a chain with maintenance5 props,
 * sentinel pre-fill, verify the shim wrote definite values. The shim
 * writes VK_FALSE for every prop, which is fine — we just need to see
 * the sentinels were overwritten. */
static void test_maintenance5_properties2(void) {
    if (!maintenance5_present()) { SKIP("maintenance5_properties2", "extension not present"); return; }
    PFN_vkGetPhysicalDeviceProperties2 fn =
        (PFN_vkGetPhysicalDeviceProperties2)g_vkGetInstanceProcAddr(g_instance, "vkGetPhysicalDeviceProperties2");
    if (!fn) { FAIL("maintenance5_properties2", "vkGetPhysicalDeviceProperties2 not exposed"); return; }

    VkPhysicalDeviceMaintenance5PropertiesKHR m5p = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_PROPERTIES_KHR,
        .pNext = NULL,
        .earlyFragmentMultisampleCoverageAfterSampleCounting = 0xDEADBEEF,
        .earlyFragmentSampleMaskTestBeforeSampleCounting     = 0xDEADBEEF,
        .depthStencilSwizzleOneSupport                       = 0xDEADBEEF,
        .polygonModePointSize                                = 0xDEADBEEF,
        .nonStrictSinglePixelWideLinesUseParallelogram       = 0xDEADBEEF,
        .nonStrictWideLinesUseParallelogram                  = 0xDEADBEEF,
    };
    VkPhysicalDeviceProperties2 p2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &m5p,
    };
    fn(g_phys, &p2);
    int any_sentinel =
        m5p.earlyFragmentMultisampleCoverageAfterSampleCounting == 0xDEADBEEF ||
        m5p.earlyFragmentSampleMaskTestBeforeSampleCounting     == 0xDEADBEEF ||
        m5p.depthStencilSwizzleOneSupport                       == 0xDEADBEEF ||
        m5p.polygonModePointSize                                == 0xDEADBEEF ||
        m5p.nonStrictSinglePixelWideLinesUseParallelogram       == 0xDEADBEEF ||
        m5p.nonStrictWideLinesUseParallelogram                  == 0xDEADBEEF;
    if (any_sentinel)
        FAIL("maintenance5_properties2", "shim did not fill all properties (some sentinels persist)");
    else
        PASS("maintenance5_properties2", "all 6 maintenance5 properties written");
}

/* DXVK 2.7.1 gates adapter acceptance on VkPhysicalDeviceRobustness2FeaturesEXT
 * with all three bools (robustBufferAccess2 / robustImageAccess2 /
 * nullDescriptor) set. Sentinel-pattern check that the shim wrote them
 * all to VK_TRUE. */
static void test_robustness2_features2(void) {
    /* Robustness2 is injected as an extension by the shim; in PASS A
     * (vanilla wrapper) the extension isn't present and there's nothing
     * to test. */
    PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties =
        (PFN_vkEnumerateDeviceExtensionProperties)
        g_vkGetInstanceProcAddr(g_instance, "vkEnumerateDeviceExtensionProperties");
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(g_phys, NULL, &n, NULL);
    VkExtensionProperties *ex = calloc(n, sizeof(*ex));
    vkEnumerateDeviceExtensionProperties(g_phys, NULL, &n, ex);
    int has = 0;
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(ex[i].extensionName, "VK_EXT_robustness2")) { has = 1; break; }
    free(ex);
    if (!has) { SKIP("robustness2_features2", "VK_EXT_robustness2 not present"); return; }

    PFN_vkGetPhysicalDeviceFeatures2 fn =
        (PFN_vkGetPhysicalDeviceFeatures2)g_vkGetInstanceProcAddr(g_instance, "vkGetPhysicalDeviceFeatures2");
    if (!fn) { FAIL("robustness2_features2", "vkGetPhysicalDeviceFeatures2 not exposed"); return; }

    VkPhysicalDeviceRobustness2FeaturesEXT r2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
        .pNext = NULL,
        .robustBufferAccess2 = 0xDEADBEEF,
        .robustImageAccess2  = 0xDEADBEEF,
        .nullDescriptor      = 0xDEADBEEF,
    };
    VkPhysicalDeviceFeatures2 f2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &r2,
    };
    fn(g_phys, &f2);
    if (r2.robustBufferAccess2 == 0xDEADBEEF ||
        r2.robustImageAccess2  == 0xDEADBEEF ||
        r2.nullDescriptor      == 0xDEADBEEF) {
        FAIL("robustness2_features2", "shim did not fill robustness2 features (sentinels persisted)");
        return;
    }
    if (r2.robustBufferAccess2 == VK_TRUE &&
        r2.robustImageAccess2  == VK_TRUE &&
        r2.nullDescriptor      == VK_TRUE)
        PASS("robustness2_features2", "all 3 bools = VK_TRUE (DXVK 2.7.1 adapter gate satisfied)");
    else
        FAIL("robustness2_features2", "values bufAcc=%u imgAcc=%u nullDesc=%u (expected all VK_TRUE)",
             r2.robustBufferAccess2, r2.robustImageAccess2, r2.nullDescriptor);
}

/* Robustness2 properties: shim writes typical 4-byte alignments.
 * Sentinel-check that both fields were written. */
static void test_robustness2_properties2(void) {
    PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties =
        (PFN_vkEnumerateDeviceExtensionProperties)
        g_vkGetInstanceProcAddr(g_instance, "vkEnumerateDeviceExtensionProperties");
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(g_phys, NULL, &n, NULL);
    VkExtensionProperties *ex = calloc(n, sizeof(*ex));
    vkEnumerateDeviceExtensionProperties(g_phys, NULL, &n, ex);
    int has = 0;
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(ex[i].extensionName, "VK_EXT_robustness2")) { has = 1; break; }
    free(ex);
    if (!has) { SKIP("robustness2_properties2", "VK_EXT_robustness2 not present"); return; }

    PFN_vkGetPhysicalDeviceProperties2 fn =
        (PFN_vkGetPhysicalDeviceProperties2)g_vkGetInstanceProcAddr(g_instance, "vkGetPhysicalDeviceProperties2");
    if (!fn) { FAIL("robustness2_properties2", "vkGetPhysicalDeviceProperties2 not exposed"); return; }

    VkPhysicalDeviceRobustness2PropertiesEXT r2p = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_PROPERTIES_EXT,
        .pNext = NULL,
        .robustStorageBufferAccessSizeAlignment = 0xDEADBEEFDEADBEEFull,
        .robustUniformBufferAccessSizeAlignment = 0xDEADBEEFDEADBEEFull,
    };
    VkPhysicalDeviceProperties2 p2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &r2p,
    };
    fn(g_phys, &p2);
    if (r2p.robustStorageBufferAccessSizeAlignment == 0xDEADBEEFDEADBEEFull ||
        r2p.robustUniformBufferAccessSizeAlignment == 0xDEADBEEFDEADBEEFull)
        FAIL("robustness2_properties2", "shim did not fill alignments (sentinels persisted)");
    else
        PASS("robustness2_properties2", "alignments storageBuf=%llu uniformBuf=%llu",
             (unsigned long long)r2p.robustStorageBufferAccessSizeAlignment,
             (unsigned long long)r2p.robustUniformBufferAccessSizeAlignment);
}

/* nullDescriptor real-impl tests. The shim substitutes VK_NULL_HANDLE
 * descriptor handles with per-device standin resources at
 * vkUpdateDescriptorSets / template-update time. We dlsym
 * shim_m5_null_subst_count out of the .so to prove a substitution
 * actually fired (the wrapper might silently accept null handles in
 * some configurations, which is the lax case we don't want passing
 * silently). */

/* Helper: build a tiny single-binding descriptor pool/layout/set for
 * the given descriptor type. Returns 0 on failure. Caller must destroy
 * pool + layout. */
static int build_single_binding_set(VkDescriptorType type, VkShaderStageFlags stages,
                                    VkDescriptorPool *outPool,
                                    VkDescriptorSetLayout *outLayout,
                                    VkDescriptorSet *outSet) {
    PFN_vkGetDeviceProcAddr pfn_GetDeviceProcAddr =
        (PFN_vkGetDeviceProcAddr)g_vkGetInstanceProcAddr(g_instance, "vkGetDeviceProcAddr");
    LOAD_DEV(vkCreateDescriptorPool);
    LOAD_DEV(vkCreateDescriptorSetLayout);
    LOAD_DEV(vkAllocateDescriptorSets);
    VkDescriptorPoolSize ps = { .type = type, .descriptorCount = 1 };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps,
    };
    if (vkCreateDescriptorPool(g_device, &dpci, NULL, outPool) != VK_SUCCESS) return 0;
    VkDescriptorSetLayoutBinding b = {
        .binding = 0, .descriptorType = type, .descriptorCount = 1, .stageFlags = stages,
    };
    VkDescriptorSetLayoutCreateInfo dsli = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &b,
    };
    if (vkCreateDescriptorSetLayout(g_device, &dsli, NULL, outLayout) != VK_SUCCESS) return 0;
    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = *outPool, .descriptorSetCount = 1, .pSetLayouts = outLayout,
    };
    return vkAllocateDescriptorSets(g_device, &dsai, outSet) == VK_SUCCESS;
}

static void cleanup_single_binding_set(VkDescriptorPool pool, VkDescriptorSetLayout layout) {
    PFN_vkGetDeviceProcAddr pfn_GetDeviceProcAddr =
        (PFN_vkGetDeviceProcAddr)g_vkGetInstanceProcAddr(g_instance, "vkGetDeviceProcAddr");
    LOAD_DEV(vkDestroyDescriptorPool);
    LOAD_DEV(vkDestroyDescriptorSetLayout);
    if (pool   != VK_NULL_HANDLE) vkDestroyDescriptorPool(g_device, pool, NULL);
    if (layout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(g_device, layout, NULL);
}

/* Test: write a UNIFORM_BUFFER descriptor with VkBuffer = VK_NULL_HANDLE.
 * Counter must advance. */
static void test_null_subst_uniform_buffer(void) {
    if (!device_ext_present("VK_EXT_robustness2")) {
        SKIP("null_subst_uniform_buffer", "VK_EXT_robustness2 not present"); return;
    }
    PFN_vkGetDeviceProcAddr pfn_GetDeviceProcAddr =
        (PFN_vkGetDeviceProcAddr)g_vkGetInstanceProcAddr(g_instance, "vkGetDeviceProcAddr");
    LOAD_DEV(vkUpdateDescriptorSets);

    volatile int *cnt = (volatile int *)dlsym(g_lib, "shim_m5_null_subst_count");
    if (!cnt) { INCOMPLETE("null_subst_uniform_buffer", "shim counter symbol missing — cannot prove subst fired"); return; }

    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!build_single_binding_set(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                  VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                  &pool, &layout, &set)) {
        FAIL("null_subst_uniform_buffer", "descriptor set setup failed");
        cleanup_single_binding_set(pool, layout); return;
    }

    int before = *cnt;
    VkDescriptorBufferInfo bi = { .buffer = VK_NULL_HANDLE, .offset = 0, .range = VK_WHOLE_SIZE };
    VkWriteDescriptorSet w = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set, .dstBinding = 0, .dstArrayElement = 0,
        .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .pBufferInfo = &bi,
    };
    vkUpdateDescriptorSets(g_device, 1, &w, 0, NULL);
    int after = *cnt;
    if (after > before) PASS("null_subst_uniform_buffer", "shim substituted standin (counter %d→%d)", before, after);
    else FAIL("null_subst_uniform_buffer", "counter did not advance (stuck at %d) — substitution did not fire", before);

    cleanup_single_binding_set(pool, layout);
}

/* Test: write a SAMPLED_IMAGE descriptor with VkImageView = VK_NULL_HANDLE. */
static void test_null_subst_sampled_image(void) {
    if (!device_ext_present("VK_EXT_robustness2")) {
        SKIP("null_subst_sampled_image", "VK_EXT_robustness2 not present"); return;
    }
    PFN_vkGetDeviceProcAddr pfn_GetDeviceProcAddr =
        (PFN_vkGetDeviceProcAddr)g_vkGetInstanceProcAddr(g_instance, "vkGetDeviceProcAddr");
    LOAD_DEV(vkUpdateDescriptorSets);
    volatile int *cnt = (volatile int *)dlsym(g_lib, "shim_m5_null_subst_count");
    if (!cnt) { INCOMPLETE("null_subst_sampled_image", "shim counter symbol missing"); return; }

    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!build_single_binding_set(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                  VK_SHADER_STAGE_FRAGMENT_BIT,
                                  &pool, &layout, &set)) {
        FAIL("null_subst_sampled_image", "descriptor set setup failed");
        cleanup_single_binding_set(pool, layout); return;
    }
    int before = *cnt;
    VkDescriptorImageInfo ii = {
        .sampler = VK_NULL_HANDLE,
        .imageView = VK_NULL_HANDLE,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet w = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set, .dstBinding = 0, .dstArrayElement = 0,
        .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        .pImageInfo = &ii,
    };
    vkUpdateDescriptorSets(g_device, 1, &w, 0, NULL);
    int after = *cnt;
    if (after > before) PASS("null_subst_sampled_image", "shim substituted standin imageView (counter %d→%d)", before, after);
    else FAIL("null_subst_sampled_image", "counter did not advance (stuck at %d)", before);

    cleanup_single_binding_set(pool, layout);
}

/* Test: storage descriptor type — substitution still fires, but the
 * shim's write-discard semantics are not spec-correct (shared standin).
 * Test reports both counters: subst_count must advance (substitution
 * fired) AND null_storage_subst_count must advance (it was a storage
 * type). Result is reported as INCOMPLETE because the lie persists in
 * the write-discard contract. */
static void test_null_subst_storage_buffer_incomplete(void) {
    if (!device_ext_present("VK_EXT_robustness2")) {
        SKIP("null_subst_storage_buffer_incomplete", "VK_EXT_robustness2 not present"); return;
    }
    PFN_vkGetDeviceProcAddr pfn_GetDeviceProcAddr =
        (PFN_vkGetDeviceProcAddr)g_vkGetInstanceProcAddr(g_instance, "vkGetDeviceProcAddr");
    LOAD_DEV(vkUpdateDescriptorSets);
    volatile int *cnt   = (volatile int *)dlsym(g_lib, "shim_m5_null_subst_count");
    volatile int *cnt_s = (volatile int *)dlsym(g_lib, "shim_m5_null_storage_subst_count");
    if (!cnt || !cnt_s) { INCOMPLETE("null_subst_storage_buffer_incomplete", "shim counter symbols missing"); return; }

    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!build_single_binding_set(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  VK_SHADER_STAGE_COMPUTE_BIT,
                                  &pool, &layout, &set)) {
        FAIL("null_subst_storage_buffer_incomplete", "descriptor set setup failed");
        cleanup_single_binding_set(pool, layout); return;
    }
    int before   = *cnt;
    int before_s = *cnt_s;
    VkDescriptorBufferInfo bi = { .buffer = VK_NULL_HANDLE, .offset = 0, .range = VK_WHOLE_SIZE };
    VkWriteDescriptorSet w = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set, .dstBinding = 0, .dstArrayElement = 0,
        .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &bi,
    };
    vkUpdateDescriptorSets(g_device, 1, &w, 0, NULL);
    int after   = *cnt;
    int after_s = *cnt_s;
    if (after > before && after_s > before_s)
        INCOMPLETE("null_subst_storage_buffer_incomplete",
                   "subst fired (subst %d→%d, storage %d→%d) but write-discard semantics share the standin (needs SPIR-V instrumentation)",
                   before, after, before_s, after_s);
    else
        FAIL("null_subst_storage_buffer_incomplete",
             "counters did not advance (subst %d→%d, storage %d→%d)", before, after, before_s, after_s);

    cleanup_single_binding_set(pool, layout);
}

/* Test: descriptor update template with a null descriptor.
 * vkCreateDescriptorUpdateTemplate must be hooked, the entries must be
 * mirrored, and vkUpdateDescriptorSetWithTemplate must walk the data
 * blob and substitute. Counter must advance. */
static void test_null_subst_via_template(void) {
    if (!device_ext_present("VK_EXT_robustness2")) {
        SKIP("null_subst_via_template", "VK_EXT_robustness2 not present"); return;
    }
    /* Templates are core 1.1 — no separate extension gate. */
    PFN_vkGetDeviceProcAddr pfn_GetDeviceProcAddr =
        (PFN_vkGetDeviceProcAddr)g_vkGetInstanceProcAddr(g_instance, "vkGetDeviceProcAddr");
    LOAD_DEV(vkCreateDescriptorUpdateTemplate);
    LOAD_DEV(vkDestroyDescriptorUpdateTemplate);
    LOAD_DEV(vkUpdateDescriptorSetWithTemplate);
    volatile int *cnt = (volatile int *)dlsym(g_lib, "shim_m5_null_subst_count");
    if (!cnt) { INCOMPLETE("null_subst_via_template", "shim counter symbol missing"); return; }

    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!build_single_binding_set(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                  VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                  &pool, &layout, &set)) {
        FAIL("null_subst_via_template", "descriptor set setup failed");
        cleanup_single_binding_set(pool, layout); return;
    }
    /* Single-entry template: at offset 0, one VkDescriptorBufferInfo. */
    VkDescriptorUpdateTemplateEntry tpl_entry = {
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .offset = 0,
        .stride = sizeof(VkDescriptorBufferInfo),
    };
    VkDescriptorUpdateTemplateCreateInfo tci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,
        .descriptorUpdateEntryCount = 1,
        .pDescriptorUpdateEntries = &tpl_entry,
        .templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET,
        .descriptorSetLayout = layout,
    };
    VkDescriptorUpdateTemplate tpl = VK_NULL_HANDLE;
    VkResult r = vkCreateDescriptorUpdateTemplate(g_device, &tci, NULL, &tpl);
    if (r != VK_SUCCESS) {
        FAIL("null_subst_via_template", "vkCreateDescriptorUpdateTemplate: %s", vkresult_str(r));
        cleanup_single_binding_set(pool, layout); return;
    }
    int before = *cnt;
    VkDescriptorBufferInfo bi = { .buffer = VK_NULL_HANDLE, .offset = 0, .range = VK_WHOLE_SIZE };
    vkUpdateDescriptorSetWithTemplate(g_device, set, tpl, &bi);
    int after = *cnt;
    if (after > before) PASS("null_subst_via_template", "template-mediated subst fired (counter %d→%d)", before, after);
    else FAIL("null_subst_via_template", "counter did not advance (stuck at %d)", before);

    vkDestroyDescriptorUpdateTemplate(g_device, tpl, NULL);
    cleanup_single_binding_set(pool, layout);
}

/* Verifies that the shim's SPIR-V pass correctly identified the OOB
 * probe shader's SSBO loads. The probe has 1 OpLoad, and that load is
 * through a StorageBuffer descriptor. After running the probe through
 * the hook, the pass's descriptor-load counter must have advanced by
 * at least 1. */
static void test_spirv_pass_identifies_descriptor_loads(void) {
    PFN_vkGetDeviceProcAddr pfn_GetDeviceProcAddr =
        (PFN_vkGetDeviceProcAddr)g_vkGetInstanceProcAddr(g_instance, "vkGetDeviceProcAddr");
    LOAD_DEV(vkCreateShaderModule);
    LOAD_DEV(vkDestroyShaderModule);

    volatile int *desc_cnt = (volatile int *)dlsym(g_lib, "shim_m5_spirv_descriptor_loads_seen");
    if (!desc_cnt) {
        SKIP("spirv_pass_identifies_descriptor_loads", "shim counter symbol not present");
        return;
    }
    int before = *desc_cnt;
    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = oob_probe_spv_len,
        .pCode = (const uint32_t *)oob_probe_spv,
    };
    VkShaderModule mod = VK_NULL_HANDLE;
    VkResult r = vkCreateShaderModule(g_device, &smci, NULL, &mod);
    int after = *desc_cnt;
    if (mod != VK_NULL_HANDLE) vkDestroyShaderModule(g_device, mod, NULL);
    if (r != VK_SUCCESS) {
        FAIL("spirv_pass_identifies_descriptor_loads", "vkCreateShaderModule rejected: %s", vkresult_str(r));
        return;
    }
    /* The OOB probe shader has 2 SSBO loads (input[1024] AND outdata[0]
     * referenced for store path; the store of value into outdata[0]
     * uses an OpStore not OpLoad, so we mainly count input[1024]'s
     * load). At minimum, 1 descriptor load should be seen. */
    if (after - before >= 1)
        PASS("spirv_pass_identifies_descriptor_loads",
             "pass found %d descriptor-load(s) in OOB probe (counter %d→%d)",
             after - before, before, after);
    else
        FAIL("spirv_pass_identifies_descriptor_loads",
             "pass found 0 descriptor loads (counter stuck at %d) — pass not navigating SPIR-V correctly",
             before);
}

/* Verifies the shim's vkCreateShaderModule hook actually routes through
 * the SPIR-V instrumenter. Counter symbol is dlsym'd from the loaded
 * shim .so; missing in PASS A. We just create a shader module from the
 * embedded OOB probe SPIR-V — the body of that module isn't relevant
 * here, only that it's a valid binary the round-trip can parse. */
static void test_spirv_hook_fires(void) {
    PFN_vkGetDeviceProcAddr pfn_GetDeviceProcAddr =
        (PFN_vkGetDeviceProcAddr)g_vkGetInstanceProcAddr(g_instance, "vkGetDeviceProcAddr");
    LOAD_DEV(vkCreateShaderModule);
    LOAD_DEV(vkDestroyShaderModule);

    volatile int *cnt = (volatile int *)dlsym(g_lib, "shim_m5_spirv_instrument_count");
    if (!cnt) {
        SKIP("spirv_hook_fires", "shim counter symbol not present (vanilla wrapper or pre-SPIR-V build)");
        return;
    }
    int before = *cnt;
    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = oob_probe_spv_len,
        .pCode = (const uint32_t *)oob_probe_spv,
    };
    VkShaderModule mod = VK_NULL_HANDLE;
    VkResult r = vkCreateShaderModule(g_device, &smci, NULL, &mod);
    int after = *cnt;
    if (mod != VK_NULL_HANDLE) vkDestroyShaderModule(g_device, mod, NULL);
    if (r != VK_SUCCESS) {
        FAIL("spirv_hook_fires", "vkCreateShaderModule round-trip rejected the SPIR-V: %s", vkresult_str(r));
        return;
    }
    if (after > before)
        PASS("spirv_hook_fires",
             "shim parsed + reserialized OOB probe SPIR-V via SPIRV-Tools (counter %d→%d)",
             before, after);
    else
        FAIL("spirv_hook_fires", "counter did not advance (stuck at %d) — hook bypassed", before);
}

/* Phase A2 verification: the metadata-injection pass must add a
 * runtime-array uint SSBO at (set=6, binding=0) and decorate it
 * Block + ArrayStride 4 + member Offset 0. We verify two things:
 *   (1) the per-module counter advances under vkCreateShaderModule,
 *   (2) shim_spv_instrument's output bytes contain the expected
 *       OpDecorate target/literal pairs.
 *
 * (2) bypasses the wrapper entirely — calls into the shim's
 * public C entry directly via dlsym — so we can inspect the
 * post-pass binary without depending on the wrapper accepting it.
 * (1) doubles as the end-to-end "wrapper still accepts the
 * round-tripped binary" check (vkCreateShaderModule succeeds). */
static void test_spirv_pass_injects_metadata_binding(void) {
    PFN_vkGetDeviceProcAddr pfn_GetDeviceProcAddr =
        (PFN_vkGetDeviceProcAddr)g_vkGetInstanceProcAddr(g_instance, "vkGetDeviceProcAddr");
    LOAD_DEV(vkCreateShaderModule);
    LOAD_DEV(vkDestroyShaderModule);

    volatile int *inj_cnt = (volatile int *)dlsym(g_lib, "shim_m5_spirv_metadata_injected");
    typedef int (*pfn_instrument)(const uint32_t *, size_t, uint32_t **, size_t *);
    typedef void (*pfn_free)(uint32_t *);
    typedef int (*pfn_validate)(const uint32_t *, size_t, char *, size_t);
    pfn_instrument instrument = (pfn_instrument)dlsym(g_lib, "shim_spv_instrument");
    pfn_free       freefn     = (pfn_free)dlsym(g_lib, "shim_spv_free");
    pfn_validate   validate   = (pfn_validate)dlsym(g_lib, "shim_spv_validate");
    if (!inj_cnt || !instrument || !freefn || !validate) {
        SKIP("spirv_pass_injects_metadata_binding", "shim symbols not present (PASS A or pre-A2 build)");
        return;
    }

    int before = *inj_cnt;
    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = oob_probe_spv_len,
        .pCode = (const uint32_t *)oob_probe_spv,
    };
    VkShaderModule mod = VK_NULL_HANDLE;
    VkResult r = vkCreateShaderModule(g_device, &smci, NULL, &mod);
    int after = *inj_cnt;
    if (mod != VK_NULL_HANDLE) vkDestroyShaderModule(g_device, mod, NULL);
    if (r != VK_SUCCESS) {
        FAIL("spirv_pass_injects_metadata_binding",
             "vkCreateShaderModule rejected the instrumented module: %s "
             "(SPIRV-Tools emit may be malformed, or the wrapper's SPIR-V "
             "checker rejects the injected binding)",
             vkresult_str(r));
        return;
    }
    if (after - before < 1) {
        FAIL("spirv_pass_injects_metadata_binding",
             "injection counter did not advance (stuck at %d) — A2 pass "
             "either skipped (pre-1.3) or never ran",
             before);
        return;
    }

    /* Direct path: instrument the probe SPV out-of-band and grep the
     * bytes for our markers. */
    uint32_t *out_code = NULL;
    size_t    out_size = 0;
    int ok = instrument((const uint32_t *)oob_probe_spv, oob_probe_spv_len,
                        &out_code, &out_size);
    if (!ok || !out_code || out_size < 20) {
        if (out_code) freefn(out_code);
        FAIL("spirv_pass_injects_metadata_binding",
             "shim_spv_instrument returned no output bytes for the probe");
        return;
    }
    /* spirv-val (Vulkan 1.3 env). The plan requires the post-pass
     * binary to remain spec-valid; failure here would mean our
     * emit logic is malformed (wrong operand types / missing
     * decorations / interface mismatch) even if the wrapper
     * happens to accept it. */
    char vmsg[256];
    if (!validate(out_code, out_size, vmsg, sizeof(vmsg))) {
        freefn(out_code);
        FAIL("spirv_pass_injects_metadata_binding",
             "spirv-val rejected the instrumented module: %s", vmsg);
        return;
    }

    /* SPIR-V opcode literals (from spirv.json):
     *   OpDecorate           = 71
     *   OpMemberDecorate     = 72
     *   OpTypeRuntimeArray   = 29
     *   OpVariable           = 59
     * Decoration enum:
     *   Block         = 2
     *   ArrayStride   = 6
     *   NonWritable   = 24
     *   Binding       = 33
     *   DescriptorSet = 34
     *   Offset        = 35
     */
    int found_runtime_array = 0;
    int found_block_decoration = 0;
    int found_array_stride_4 = 0;
    int found_member_offset_0 = 0;
    int found_descriptor_set_7 = 0;
    int found_binding_0 = 0;
    int found_nonwritable = 0;
    uint32_t metadata_var_id_from_descset = 0;
    uint32_t metadata_var_id_from_binding = 0;

    const size_t total_words = out_size / 4;
    if (total_words < 5) goto check;
    /* Skip 5-word header. */
    size_t i = 5;
    while (i < total_words) {
        uint32_t w0 = out_code[i];
        uint32_t len = w0 >> 16;
        uint32_t op  = w0 & 0xFFFFu;
        if (len == 0 || i + len > total_words) break;
        switch (op) {
        case 29: /* OpTypeRuntimeArray */
            found_runtime_array++;
            break;
        case 71: /* OpDecorate target deco [literals...] */
            if (len >= 3) {
                uint32_t target = out_code[i + 1];
                uint32_t deco   = out_code[i + 2];
                uint32_t lit    = (len >= 4) ? out_code[i + 3] : 0u;
                if (deco == 2) found_block_decoration++;
                if (deco == 6 && lit == 4) found_array_stride_4++;
                if (deco == 24) found_nonwritable++;
                if (deco == 33 && lit == 0) {
                    found_binding_0++;
                    metadata_var_id_from_binding = target;
                }
                if (deco == 34 && lit == 6) {
                    found_descriptor_set_7++;
                    metadata_var_id_from_descset = target;
                }
            }
            break;
        case 72: /* OpMemberDecorate target member deco [literals...] */
            if (len >= 4) {
                uint32_t member = out_code[i + 2];
                uint32_t deco   = out_code[i + 3];
                uint32_t lit    = (len >= 5) ? out_code[i + 4] : 0u;
                if (deco == 35 && member == 0 && lit == 0) found_member_offset_0++;
            }
            break;
        default: break;
        }
        i += len;
    }
check:
    freefn(out_code);

    if (found_runtime_array < 1 ||
        found_block_decoration < 1 ||
        found_array_stride_4 < 1 ||
        found_member_offset_0 < 1 ||
        found_descriptor_set_7 != 1 ||
        found_binding_0 < 1 ||
        metadata_var_id_from_descset == 0 ||
        metadata_var_id_from_binding != metadata_var_id_from_descset) {
        FAIL("spirv_pass_injects_metadata_binding",
             "byte-scan of instrumented SPIR-V missing markers: "
             "runtime_array=%d Block=%d ArrayStride4=%d MemberOffset0=%d "
             "DescriptorSet6=%d Binding0=%d (descset_var=%u binding_var=%u)",
             found_runtime_array, found_block_decoration, found_array_stride_4,
             found_member_offset_0, found_descriptor_set_7, found_binding_0,
             metadata_var_id_from_descset, metadata_var_id_from_binding);
        return;
    }

    PASS("spirv_pass_injects_metadata_binding",
         "metadata SSBO injected (var_id=%u, set=6 binding=0, Block + "
         "ArrayStride 4 + member Offset 0); counter %d→%d, NonWritable=%d, "
         "spirv-val OK, wrapper accepted instrumented module",
         metadata_var_id_from_descset, before, after, found_nonwritable);
}

/* Phase A3 verification: byte-scan + spirv-val. End-to-end pipeline
 * dispatch is A4's responsibility — the metadata buffer must be
 * created and bound by the wrapper, not by the test. We assert:
 *   (1) shim_m5_spirv_loads_clamped advances per shader-module create,
 *   (2) shim_spv_instrument's output passes spirv-val,
 *   (3) byte-scan finds an OpAccessChain into the metadata var
 *       (set=6, binding=0), then OpULessThan, and OpSelect.
 *
 * A3 is gated behind SHIM_INSTRUMENT_ENABLE — set just around this
 * test's own vkCreateShaderModule. */
static void test_spirv_pass_clamps_descriptor_loads(void) {
    PFN_vkGetDeviceProcAddr pfn_GetDeviceProcAddr =
        (PFN_vkGetDeviceProcAddr)g_vkGetInstanceProcAddr(g_instance, "vkGetDeviceProcAddr");
    LOAD_DEV(vkCreateShaderModule);
    LOAD_DEV(vkDestroyShaderModule);

    volatile int *clamped_cnt = (volatile int *)dlsym(g_lib, "shim_m5_spirv_loads_clamped");
    typedef int (*pfn_instrument)(const uint32_t *, size_t, uint32_t **, size_t *);
    typedef void (*pfn_free)(uint32_t *);
    typedef int (*pfn_validate)(const uint32_t *, size_t, char *, size_t);
    pfn_instrument instrument = (pfn_instrument)dlsym(g_lib, "shim_spv_instrument");
    pfn_free       freefn     = (pfn_free)dlsym(g_lib, "shim_spv_free");
    pfn_validate   validate   = (pfn_validate)dlsym(g_lib, "shim_spv_validate");
    if (!clamped_cnt || !instrument || !freefn || !validate) {
        SKIP("spirv_pass_clamps_descriptor_loads", "shim symbols not present (PASS A or pre-A3 build)");
        return;
    }

    typedef void (*pfn_refresh)(void);
    pfn_refresh refresh = (pfn_refresh)dlsym(g_lib, "shim_a4_refresh_env");

    int before = *clamped_cnt;
    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = oob_probe_spv_len,
        .pCode = (const uint32_t *)oob_probe_spv,
    };
    VkShaderModule mod = VK_NULL_HANDLE;
    setenv("SHIM_INSTRUMENT_ENABLE", "1", 1);
    if (refresh) refresh();
    VkResult r = vkCreateShaderModule(g_device, &smci, NULL, &mod);
    int after = *clamped_cnt;
    if (mod != VK_NULL_HANDLE) vkDestroyShaderModule(g_device, mod, NULL);
    if (r != VK_SUCCESS) {
        unsetenv("SHIM_INSTRUMENT_ENABLE");
        if (refresh) refresh();
        FAIL("spirv_pass_clamps_descriptor_loads",
             "vkCreateShaderModule rejected the A3-clamped module: %s",
             vkresult_str(r));
        return;
    }
    if (after - before < 1) {
        unsetenv("SHIM_INSTRUMENT_ENABLE");
        if (refresh) refresh();
        FAIL("spirv_pass_clamps_descriptor_loads",
             "loads_clamped counter did not advance (stuck at %d) — A3 either skipped or never ran",
             before);
        return;
    }

    uint32_t *out_code = NULL;
    size_t    out_size = 0;
    int instrument_ok = instrument((const uint32_t *)oob_probe_spv, oob_probe_spv_len, &out_code, &out_size);
    unsetenv("SHIM_INSTRUMENT_ENABLE");
    if (refresh) refresh();
    if (!instrument_ok || !out_code || out_size < 20) {
        if (out_code) freefn(out_code);
        FAIL("spirv_pass_clamps_descriptor_loads", "shim_spv_instrument returned no output bytes");
        return;
    }
    char vmsg[256];
    if (!validate(out_code, out_size, vmsg, sizeof(vmsg))) {
        freefn(out_code);
        FAIL("spirv_pass_clamps_descriptor_loads",
             "spirv-val rejected the A3-clamped module: %s", vmsg);
        return;
    }

    /* Locate the metadata var_id (target of OpDecorate DescriptorSet 7
     * + OpDecorate Binding 0 sharing the same target).
     *   OpDecorate           = 71
     *   OpAccessChain        = 65
     *   OpULessThan          = 176
     *   OpSelect             = 169
     */
    uint32_t metadata_var_id = 0;
    {
        size_t total_words = out_size / 4;
        size_t i = 5;
        uint32_t descset_target = 0;
        while (i < total_words) {
            uint32_t w0 = out_code[i];
            uint32_t len = w0 >> 16;
            uint32_t op  = w0 & 0xFFFFu;
            if (len == 0 || i + len > total_words) break;
            if (op == 71 && len >= 4) {
                uint32_t target = out_code[i + 1];
                uint32_t deco   = out_code[i + 2];
                uint32_t lit    = out_code[i + 3];
                if (deco == 34 && lit == 6) descset_target = target;
                if (deco == 33 && lit == 0 && target == descset_target) {
                    metadata_var_id = target;
                }
            }
            i += len;
        }
    }
    if (metadata_var_id == 0) {
        freefn(out_code);
        FAIL("spirv_pass_clamps_descriptor_loads", "metadata var (set=6, binding=0) not found in A2 output");
        return;
    }

    int access_into_metadata = 0;
    int ulessthan_count = 0;
    int select_count = 0;
    {
        size_t total_words = out_size / 4;
        size_t i = 5;
        while (i < total_words) {
            uint32_t w0 = out_code[i];
            uint32_t len = w0 >> 16;
            uint32_t op  = w0 & 0xFFFFu;
            if (len == 0 || i + len > total_words) break;
            switch (op) {
            case 65:
                if (len >= 4) {
                    uint32_t base = out_code[i + 3];
                    if (base == metadata_var_id) access_into_metadata++;
                }
                break;
            case 176: ulessthan_count++; break;
            case 169: select_count++;    break;
            default: break;
            }
            i += len;
        }
    }
    freefn(out_code);

    if (access_into_metadata < 1 || ulessthan_count < 1 || select_count < 1) {
        FAIL("spirv_pass_clamps_descriptor_loads",
             "clamp scaffolding missing: AccessChain-into-metadata=%d "
             "OpULessThan=%d OpSelect=%d (expected ≥1 of each)",
             access_into_metadata, ulessthan_count, select_count);
        return;
    }
    PASS("spirv_pass_clamps_descriptor_loads",
         "A3 emitted %d clamp(s); counter %d→%d, AccessChain→metadata=%d, "
         "OpULessThan=%d, OpSelect=%d, spirv-val OK",
         after - before, before, after,
         access_into_metadata, ulessthan_count, select_count);
}

/* Phase A5 verification: image-read instrumentation should wrap
 * OpImageRead with OpImageQuerySize + bounds predicates + OpSelect-zero.
 * This is SPIR-V-only; the storage-image dispatch test below checks the
 * runtime behavior on Mali. */
static void test_spirv_pass_clamps_image_reads(void) {
    PFN_vkGetDeviceProcAddr pfn_GetDeviceProcAddr =
        (PFN_vkGetDeviceProcAddr)g_vkGetInstanceProcAddr(g_instance, "vkGetDeviceProcAddr");
    LOAD_DEV(vkCreateShaderModule);
    LOAD_DEV(vkDestroyShaderModule);

    volatile int *clamped_cnt = (volatile int *)dlsym(g_lib, "shim_m5_spirv_image_ops_clamped");
    typedef int (*pfn_instrument)(const uint32_t *, size_t, uint32_t **, size_t *);
    typedef void (*pfn_free)(uint32_t *);
    typedef int (*pfn_validate)(const uint32_t *, size_t, char *, size_t);
    pfn_instrument instrument = (pfn_instrument)dlsym(g_lib, "shim_spv_instrument");
    pfn_free       freefn     = (pfn_free)dlsym(g_lib, "shim_spv_free");
    pfn_validate   validate   = (pfn_validate)dlsym(g_lib, "shim_spv_validate");
    if (!clamped_cnt || !instrument || !freefn || !validate) {
        SKIP("spirv_pass_clamps_image_reads", "shim symbols not present (PASS A or pre-A5 build)");
        return;
    }

    typedef void (*pfn_refresh)(void);
    pfn_refresh refresh = (pfn_refresh)dlsym(g_lib, "shim_a4_refresh_env");

    int before = *clamped_cnt;
    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = oob_image_probe_spv_len,
        .pCode = (const uint32_t *)oob_image_probe_spv,
    };
    VkShaderModule mod = VK_NULL_HANDLE;
    setenv("SHIM_INSTRUMENT_ENABLE", "1", 1);
    if (refresh) refresh();
    VkResult r = vkCreateShaderModule(g_device, &smci, NULL, &mod);
    int after = *clamped_cnt;
    if (mod != VK_NULL_HANDLE) vkDestroyShaderModule(g_device, mod, NULL);
    if (r != VK_SUCCESS) {
        unsetenv("SHIM_INSTRUMENT_ENABLE");
        if (refresh) refresh();
        FAIL("spirv_pass_clamps_image_reads", "vkCreateShaderModule rejected A5 module: %s", vkresult_str(r));
        return;
    }
    if (after - before < 2) {
        unsetenv("SHIM_INSTRUMENT_ENABLE");
        if (refresh) refresh();
        FAIL("spirv_pass_clamps_image_reads",
             "image clamp counter advanced by %d, expected at least 2 imageLoad ops",
             after - before);
        return;
    }

    uint32_t *out_code = NULL;
    size_t out_size = 0;
    int ok = instrument((const uint32_t *)oob_image_probe_spv, oob_image_probe_spv_len,
                        &out_code, &out_size);
    unsetenv("SHIM_INSTRUMENT_ENABLE");
    if (refresh) refresh();
    if (!ok || !out_code || out_size < 20) {
        if (out_code) freefn(out_code);
        FAIL("spirv_pass_clamps_image_reads", "shim_spv_instrument returned no output bytes");
        return;
    }
    char vmsg[256];
    if (!validate(out_code, out_size, vmsg, sizeof(vmsg))) {
        freefn(out_code);
        FAIL("spirv_pass_clamps_image_reads", "spirv-val rejected A5 module: %s", vmsg);
        return;
    }

    int image_query_size = 0, select_count = 0, logical_and = 0;
    size_t total_words = out_size / 4;
    for (size_t i = 5; i < total_words;) {
        uint32_t w0 = out_code[i];
        uint32_t len = w0 >> 16;
        uint32_t op = w0 & 0xFFFFu;
        if (len == 0 || i + len > total_words) break;
        if (op == 104) image_query_size++; /* OpImageQuerySize */
        if (op == 169) select_count++;     /* OpSelect */
        if (op == 167) logical_and++;      /* OpLogicalAnd */
        i += len;
    }
    freefn(out_code);

    if (image_query_size < 2 || select_count < 2 || logical_and < 2) {
        FAIL("spirv_pass_clamps_image_reads",
             "A5 scaffolding missing: OpImageQuerySize=%d OpSelect=%d OpLogicalAnd=%d",
             image_query_size, select_count, logical_and);
        return;
    }
    PASS("spirv_pass_clamps_image_reads",
         "A5 emitted image-read clamps; counter %d→%d, OpImageQuerySize=%d OpSelect=%d, spirv-val OK",
         before, after, image_query_size, select_count);
}

/* A5 sampled-image fetch slice: texelFetch lowers to OpImageFetch with a
 * Lod image operand. The pass should use OpImageQuerySizeLod so mip-level
 * coordinates are checked against the correct dimensions. */
static void test_spirv_pass_clamps_image_fetches(void) {
    PFN_vkGetDeviceProcAddr pfn_GetDeviceProcAddr =
        (PFN_vkGetDeviceProcAddr)g_vkGetInstanceProcAddr(g_instance, "vkGetDeviceProcAddr");
    LOAD_DEV(vkCreateShaderModule);
    LOAD_DEV(vkDestroyShaderModule);

    volatile int *clamped_cnt = (volatile int *)dlsym(g_lib, "shim_m5_spirv_image_ops_clamped");
    typedef int (*pfn_instrument)(const uint32_t *, size_t, uint32_t **, size_t *);
    typedef void (*pfn_free)(uint32_t *);
    typedef int (*pfn_validate)(const uint32_t *, size_t, char *, size_t);
    pfn_instrument instrument = (pfn_instrument)dlsym(g_lib, "shim_spv_instrument");
    pfn_free       freefn     = (pfn_free)dlsym(g_lib, "shim_spv_free");
    pfn_validate   validate   = (pfn_validate)dlsym(g_lib, "shim_spv_validate");
    if (!clamped_cnt || !instrument || !freefn || !validate) {
        SKIP("spirv_pass_clamps_image_fetches", "shim symbols not present (PASS A or pre-A5 build)");
        return;
    }

    typedef void (*pfn_refresh)(void);
    pfn_refresh refresh = (pfn_refresh)dlsym(g_lib, "shim_a4_refresh_env");

    int before = *clamped_cnt;
    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = oob_fetch_probe_spv_len,
        .pCode = (const uint32_t *)oob_fetch_probe_spv,
    };
    VkShaderModule mod = VK_NULL_HANDLE;
    setenv("SHIM_INSTRUMENT_ENABLE", "1", 1);
    if (refresh) refresh();
    VkResult r = vkCreateShaderModule(g_device, &smci, NULL, &mod);
    int after = *clamped_cnt;
    if (mod != VK_NULL_HANDLE) vkDestroyShaderModule(g_device, mod, NULL);
    if (r != VK_SUCCESS) {
        unsetenv("SHIM_INSTRUMENT_ENABLE");
        if (refresh) refresh();
        FAIL("spirv_pass_clamps_image_fetches", "vkCreateShaderModule rejected A5 fetch module: %s", vkresult_str(r));
        return;
    }
    if (after - before < 2) {
        unsetenv("SHIM_INSTRUMENT_ENABLE");
        if (refresh) refresh();
        FAIL("spirv_pass_clamps_image_fetches",
             "image clamp counter advanced by %d, expected at least 2 texelFetch ops",
             after - before);
        return;
    }

    uint32_t *out_code = NULL;
    size_t out_size = 0;
    int ok = instrument((const uint32_t *)oob_fetch_probe_spv, oob_fetch_probe_spv_len,
                        &out_code, &out_size);
    unsetenv("SHIM_INSTRUMENT_ENABLE");
    if (refresh) refresh();
    if (!ok || !out_code || out_size < 20) {
        if (out_code) freefn(out_code);
        FAIL("spirv_pass_clamps_image_fetches", "shim_spv_instrument returned no output bytes");
        return;
    }
    char vmsg[256];
    if (!validate(out_code, out_size, vmsg, sizeof(vmsg))) {
        freefn(out_code);
        FAIL("spirv_pass_clamps_image_fetches", "spirv-val rejected A5 fetch module: %s", vmsg);
        return;
    }

    int image_query_size_lod = 0, image_fetch = 0, select_count = 0;
    size_t total_words = out_size / 4;
    for (size_t i = 5; i < total_words;) {
        uint32_t w0 = out_code[i];
        uint32_t len = w0 >> 16;
        uint32_t op = w0 & 0xFFFFu;
        if (len == 0 || i + len > total_words) break;
        if (op == 103) image_query_size_lod++; /* OpImageQuerySizeLod */
        if (op == 95) image_fetch++;           /* OpImageFetch */
        if (op == 169) select_count++;         /* OpSelect */
        i += len;
    }
    freefn(out_code);

    if (image_query_size_lod < 2 || image_fetch < 2 || select_count < 2) {
        FAIL("spirv_pass_clamps_image_fetches",
             "A5 fetch scaffolding missing: OpImageQuerySizeLod=%d OpImageFetch=%d OpSelect=%d",
             image_query_size_lod, image_fetch, select_count);
        return;
    }
    PASS("spirv_pass_clamps_image_fetches",
         "A5 emitted texelFetch clamps; counter %d→%d, OpImageQuerySizeLod=%d OpSelect=%d, spirv-val OK",
         before, after, image_query_size_lod, select_count);
}

/* Wrapper-behavior probe: dispatch a compute shader that reads SSBO
 * index 1024 through a descriptor whose range covers only 1 element
 * (4 bytes). The underlying buffer ALLOCATION is 16 KiB pre-filled
 * with 0xDEADBEEF, so byte-offset 4096 (= index 1024 × sizeof(uint))
 * exists in memory but is outside the descriptor's declared range.
 * Result tells us how Mali / leegao handle OOB descriptor access:
 *
 *   result == 0          → Mali zeros OOB at descriptor level (free SSBO robustness2)
 *   result == 0xDEADBEEF → Mali ignored descriptor range (instrumentation needed)
 *   anything else        → garbage / fault (instrumentation needed)
 *
 * This is a wrapper-behavior test, not a shim-correctness test — it
 * gives the same answer in PASS A and PASS B. The point is to inform
 * whether real robustBufferAccess2 needs SPIR-V instrumentation or
 * whether Mali already delivers it. */
static void test_mali_oob_ssbo_probe(void) {
    PFN_vkGetDeviceProcAddr pfn_GetDeviceProcAddr =
        (PFN_vkGetDeviceProcAddr)g_vkGetInstanceProcAddr(g_instance, "vkGetDeviceProcAddr");
    /* When the shim has the A4 runtime metadata-buffer infrastructure,
     * setting SHIM_INSTRUMENT_ENABLE around the entire probe makes A3+A4
     * fully transparent — vkCreateShaderModule clamps OOB loads,
     * vkCreatePipelineLayout extends to slot 6 = our meta layout,
     * vkAllocateDescriptorSets registers the test's set,
     * vkUpdateDescriptorSets captures the (set 0, binding 0) range,
     * vkCmdBindDescriptorSets refreshes the metadata buffer + binds
     * our meta set at slot 6. */
    int a4_active = ((volatile int *)dlsym(g_lib, "shim_m5_a4_pipeline_layouts_extended")) != NULL;
    volatile int *a4_binds_cnt = a4_active
        ? (volatile int *)dlsym(g_lib, "shim_m5_a4_binds_extended")
        : NULL;
    int a4_binds_before = a4_binds_cnt ? *a4_binds_cnt : -1;
    typedef void (*pfn_refresh)(void);
    pfn_refresh a4_refresh = a4_active ? (pfn_refresh)dlsym(g_lib, "shim_a4_refresh_env") : NULL;
    if (a4_active) {
        setenv("SHIM_INSTRUMENT_ENABLE", "1", 1);
        if (a4_refresh) a4_refresh();
    }
    LOAD_DEV(vkCreateBuffer);
    LOAD_DEV(vkDestroyBuffer);
    LOAD_DEV(vkAllocateMemory);
    LOAD_DEV(vkFreeMemory);
    LOAD_DEV(vkBindBufferMemory);
    LOAD_DEV(vkGetBufferMemoryRequirements);
    LOAD_DEV(vkMapMemory);
    LOAD_DEV(vkUnmapMemory);
    LOAD_DEV(vkCreateShaderModule);
    LOAD_DEV(vkDestroyShaderModule);
    LOAD_DEV(vkCreateDescriptorSetLayout);
    LOAD_DEV(vkDestroyDescriptorSetLayout);
    LOAD_DEV(vkCreateDescriptorPool);
    LOAD_DEV(vkDestroyDescriptorPool);
    LOAD_DEV(vkAllocateDescriptorSets);
    LOAD_DEV(vkUpdateDescriptorSets);
    LOAD_DEV(vkCreatePipelineLayout);
    LOAD_DEV(vkDestroyPipelineLayout);
    LOAD_DEV(vkCreateComputePipelines);
    LOAD_DEV(vkDestroyPipeline);
    LOAD_DEV(vkCreateCommandPool);
    LOAD_DEV(vkDestroyCommandPool);
    LOAD_DEV(vkAllocateCommandBuffers);
    LOAD_DEV(vkBeginCommandBuffer);
    LOAD_DEV(vkEndCommandBuffer);
    LOAD_DEV(vkCmdBindPipeline);
    LOAD_DEV(vkCmdBindDescriptorSets);
    LOAD_DEV(vkCmdDispatch);
    LOAD_DEV(vkCreateFence);
    LOAD_DEV(vkDestroyFence);
    LOAD_DEV(vkWaitForFences);
    LOAD_DEV(vkGetDeviceQueue);
    LOAD_DEV(vkQueueSubmit);
    LOAD_INST(vkGetPhysicalDeviceMemoryProperties);

    /* Buffers — 16 KiB host-visible. */
    const VkDeviceSize BUF_SZ = 16 * 1024;
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = BUF_SZ, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer in_buf = VK_NULL_HANDLE, out_buf = VK_NULL_HANDLE;
    VkResult r = vkCreateBuffer(g_device, &bci, NULL, &in_buf);
    if (r != VK_SUCCESS) { FAIL("mali_oob_ssbo_probe", "vkCreateBuffer in: %s", vkresult_str(r)); return; }
    r = vkCreateBuffer(g_device, &bci, NULL, &out_buf);
    if (r != VK_SUCCESS) { FAIL("mali_oob_ssbo_probe", "vkCreateBuffer out: %s", vkresult_str(r));
        vkDestroyBuffer(g_device, in_buf, NULL); return; }

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_device, in_buf, &req);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(g_phys, &mp);
    int mt = -1;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if (!(req.memoryTypeBits & (1u << i))) continue;
        VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if ((mp.memoryTypes[i].propertyFlags & want) == want) { mt = (int)i; break; }
    }
    if (mt < 0) { FAIL("mali_oob_ssbo_probe", "no host-visible+coherent memory"); return; }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = (uint32_t)mt };
    VkDeviceMemory in_mem = VK_NULL_HANDLE, out_mem = VK_NULL_HANDLE;
    vkAllocateMemory(g_device, &mai, NULL, &in_mem);
    vkAllocateMemory(g_device, &mai, NULL, &out_mem);
    vkBindBufferMemory(g_device, in_buf,  in_mem,  0);
    vkBindBufferMemory(g_device, out_buf, out_mem, 0);

    /* Pre-fill input with 0xDEADBEEF so OOB reads against the underlying
     * allocation produce a recognizable marker. Output starts zeroed. */
    void *p = NULL;
    vkMapMemory(g_device, in_mem, 0, BUF_SZ, 0, &p);
    for (size_t i = 0; i < BUF_SZ / 4; i++) ((uint32_t *)p)[i] = 0xDEADBEEFu;
    vkUnmapMemory(g_device, in_mem);
    vkMapMemory(g_device, out_mem, 0, BUF_SZ, 0, &p);
    memset(p, 0, BUF_SZ);
    vkUnmapMemory(g_device, out_mem);

    /* Compute pipeline. */
    VkShaderModuleCreateInfo smci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = oob_probe_spv_len, .pCode = (const uint32_t *)oob_probe_spv };
    VkShaderModule shader = VK_NULL_HANDLE;
    r = vkCreateShaderModule(g_device, &smci, NULL, &shader);
    if (r != VK_SUCCESS) { FAIL("mali_oob_ssbo_probe", "vkCreateShaderModule: %s", vkresult_str(r));
        goto cleanup_buffers; }

    VkDescriptorSetLayoutBinding dslb[2] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
    };
    VkDescriptorSetLayoutCreateInfo dslci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = dslb };
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    r = vkCreateDescriptorSetLayout(g_device, &dslci, NULL, &dsl);
    if (r != VK_SUCCESS) { FAIL("mali_oob_ssbo_probe", "vkCreateDescriptorSetLayout: %s", vkresult_str(r));
        vkDestroyShaderModule(g_device, shader, NULL); goto cleanup_buffers; }

    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &dsl };
    VkPipelineLayout pl = VK_NULL_HANDLE;
    vkCreatePipelineLayout(g_device, &plci, NULL, &pl);

    VkComputePipelineCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader, .pName = "main" },
        .layout = pl };
    VkPipeline pipe = VK_NULL_HANDLE;
    r = vkCreateComputePipelines(g_device, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe);
    if (r != VK_SUCCESS) { FAIL("mali_oob_ssbo_probe", "vkCreateComputePipelines: %s", vkresult_str(r));
        vkDestroyPipelineLayout(g_device, pl, NULL); vkDestroyDescriptorSetLayout(g_device, dsl, NULL);
        vkDestroyShaderModule(g_device, shader, NULL); goto cleanup_buffers; }

    VkDescriptorPoolSize ps = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 2 };
    VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    vkCreateDescriptorPool(g_device, &dpci, NULL, &dpool);
    VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
    VkDescriptorSet dset = VK_NULL_HANDLE;
    vkAllocateDescriptorSets(g_device, &dsai, &dset);

    /* The crux: input descriptor RANGE = 4 bytes. Underlying allocation
     * has 16 KiB of 0xDEADBEEF, so byte-offset 4096 is real memory but
     * outside the descriptor's declared range. */
    VkDescriptorBufferInfo bi_in  = { .buffer = in_buf,  .offset = 0, .range = 4 };
    VkDescriptorBufferInfo bi_out = { .buffer = out_buf, .offset = 0, .range = VK_WHOLE_SIZE };
    VkWriteDescriptorSet writes[2] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 0,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi_in },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 1,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi_out },
    };
    vkUpdateDescriptorSets(g_device, 2, writes, 0, NULL);

    /* Record + submit. */
    VkCommandPoolCreateInfo cpci2 = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = g_gfx_qfam };
    VkCommandPool cpool = VK_NULL_HANDLE;
    vkCreateCommandPool(g_device, &cpci2, NULL, &cpool);
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(g_device, &cbai, &cb);
    VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(cb, &cbbi);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &dset, 0, NULL);
    vkCmdDispatch(cb, 1, 1, 1);
    vkEndCommandBuffer(cb);

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(g_device, g_gfx_qfam, 0, &queue);
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(g_device, &fci, NULL, &fence);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &cb };
    r = vkQueueSubmit(queue, 1, &si, fence);
    if (r != VK_SUCCESS) {
        FAIL("mali_oob_ssbo_probe", "vkQueueSubmit: %s (Mali rejected the dispatch)", vkresult_str(r));
        goto cleanup_all;
    }
    r = vkWaitForFences(g_device, 1, &fence, VK_TRUE, 5ULL * 1000 * 1000 * 1000);
    if (r != VK_SUCCESS) {
        FAIL("mali_oob_ssbo_probe",
             "vkWaitForFences: %s (Mali likely faulted on OOB read — instrumentation REQUIRED for robustness2)",
             vkresult_str(r));
        goto cleanup_all;
    }

    /* Read result. */
    uint32_t result = 0xCAFEBABEu;
    void *outp = NULL;
    vkMapMemory(g_device, out_mem, 0, BUF_SZ, 0, &outp);
    if (outp) result = *(uint32_t *)outp;
    vkUnmapMemory(g_device, out_mem);
    int a4_binds_after = a4_binds_cnt ? *a4_binds_cnt : -1;

    if (result == 0u) {
        if (a4_active) {
            if (!a4_binds_cnt || a4_binds_after <= a4_binds_before) {
                FAIL("mali_oob_ssbo_probe",
                     "result=0 but A4 bind counter did not advance (%d→%d) — metadata path was not proven",
                     a4_binds_before, a4_binds_after);
                goto cleanup_all;
            }
            PASS("mali_oob_ssbo_probe",
                 "result=0 with A3+A4 active → SPIR-V instrumentation clamped the OOB read; metadata bind counter %d→%d",
                 a4_binds_before, a4_binds_after);
        } else {
            PASS("mali_oob_ssbo_probe",
                 "result=0 → Mali zeros OOB at descriptor-range level (real robustness2 for SSBOs comes free, no SPIR-V instrumentation needed for buffer reads)");
        }
    } else if (result == 0xDEADBEEFu) {
        if (a4_active) {
            FAIL("mali_oob_ssbo_probe",
                 "result=0xDEADBEEF with A3+A4 active → instrumentation did NOT clamp; check counters: pipeline_layouts_extended, sets_tracked, writes_recorded, binds_extended");
        } else {
            INCOMPLETE("mali_oob_ssbo_probe",
                       "result=0xDEADBEEF → Mali read past descriptor range into underlying allocation; spec-correct robustness2 requires SPIR-V instrumentation");
        }
    } else {
        INCOMPLETE("mali_oob_ssbo_probe",
                   "result=0x%08x → unexpected (neither zero nor marker); behavior is implementation-defined garbage, instrumentation required",
                   result);
    }

cleanup_all:
    vkDestroyFence(g_device, fence, NULL);
    vkDestroyCommandPool(g_device, cpool, NULL);
    vkDestroyDescriptorPool(g_device, dpool, NULL);
    vkDestroyPipeline(g_device, pipe, NULL);
    vkDestroyPipelineLayout(g_device, pl, NULL);
    vkDestroyDescriptorSetLayout(g_device, dsl, NULL);
    vkDestroyShaderModule(g_device, shader, NULL);
cleanup_buffers:
    vkFreeMemory(g_device, in_mem, NULL);
    vkFreeMemory(g_device, out_mem, NULL);
    vkDestroyBuffer(g_device, in_buf, NULL);
    vkDestroyBuffer(g_device, out_buf, NULL);
    if (a4_active) {
        unsetenv("SHIM_INSTRUMENT_ENABLE");
        if (a4_refresh) a4_refresh();
    }
}

/* End-to-end A5 probe: a compute shader reads one in-bounds texel and one
 * out-of-bounds texel from a 1x1 R32_UINT storage image. A5 should preserve
 * the in-bounds marker and select zero for the OOB imageLoad. */
static void test_mali_oob_storage_image_probe(void) {
    volatile int *a5_clamped = (volatile int *)dlsym(g_lib, "shim_m5_spirv_image_ops_clamped");
    volatile int *a4_binds_cnt = (volatile int *)dlsym(g_lib, "shim_m5_a4_binds_extended");
    typedef void (*pfn_refresh)(void);
    pfn_refresh a4_refresh = (pfn_refresh)dlsym(g_lib, "shim_a4_refresh_env");
    if (!a5_clamped || !a4_binds_cnt || !a4_refresh) {
        SKIP("mali_oob_storage_image_probe", "shim A5/A4 symbols not present (PASS A or pre-A5 build)");
        return;
    }

    PFN_vkGetDeviceProcAddr pfn_GetDeviceProcAddr =
        (PFN_vkGetDeviceProcAddr)g_vkGetInstanceProcAddr(g_instance, "vkGetDeviceProcAddr");
    LOAD_DEV(vkCreateImage);
    LOAD_DEV(vkDestroyImage);
    LOAD_DEV(vkGetImageMemoryRequirements);
    LOAD_DEV(vkBindImageMemory);
    LOAD_DEV(vkCreateImageView);
    LOAD_DEV(vkDestroyImageView);
    LOAD_DEV(vkCreateBuffer);
    LOAD_DEV(vkDestroyBuffer);
    LOAD_DEV(vkGetBufferMemoryRequirements);
    LOAD_DEV(vkAllocateMemory);
    LOAD_DEV(vkFreeMemory);
    LOAD_DEV(vkBindBufferMemory);
    LOAD_DEV(vkMapMemory);
    LOAD_DEV(vkUnmapMemory);
    LOAD_DEV(vkCreateShaderModule);
    LOAD_DEV(vkDestroyShaderModule);
    LOAD_DEV(vkCreateDescriptorSetLayout);
    LOAD_DEV(vkDestroyDescriptorSetLayout);
    LOAD_DEV(vkCreateDescriptorPool);
    LOAD_DEV(vkDestroyDescriptorPool);
    LOAD_DEV(vkAllocateDescriptorSets);
    LOAD_DEV(vkUpdateDescriptorSets);
    LOAD_DEV(vkCreatePipelineLayout);
    LOAD_DEV(vkDestroyPipelineLayout);
    LOAD_DEV(vkCreateComputePipelines);
    LOAD_DEV(vkDestroyPipeline);
    LOAD_DEV(vkCreateCommandPool);
    LOAD_DEV(vkDestroyCommandPool);
    LOAD_DEV(vkAllocateCommandBuffers);
    LOAD_DEV(vkBeginCommandBuffer);
    LOAD_DEV(vkEndCommandBuffer);
    LOAD_DEV(vkCmdPipelineBarrier);
    LOAD_DEV(vkCmdBindPipeline);
    LOAD_DEV(vkCmdBindDescriptorSets);
    LOAD_DEV(vkCmdDispatch);
    LOAD_DEV(vkCreateFence);
    LOAD_DEV(vkDestroyFence);
    LOAD_DEV(vkWaitForFences);
    LOAD_DEV(vkGetDeviceQueue);
    LOAD_DEV(vkQueueSubmit);

    setenv("SHIM_INSTRUMENT_ENABLE", "1", 1);
    a4_refresh();
    int a5_before = *a5_clamped;
    int binds_before = *a4_binds_cnt;

    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory img_mem = VK_NULL_HANDLE;
    VkImageView img_view = VK_NULL_HANDLE;
    VkBuffer out_buf = VK_NULL_HANDLE;
    VkDeviceMemory out_mem = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkCommandPool cpool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R32_UINT,
        .extent = { 1, 1, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult r = vkCreateImage(g_device, &ici, NULL, &img);
    if (r != VK_SUCCESS) { SKIP("mali_oob_storage_image_probe", "vkCreateImage(R32_UINT storage): %s", vkresult_str(r)); goto cleanup; }
    VkMemoryRequirements ireq;
    vkGetImageMemoryRequirements(g_device, img, &ireq);
    int imt = pick_memory_type(ireq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (imt < 0) { FAIL("mali_oob_storage_image_probe", "no memory type for storage image"); goto cleanup; }
    VkMemoryAllocateInfo imai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = ireq.size, .memoryTypeIndex = (uint32_t)imt };
    r = vkAllocateMemory(g_device, &imai, NULL, &img_mem);
    if (r != VK_SUCCESS) { FAIL("mali_oob_storage_image_probe", "vkAllocateMemory image: %s", vkresult_str(r)); goto cleanup; }
    vkBindImageMemory(g_device, img, img_mem, 0);

    VkImageViewCreateInfo ivci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R32_UINT,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    r = vkCreateImageView(g_device, &ivci, NULL, &img_view);
    if (r != VK_SUCCESS) { FAIL("mali_oob_storage_image_probe", "vkCreateImageView: %s", vkresult_str(r)); goto cleanup; }

    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 8, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    r = vkCreateBuffer(g_device, &bci, NULL, &out_buf);
    if (r != VK_SUCCESS) { FAIL("mali_oob_storage_image_probe", "vkCreateBuffer out: %s", vkresult_str(r)); goto cleanup; }
    VkMemoryRequirements breq;
    vkGetBufferMemoryRequirements(g_device, out_buf, &breq);
    int bmt = pick_memory_type(breq.memoryTypeBits,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (bmt < 0) { FAIL("mali_oob_storage_image_probe", "no host-visible buffer memory"); goto cleanup; }
    VkMemoryAllocateInfo bmai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = breq.size, .memoryTypeIndex = (uint32_t)bmt };
    r = vkAllocateMemory(g_device, &bmai, NULL, &out_mem);
    if (r != VK_SUCCESS) { FAIL("mali_oob_storage_image_probe", "vkAllocateMemory out: %s", vkresult_str(r)); goto cleanup; }
    vkBindBufferMemory(g_device, out_buf, out_mem, 0);
    void *p = NULL;
    vkMapMemory(g_device, out_mem, 0, 8, 0, &p);
    if (p) memset(p, 0, 8);
    vkUnmapMemory(g_device, out_mem);

    VkShaderModuleCreateInfo smci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = oob_image_probe_spv_len, .pCode = (const uint32_t *)oob_image_probe_spv };
    r = vkCreateShaderModule(g_device, &smci, NULL, &shader);
    int a5_after_shader = *a5_clamped;
    if (r != VK_SUCCESS) { FAIL("mali_oob_storage_image_probe", "vkCreateShaderModule: %s", vkresult_str(r)); goto cleanup; }
    if (a5_after_shader - a5_before < 2) {
        FAIL("mali_oob_storage_image_probe", "A5 counter advanced by %d, expected at least 2", a5_after_shader - a5_before);
        goto cleanup;
    }

    VkDescriptorSetLayoutBinding dslb[2] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
    };
    VkDescriptorSetLayoutCreateInfo dslci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = dslb };
    r = vkCreateDescriptorSetLayout(g_device, &dslci, NULL, &dsl);
    if (r != VK_SUCCESS) { FAIL("mali_oob_storage_image_probe", "vkCreateDescriptorSetLayout: %s", vkresult_str(r)); goto cleanup; }

    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &dsl };
    r = vkCreatePipelineLayout(g_device, &plci, NULL, &pl);
    if (r != VK_SUCCESS) { FAIL("mali_oob_storage_image_probe", "vkCreatePipelineLayout: %s", vkresult_str(r)); goto cleanup; }

    VkComputePipelineCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader, .pName = "main" },
        .layout = pl };
    r = vkCreateComputePipelines(g_device, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe);
    if (r != VK_SUCCESS) { FAIL("mali_oob_storage_image_probe", "vkCreateComputePipelines: %s", vkresult_str(r)); goto cleanup; }

    VkDescriptorPoolSize ps[2] = {
        { .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1 },
        { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1 },
    };
    VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = ps };
    r = vkCreateDescriptorPool(g_device, &dpci, NULL, &dpool);
    if (r != VK_SUCCESS) { FAIL("mali_oob_storage_image_probe", "vkCreateDescriptorPool: %s", vkresult_str(r)); goto cleanup; }
    VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
    VkDescriptorSet dset = VK_NULL_HANDLE;
    r = vkAllocateDescriptorSets(g_device, &dsai, &dset);
    if (r != VK_SUCCESS) { FAIL("mali_oob_storage_image_probe", "vkAllocateDescriptorSets: %s", vkresult_str(r)); goto cleanup; }

    VkDescriptorImageInfo ii = { .imageView = img_view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorBufferInfo bi = { .buffer = out_buf, .offset = 0, .range = 8 };
    VkWriteDescriptorSet writes[2] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 0,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &ii },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 1,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi },
    };
    vkUpdateDescriptorSets(g_device, 2, writes, 0, NULL);

    VkCommandPoolCreateInfo cpci2 = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = g_gfx_qfam };
    r = vkCreateCommandPool(g_device, &cpci2, NULL, &cpool);
    if (r != VK_SUCCESS) { FAIL("mali_oob_storage_image_probe", "vkCreateCommandPool: %s", vkresult_str(r)); goto cleanup; }
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(g_device, &cbai, &cb);
    VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(cb, &cbbi);
    VkImageMemoryBarrier imb = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 1, &imb);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &dset, 0, NULL);
    vkCmdDispatch(cb, 1, 1, 1);
    vkEndCommandBuffer(cb);

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(g_device, g_gfx_qfam, 0, &queue);
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    vkCreateFence(g_device, &fci, NULL, &fence);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &cb };
    r = vkQueueSubmit(queue, 1, &si, fence);
    if (r != VK_SUCCESS) { FAIL("mali_oob_storage_image_probe", "vkQueueSubmit: %s", vkresult_str(r)); goto cleanup; }
    r = vkWaitForFences(g_device, 1, &fence, VK_TRUE, 5ULL * 1000 * 1000 * 1000);
    if (r != VK_SUCCESS) { FAIL("mali_oob_storage_image_probe", "vkWaitForFences: %s", vkresult_str(r)); goto cleanup; }

    uint32_t result[2] = { 0xCAFEBABEu, 0xCAFEBABEu };
    vkMapMemory(g_device, out_mem, 0, 8, 0, &p);
    if (p) memcpy(result, p, 8);
    vkUnmapMemory(g_device, out_mem);
    int binds_after = *a4_binds_cnt;
    if (binds_after <= binds_before) {
        FAIL("mali_oob_storage_image_probe", "A4 metadata bind counter did not advance (%d→%d)", binds_before, binds_after);
    } else if (result[0] == 0xDEADBEEFu && result[1] == 0u) {
        PASS("mali_oob_storage_image_probe",
             "in-bounds imageLoad preserved 0x%08x and OOB imageLoad zeroed; A5 counter %d→%d, A4 binds %d→%d",
             result[0], a5_before, a5_after_shader, binds_before, binds_after);
    } else {
        FAIL("mali_oob_storage_image_probe",
             "unexpected results: in_bounds=0x%08x oob=0x%08x (expected 0xDEADBEEF,0)",
             result[0], result[1]);
    }

cleanup:
    if (fence) vkDestroyFence(g_device, fence, NULL);
    if (cpool) vkDestroyCommandPool(g_device, cpool, NULL);
    if (dpool) vkDestroyDescriptorPool(g_device, dpool, NULL);
    if (pipe) vkDestroyPipeline(g_device, pipe, NULL);
    if (pl) vkDestroyPipelineLayout(g_device, pl, NULL);
    if (dsl) vkDestroyDescriptorSetLayout(g_device, dsl, NULL);
    if (shader) vkDestroyShaderModule(g_device, shader, NULL);
    if (out_mem) vkFreeMemory(g_device, out_mem, NULL);
    if (out_buf) vkDestroyBuffer(g_device, out_buf, NULL);
    if (img_view) vkDestroyImageView(g_device, img_view, NULL);
    if (img_mem) vkFreeMemory(g_device, img_mem, NULL);
    if (img) vkDestroyImage(g_device, img, NULL);
    unsetenv("SHIM_INSTRUMENT_ENABLE");
    a4_refresh();
}

/* Control: vkCreateBuffer(usage=0) with NO flags2 pNext at all. Per
 * VUID-VkBufferCreateInfo-usage-parameter, usage must be non-zero. If
 * the wrapper rejects this, the partner _fold test's PASS is meaningful
 * (folding is the only way the buffer becomes valid). If the wrapper
 * accepts usage=0 here, the wrapper is lax and the _fold PASS is
 * ambiguous (might be the shim, might be the wrapper not validating). */
static void test_wrapper_usage_zero_strictness(void) {
    PFN_vkGetDeviceProcAddr pfn_GetDeviceProcAddr =
        (PFN_vkGetDeviceProcAddr)g_vkGetInstanceProcAddr(g_instance, "vkGetDeviceProcAddr");
    LOAD_DEV(vkCreateBuffer);
    LOAD_DEV(vkDestroyBuffer);
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 4096,
        .usage = 0,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buf = VK_NULL_HANDLE;
    VkResult r = vkCreateBuffer(g_device, &bci, NULL, &buf);
    if (r == VK_SUCCESS) {
        INCOMPLETE("wrapper_usage_zero_strictness",
                   "wrapper accepted usage=0 (lax); _fold test is therefore ambiguous as shim-correctness signal");
        if (buf != VK_NULL_HANDLE) vkDestroyBuffer(g_device, buf, NULL);
    } else {
        PASS("wrapper_usage_zero_strictness",
             "wrapper rejects usage=0 (%s) — strictness confirms _fold test is meaningful",
             vkresult_str(r));
    }
}

/* DXVK 2.7+ passes VkBufferUsageFlags2CreateInfoKHR via VkBufferCreateInfo.pNext
 * to extend the usage bitmask. The shim must fold flags2 bits into
 * v1.usage so the wrapper (which doesn't recognize the new sType) still
 * sees the intended bits.
 *
 * Detection: dlsym shim_m5_buffer_flags2_fold_count from the .so
 * loaded as WRAPPER_TESTBENCH_LIB. The shim increments it whenever a
 * fold actually happens. PASS only if the counter advanced — the buffer
 * being created successfully is not a sufficient signal (the wrapper
 * is lax about usage=0; see test_wrapper_usage_zero_strictness). */
static void test_maintenance5_buffer_usage_flags2_fold(void) {
    if (!maintenance5_present()) { SKIP("maintenance5_buffer_usage_flags2_fold", "extension not present"); return; }
    PFN_vkGetDeviceProcAddr pfn_GetDeviceProcAddr =
        (PFN_vkGetDeviceProcAddr)g_vkGetInstanceProcAddr(g_instance, "vkGetDeviceProcAddr");
    LOAD_DEV(vkCreateBuffer);
    LOAD_DEV(vkDestroyBuffer);

    volatile int *fold_counter = (volatile int *)dlsym(g_lib, "shim_m5_buffer_flags2_fold_count");
    int before = fold_counter ? *fold_counter : -1;

    VkBufferUsageFlags2CreateInfoKHR f2 = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR,
        .pNext = NULL,
        .usage = VK_BUFFER_USAGE_2_TRANSFER_SRC_BIT_KHR | VK_BUFFER_USAGE_2_TRANSFER_DST_BIT_KHR,
    };
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = &f2,
        .size = 4096,
        .usage = 0, /* deliberate: usage comes from flags2 only */
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buf = VK_NULL_HANDLE;
    VkResult r = vkCreateBuffer(g_device, &bci, NULL, &buf);
    if (buf != VK_NULL_HANDLE) vkDestroyBuffer(g_device, buf, NULL);

    if (r != VK_SUCCESS) {
        FAIL("maintenance5_buffer_usage_flags2_fold",
             "vkCreateBuffer(usage=0,flags2={SRC|DST}): %s", vkresult_str(r));
        return;
    }
    if (!fold_counter) {
        INCOMPLETE("maintenance5_buffer_usage_flags2_fold",
                   "create succeeded but shim fold-counter symbol not found (cannot prove fold fired)");
        return;
    }
    int after = *fold_counter;
    if (after > before)
        PASS("maintenance5_buffer_usage_flags2_fold",
             "shim folded flags2 into v1.usage (counter %d→%d) and create succeeded", before, after);
    else
        FAIL("maintenance5_buffer_usage_flags2_fold",
             "create succeeded but shim fold-counter did NOT advance (counter stuck at %d) — fold path not taken", before);
}

/* ----- harness ----- */

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("=== wrapper_testbench (leegao bionic-vulkan-wrapper direct harness) ===\n");
    if (setup_vulkan() != 0) {
        printf("=== %d passed, %d failed (setup blocked) ===\n", g_pass, g_fail);
        return g_fail;
    }
    test_enum_extensions();
    test_dxvk2_extensions_present();
    test_buffer_alloc_smoke();
    test_maintenance5_dispatch();
    test_maintenance5_record();
    test_maintenance5_bounded_size();
    test_maintenance5_index_uint8();
    test_maintenance5_granularity();
    test_maintenance5_isl2_agreement();
    test_maintenance5_dev_isl_stub();
    test_maintenance5_features2();
    test_maintenance5_properties2();
    test_robustness2_features2();
    test_robustness2_properties2();
    test_wrapper_usage_zero_strictness();
    test_maintenance5_buffer_usage_flags2_fold();
    test_null_subst_uniform_buffer();
    test_null_subst_sampled_image();
    test_null_subst_storage_buffer_incomplete();
    test_null_subst_via_template();
    test_spirv_hook_fires();
    test_spirv_pass_identifies_descriptor_loads();
    test_spirv_pass_injects_metadata_binding();
    test_spirv_pass_clamps_descriptor_loads();
    test_spirv_pass_clamps_image_reads();
    test_spirv_pass_clamps_image_fetches();
    test_mali_oob_ssbo_probe();
    test_mali_oob_storage_image_probe();
    printf("=== %d passed, %d failed, %d incomplete, %d skipped ===\n",
           g_pass, g_fail, g_inc, g_skip);
    if (g_inc > 0)
        printf("    NOTE: incomplete results indicate stubs / partial impls — must not be confused with PASS\n");
    return g_fail;
}
