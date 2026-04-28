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
    printf("=== %d passed, %d failed, %d incomplete, %d skipped ===\n",
           g_pass, g_fail, g_inc, g_skip);
    if (g_inc > 0)
        printf("    NOTE: incomplete results indicate stubs / partial impls — must not be confused with PASS\n");
    return g_fail;
}
