/*
 * wrapper_testbench — direct-against-leegao Vulkan harness.
 *
 * Goal: iterate on extension shims in bionic-vulkan-wrapper without
 * launching a game, wine, or DXVK. Sub-second feedback loop.
 *
 * Each test prints `[PASS] name: detail` or `[FAIL] name: detail`. main
 * runs every registered test and exits with the failure count as the
 * status code. Add a new extension by adding a new test_*() function
 * and one line in main's table.
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

#define PASS(name, fmt, ...) do { ++g_pass; printf("[PASS] %s: " fmt "\n", name, ##__VA_ARGS__); } while (0)
#define FAIL(name, fmt, ...) do { ++g_fail; printf("[FAIL] %s: " fmt "\n", name, ##__VA_ARGS__); } while (0)

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
    if (present == 4) PASS("dxvk2_extensions_present", "all 4 present (DXVK 2.x ready)");
    else if (present == 0) FAIL("dxvk2_extensions_present", "0/4 present (current baseline)");
    else PASS("dxvk2_extensions_present", "%d/4 present (partial)", present);
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
        printf("[SKIP] maintenance5_dispatch: extension not present (expected on current wrapper)\n");
        return;
    }
    PFN_vkGetDeviceProcAddr pfn_GetDeviceProcAddr =
        (PFN_vkGetDeviceProcAddr)g_vkGetInstanceProcAddr(g_instance, "vkGetDeviceProcAddr");
    void *bind2     = (void *)pfn_GetDeviceProcAddr(g_device, "vkCmdBindIndexBuffer2KHR");
    void *gran      = (void *)pfn_GetDeviceProcAddr(g_device, "vkGetRenderingAreaGranularityKHR");
    void *isl2      = (void *)pfn_GetDeviceProcAddr(g_device, "vkGetImageSubresourceLayout2KHR");
    void *dev_isl   = (void *)pfn_GetDeviceProcAddr(g_device, "vkGetDeviceImageSubresourceLayoutKHR");
    int resolved = (bind2 ? 1 : 0) + (gran ? 1 : 0) + (isl2 ? 1 : 0) + (dev_isl ? 1 : 0);
    if (resolved == 4) PASS("maintenance5_dispatch", "all 4 entrypoints resolved");
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
        printf("[SKIP] maintenance5_record: extension not present\n");
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
    if (r == VK_SUCCESS) PASS("maintenance5_record", "vkCmdBindIndexBuffer2KHR recorded + End ok");
    else                 FAIL("maintenance5_record", "vkEndCommandBuffer: %s", vkresult_str(r));

    vkDestroyCommandPool(g_device, pool, NULL);
    vkFreeMemory(g_device, mem, NULL);
    vkDestroyBuffer(g_device, buf, NULL);
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
    printf("=== %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail;
}
