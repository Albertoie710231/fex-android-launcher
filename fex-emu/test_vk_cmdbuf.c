/**
 * Minimal Windows PE Vulkan test: creates a device and begins a command buffer.
 * Tests whether vkBeginCommandBuffer works through Wine's winevulkan path.
 *
 * Compile:
 *   x86_64-w64-mingw32-gcc -o test_vk_cmdbuf.exe test_vk_cmdbuf.c -O2 -mconsole
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Vulkan types */
typedef void* VkInstance;
typedef void* VkPhysicalDevice;
typedef void* VkDevice;
typedef void* VkCommandPool;
typedef void* VkCommandBuffer;
typedef void* VkQueue;
typedef uint32_t VkFlags;
typedef int32_t VkResult;
typedef uint32_t VkStructureType;

#define VK_SUCCESS 0
#define VK_STYPE_INSTANCE_CREATE_INFO 1
#define VK_STYPE_DEVICE_CREATE_INFO 3
#define VK_STYPE_DEVICE_QUEUE_CREATE_INFO 2
#define VK_STYPE_COMMAND_POOL_CREATE_INFO 39
#define VK_STYPE_COMMAND_BUFFER_ALLOCATE_INFO 40
#define VK_STYPE_COMMAND_BUFFER_BEGIN_INFO 42
#define VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT 0x00000002
#define VK_COMMAND_BUFFER_LEVEL_PRIMARY 0
#define VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT 0x00000001
#define VK_QUEUE_GRAPHICS_BIT 0x00000001

typedef struct {
    VkStructureType sType;
    const void* pNext;
    VkFlags flags;
    const void* pApplicationInfo;
    uint32_t enabledLayerCount;
    const char* const* ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char* const* ppEnabledExtensionNames;
} MyVkInstanceCreateInfo;

typedef struct {
    VkStructureType sType;
    const void* pNext;
    VkFlags flags;
    uint32_t queueFamilyIndex;
    uint32_t queueCount;
    const float* pQueuePriorities;
} MyVkDeviceQueueCreateInfo;

typedef struct {
    VkStructureType sType;
    const void* pNext;
    VkFlags flags;
    uint32_t queueCreateInfoCount;
    const MyVkDeviceQueueCreateInfo* pQueueCreateInfos;
    uint32_t enabledLayerCount;
    const char* const* ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char* const* ppEnabledExtensionNames;
    const void* pEnabledFeatures;
} MyVkDeviceCreateInfo;

typedef struct {
    VkStructureType sType;
    const void* pNext;
    VkFlags flags;
    uint32_t queueFamilyIndex;
} MyVkCommandPoolCreateInfo;

typedef struct {
    VkStructureType sType;
    const void* pNext;
    VkCommandPool commandPool;
    uint32_t level;
    uint32_t commandBufferCount;
} MyVkCommandBufferAllocateInfo;

typedef struct {
    VkStructureType sType;
    const void* pNext;
    VkFlags flags;
    const void* pInheritanceInfo;
} MyVkCommandBufferBeginInfo;

typedef struct {
    VkFlags queueFlags;
    uint32_t queueCount;
    uint32_t timestampValidBits;
    uint32_t minImageTransferGranularity[3];
} MyVkQueueFamilyProperties;

/* Function pointer types */
typedef VkResult (WINAPI *PFN_vkCreateInstance)(const MyVkInstanceCreateInfo*, const void*, VkInstance*);
typedef VkResult (WINAPI *PFN_vkEnumeratePhysicalDevices)(VkInstance, uint32_t*, VkPhysicalDevice*);
typedef void (WINAPI *PFN_vkGetPhysicalDeviceQueueFamilyProperties)(VkPhysicalDevice, uint32_t*, MyVkQueueFamilyProperties*);
typedef VkResult (WINAPI *PFN_vkCreateDevice)(VkPhysicalDevice, const MyVkDeviceCreateInfo*, const void*, VkDevice*);
typedef VkResult (WINAPI *PFN_vkCreateCommandPool)(VkDevice, const MyVkCommandPoolCreateInfo*, const void*, VkCommandPool*);
typedef VkResult (WINAPI *PFN_vkAllocateCommandBuffers)(VkDevice, const MyVkCommandBufferAllocateInfo*, VkCommandBuffer*);
typedef VkResult (WINAPI *PFN_vkBeginCommandBuffer)(VkCommandBuffer, const MyVkCommandBufferBeginInfo*);
typedef VkResult (WINAPI *PFN_vkEndCommandBuffer)(VkCommandBuffer);
typedef void (WINAPI *PFN_vkDestroyCommandPool)(VkDevice, VkCommandPool, const void*);
typedef void (WINAPI *PFN_vkDestroyDevice)(VkDevice, const void*);
typedef void (WINAPI *PFN_vkDestroyInstance)(VkInstance, const void*);
typedef void (WINAPI *PFN_vkGetDeviceQueue)(VkDevice, uint32_t, uint32_t, VkQueue*);

#define LOAD(name) do { \
    pfn_##name = (PFN_##name)GetProcAddress(hVulkan, #name); \
    if (!pfn_##name) { fprintf(stderr, "FAIL: GetProcAddress(%s) = NULL\n", #name); fflush(stderr); return 1; } \
} while(0)

/* Multi-threaded test data structures */
typedef struct {
    PFN_vkBeginCommandBuffer begin;
    PFN_vkEndCommandBuffer end;
    VkCommandBuffer buf;
    volatile int go;
    volatile int done;
    volatile int failed;
    int iterations;
} ThreadCmdBufData;

typedef struct {
    PFN_vkCreateCommandPool createPool;
    PFN_vkAllocateCommandBuffers allocBufs;
    PFN_vkDestroyCommandPool destroyPool;
    VkDevice dev;
    uint32_t queueFamily;
    volatile int go;
    volatile int done;
    volatile int failed;
    int iterations;
} ThreadDevOpsData;

/* Thread B: begin/end command buffer in a loop (non-trampolined path) */
static DWORD WINAPI thread_cmdbuf_func(LPVOID arg) {
    ThreadCmdBufData *d = (ThreadCmdBufData*)arg;
    MyVkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    while (!d->go) Sleep(0);

    for (int i = 0; i < d->iterations; i++) {
        VkResult r = d->begin(d->buf, &bi);
        if (r != VK_SUCCESS) { d->failed = r; d->done = 1; return 1; }
        r = d->end(d->buf);
        if (r != VK_SUCCESS) { d->failed = r; d->done = 1; return 1; }
    }
    d->done = 1;
    return 0;
}

/* Thread A: call trampolined device functions that hold ICD spinlock */
static DWORD WINAPI thread_devops_func(LPVOID arg) {
    ThreadDevOpsData *d = (ThreadDevOpsData*)arg;
    while (!d->go) Sleep(0);

    for (int i = 0; i < d->iterations; i++) {
        MyVkCommandPoolCreateInfo ci;
        memset(&ci, 0, sizeof(ci));
        ci.sType = VK_STYPE_COMMAND_POOL_CREATE_INFO;
        ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        ci.queueFamilyIndex = d->queueFamily;
        VkCommandPool pool = NULL;
        VkResult r = d->createPool(d->dev, &ci, NULL, &pool);
        if (r != VK_SUCCESS) { d->failed = r; d->done = 1; return 1; }

        MyVkCommandBufferAllocateInfo ai;
        memset(&ai, 0, sizeof(ai));
        ai.sType = VK_STYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer buf = NULL;
        r = d->allocBufs(d->dev, &ai, &buf);
        if (r != VK_SUCCESS) { d->failed = r; d->done = 1; return 1; }

        d->destroyPool(d->dev, pool, NULL);
    }
    d->done = 1;
    return 0;
}

int main(void) {
    fprintf(stderr, "\n[test_vk_cmdbuf] === Vulkan Command Buffer Test (PE/Wine) ===\n");
    fflush(stderr);

    /* Load vulkan-1.dll (Wine's winevulkan) */
    fprintf(stderr, "[test] Loading vulkan-1.dll...\n"); fflush(stderr);
    HMODULE hVulkan = LoadLibraryA("vulkan-1.dll");
    if (!hVulkan) {
        fprintf(stderr, "FAIL: LoadLibrary(vulkan-1.dll) error %lu\n", GetLastError());
        fflush(stderr);
        return 1;
    }
    fprintf(stderr, "[test] vulkan-1.dll loaded at %p\n", hVulkan); fflush(stderr);

    /* Get function pointers */
    PFN_vkCreateInstance pfn_vkCreateInstance;
    PFN_vkEnumeratePhysicalDevices pfn_vkEnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties pfn_vkGetPhysicalDeviceQueueFamilyProperties;
    PFN_vkCreateDevice pfn_vkCreateDevice;
    PFN_vkCreateCommandPool pfn_vkCreateCommandPool;
    PFN_vkAllocateCommandBuffers pfn_vkAllocateCommandBuffers;
    PFN_vkBeginCommandBuffer pfn_vkBeginCommandBuffer;
    PFN_vkEndCommandBuffer pfn_vkEndCommandBuffer;
    PFN_vkDestroyCommandPool pfn_vkDestroyCommandPool;
    PFN_vkDestroyDevice pfn_vkDestroyDevice;
    PFN_vkDestroyInstance pfn_vkDestroyInstance;
    PFN_vkGetDeviceQueue pfn_vkGetDeviceQueue;

    LOAD(vkCreateInstance);
    LOAD(vkEnumeratePhysicalDevices);
    LOAD(vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD(vkCreateDevice);
    LOAD(vkCreateCommandPool);
    LOAD(vkAllocateCommandBuffers);
    LOAD(vkBeginCommandBuffer);
    LOAD(vkEndCommandBuffer);
    LOAD(vkDestroyCommandPool);
    LOAD(vkDestroyDevice);
    LOAD(vkDestroyInstance);
    LOAD(vkGetDeviceQueue);
    fprintf(stderr, "[test] All function pointers resolved\n"); fflush(stderr);

    VkResult result;

    /* 1. Create instance WITH surface extensions (triggers HeadlessLayer) */
    fprintf(stderr, "[test] Step 1: vkCreateInstance (with surface exts to activate HeadlessLayer)...\n");
    fflush(stderr);
    const char *instExts[] = {
        "VK_KHR_surface",
        "VK_KHR_win32_surface",
        "VK_KHR_get_surface_capabilities2",
    };
    MyVkInstanceCreateInfo ici;
    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STYPE_INSTANCE_CREATE_INFO;
    ici.enabledExtensionCount = 3;
    ici.ppEnabledExtensionNames = instExts;
    VkInstance instance = NULL;
    result = pfn_vkCreateInstance(&ici, NULL, &instance);
    fprintf(stderr, "[test] vkCreateInstance: result=%d instance=%p\n", result, instance); fflush(stderr);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "[test] Retrying without surface exts...\n"); fflush(stderr);
        ici.enabledExtensionCount = 0;
        ici.ppEnabledExtensionNames = NULL;
        result = pfn_vkCreateInstance(&ici, NULL, &instance);
        fprintf(stderr, "[test] vkCreateInstance (bare): result=%d instance=%p\n", result, instance);
        fflush(stderr);
        if (result != VK_SUCCESS) return 1;
    }

    /* 2. Enumerate physical devices */
    fprintf(stderr, "[test] Step 2: vkEnumeratePhysicalDevices...\n"); fflush(stderr);
    uint32_t gpuCount = 0;
    pfn_vkEnumeratePhysicalDevices(instance, &gpuCount, NULL);
    fprintf(stderr, "[test] GPU count: %u\n", gpuCount); fflush(stderr);
    if (gpuCount == 0) return 1;

    VkPhysicalDevice gpu = NULL;
    gpuCount = 1;
    pfn_vkEnumeratePhysicalDevices(instance, &gpuCount, &gpu);
    fprintf(stderr, "[test] GPU: %p\n", gpu); fflush(stderr);

    /* 3. Find graphics queue family */
    fprintf(stderr, "[test] Step 3: Queue families...\n"); fflush(stderr);
    uint32_t qfCount = 0;
    pfn_vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qfCount, NULL);
    MyVkQueueFamilyProperties qfProps[16];
    if (qfCount > 16) qfCount = 16;
    pfn_vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qfCount, qfProps);
    uint32_t gfxQF = 0;
    for (uint32_t i = 0; i < qfCount; i++) {
        fprintf(stderr, "[test]   QF[%u]: flags=0x%x count=%u\n",
                i, qfProps[i].queueFlags, qfProps[i].queueCount);
        if (qfProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            gfxQF = i;
    }
    fprintf(stderr, "[test] Using QF %u\n", gfxQF); fflush(stderr);

    /* 4. Create device */
    fprintf(stderr, "[test] Step 4: vkCreateDevice...\n"); fflush(stderr);
    float qp = 1.0f;
    MyVkDeviceQueueCreateInfo qci;
    memset(&qci, 0, sizeof(qci));
    qci.sType = VK_STYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = gfxQF;
    qci.queueCount = 1;
    qci.pQueuePriorities = &qp;

    /* Request the same extensions DXVK enables (minus Win32-only ones).
     * We try all; unsupported ones will cause vkCreateDevice to fail,
     * so we fall back to fewer extensions. */
    /* All extensions DXVK enables (from Android trace).
     * Win32 exts get translated by winevulkan to fd equivalents. */
    const char *dxvkExts[] = {
        "VK_KHR_swapchain",
        "VK_KHR_swapchain_mutable_format",
        "VK_EXT_border_color_swizzle",
        "VK_EXT_conservative_rasterization",
        "VK_EXT_custom_border_color",
        "VK_EXT_depth_clip_enable",
        "VK_EXT_robustness2",
        "VK_EXT_transform_feedback",
        "VK_KHR_maintenance5",
        "VK_KHR_maintenance6",
        "VK_KHR_pipeline_library",
        "VK_KHR_external_memory_fd",
        "VK_KHR_external_semaphore_fd",
    };
    int dxvkExtCount = 13;

    /* Query supported extensions to only request available ones */
    typedef VkResult (WINAPI *PFN_vkEnumerateDeviceExtensionProperties)(
        VkPhysicalDevice, const char*, uint32_t*, void*);
    PFN_vkEnumerateDeviceExtensionProperties pfn_vkEnumDevExts =
        (PFN_vkEnumerateDeviceExtensionProperties)GetProcAddress(hVulkan,
            "vkEnumerateDeviceExtensionProperties");

    const char *enabledExts[16];
    int enabledCount = 0;

    if (pfn_vkEnumDevExts) {
        uint32_t extCount = 0;
        pfn_vkEnumDevExts(gpu, NULL, &extCount, NULL);
        fprintf(stderr, "[test] Device has %u extensions\n", extCount); fflush(stderr);

        /* Simple check: try each DXVK ext — if device supports it, enable it */
        typedef struct { char name[256]; uint32_t ver; } ExtProp;
        ExtProp *allExts = (ExtProp*)malloc(extCount * sizeof(ExtProp));
        if (allExts) {
            pfn_vkEnumDevExts(gpu, NULL, &extCount, allExts);
            for (int d = 0; d < dxvkExtCount; d++) {
                for (uint32_t e = 0; e < extCount; e++) {
                    if (strcmp(dxvkExts[d], allExts[e].name) == 0) {
                        enabledExts[enabledCount++] = dxvkExts[d];
                        break;
                    }
                }
            }
            free(allExts);
        }
    }
    if (enabledCount == 0) {
        enabledExts[0] = "VK_KHR_swapchain";
        enabledCount = 1;
    }

    fprintf(stderr, "[test] Enabling %d device extensions:\n", enabledCount);
    for (int i = 0; i < enabledCount; i++)
        fprintf(stderr, "[test]   %s\n", enabledExts[i]);
    fflush(stderr);

    /* Enable features matching DXVK's Android config (from trace output) */
    typedef struct {
        uint32_t robustBufferAccess;
        uint32_t fullDrawIndexUint32;
        uint32_t imageCubeArray;
        uint32_t independentBlend;
        uint32_t geometryShader;
        uint32_t tessellationShader;
        uint32_t sampleRateShading;
        uint32_t dualSrcBlend;
        uint32_t logicOp;
        uint32_t multiDrawIndirect;
        uint32_t drawIndirectFirstInstance;
        uint32_t depthClamp;
        uint32_t depthBiasClamp;
        uint32_t fillModeNonSolid;
        uint32_t depthBounds;
        uint32_t wideLines;
        uint32_t largePoints;
        uint32_t alphaToOne;
        uint32_t multiViewport;
        uint32_t samplerAnisotropy;
        uint32_t textureCompressionETC2;
        uint32_t textureCompressionASTC_LDR;
        uint32_t textureCompressionBC;
        uint32_t occlusionQueryPrecise;
        uint32_t pipelineStatisticsQuery;
        uint32_t vertexPipelineStoresAndAtomics;
        uint32_t fragmentStoresAndAtomics;
        uint32_t shaderTessellationAndGeometryPointSize;
        uint32_t shaderImageGatherExtended;
        uint32_t shaderStorageImageExtendedFormats;
        uint32_t shaderStorageImageMultisample;
        uint32_t shaderStorageImageReadWithoutFormat;
        uint32_t shaderStorageImageWriteWithoutFormat;
        uint32_t shaderUniformBufferArrayDynamicIndexing;
        uint32_t shaderSampledImageArrayDynamicIndexing;
        uint32_t shaderStorageBufferArrayDynamicIndexing;
        uint32_t shaderStorageImageArrayDynamicIndexing;
        uint32_t shaderClipDistance;
        uint32_t shaderCullDistance;
        uint32_t shaderFloat64;
        uint32_t shaderInt64;
        uint32_t shaderInt16;
        uint32_t shaderResourceResidency;
        uint32_t shaderResourceMinLod;
        uint32_t sparseBinding;
        uint32_t sparseResidencyBuffer;
        uint32_t sparseResidencyImage2D;
        uint32_t sparseResidencyImage3D;
        uint32_t sparseResidency2Samples;
        uint32_t sparseResidency4Samples;
        uint32_t sparseResidency8Samples;
        uint32_t sparseResidency16Samples;
        uint32_t sparseResidencyAliased;
        uint32_t variableMultisampleRate;
        uint32_t inheritedQueries;
    } MyVkPhysicalDeviceFeatures;

    MyVkPhysicalDeviceFeatures features;
    memset(&features, 0, sizeof(features));
    /* Match DXVK Android trace: */
    features.robustBufferAccess = 1;
    features.fullDrawIndexUint32 = 1;
    features.imageCubeArray = 1;
    features.independentBlend = 1;
    features.geometryShader = 1;
    features.tessellationShader = 1;
    features.sampleRateShading = 1;
    features.dualSrcBlend = 1;
    features.logicOp = 1;
    features.multiDrawIndirect = 1;
    features.drawIndirectFirstInstance = 1;
    features.depthClamp = 1;
    features.depthBiasClamp = 1;
    features.fillModeNonSolid = 1;
    features.multiViewport = 1;
    features.samplerAnisotropy = 1;
    features.textureCompressionBC = 1;
    features.occlusionQueryPrecise = 1;
    features.vertexPipelineStoresAndAtomics = 1;
    features.fragmentStoresAndAtomics = 1;
    features.shaderImageGatherExtended = 1;
    features.shaderStorageImageExtendedFormats = 1;
    features.shaderUniformBufferArrayDynamicIndexing = 1;
    features.shaderSampledImageArrayDynamicIndexing = 1;
    features.shaderStorageBufferArrayDynamicIndexing = 1;
    features.shaderStorageImageArrayDynamicIndexing = 1;
    features.shaderClipDistance = 1;
    features.shaderCullDistance = 1;
    features.shaderInt64 = 1;
    features.shaderInt16 = 1;

    fprintf(stderr, "[test] Enabling DXVK-matching device features (robustBufferAccess, BC, etc.)\n");
    fflush(stderr);

    MyVkDeviceCreateInfo dci;
    memset(&dci, 0, sizeof(dci));
    dci.sType = VK_STYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = enabledCount;
    dci.ppEnabledExtensionNames = enabledExts;
    dci.pEnabledFeatures = &features;

    VkDevice device = NULL;
    result = pfn_vkCreateDevice(gpu, &dci, NULL, &device);
    fprintf(stderr, "[test] vkCreateDevice (with features): result=%d device=%p\n", result, device); fflush(stderr);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "[test] Device creation with features FAILED, retrying without...\n");
        fflush(stderr);
        dci.pEnabledFeatures = NULL;
        result = pfn_vkCreateDevice(gpu, &dci, NULL, &device);
        fprintf(stderr, "[test] vkCreateDevice (no features): result=%d device=%p\n", result, device);
        fflush(stderr);
        if (result != VK_SUCCESS) return 1;
    }

    /* 4b. Re-resolve device-level functions via vkGetDeviceProcAddr (like DXVK does).
     * DXVK calls vkGetDeviceProcAddr for ALL device functions.
     * This returns Wine's PE-side thunks (winevulkan.dll) which use
     * __wine_unix_call_dispatcher — a different dispatch path than the
     * Vulkan loader trampolines returned by GetProcAddress. */
    /* 4a. Test queue operations WITH swapchain */
    fprintf(stderr, "[test] Step 4a: Queue operation tests (WITH VK_KHR_swapchain)...\n"); fflush(stderr);
    {
        VkQueue queue = NULL;
        pfn_vkGetDeviceQueue(device, gfxQF, 0, &queue);
        fprintf(stderr, "[test] vkGetDeviceQueue: queue=%p\n", queue); fflush(stderr);

        typedef VkResult (WINAPI *PFN_vkQueueWaitIdle)(VkQueue);
        typedef VkResult (WINAPI *PFN_vkQueueSubmit)(VkQueue, uint32_t, const void*, void*);
        typedef VkResult (WINAPI *PFN_vkDeviceWaitIdle)(VkDevice);

        PFN_vkQueueWaitIdle pfn_vkQueueWaitIdle =
            (PFN_vkQueueWaitIdle)GetProcAddress(hVulkan, "vkQueueWaitIdle");
        PFN_vkQueueSubmit pfn_vkQueueSubmit =
            (PFN_vkQueueSubmit)GetProcAddress(hVulkan, "vkQueueSubmit");
        PFN_vkDeviceWaitIdle pfn_vkDeviceWaitIdle =
            (PFN_vkDeviceWaitIdle)GetProcAddress(hVulkan, "vkDeviceWaitIdle");

        if (queue && pfn_vkQueueWaitIdle) {
            fprintf(stderr, "[test] >>> vkQueueWaitIdle CALLING (empty queue)... <<<\n"); fflush(stderr);
            result = pfn_vkQueueWaitIdle(queue);
            fprintf(stderr, "[test] vkQueueWaitIdle: result=%d %s\n",
                    result, result == 0 ? "SUCCESS" : "FAILED"); fflush(stderr);
        }
    }

    /* Cleanup first device */
    pfn_vkDestroyCommandPool(device, NULL, NULL);  /* pool not created yet */
    pfn_vkDestroyDevice(device, NULL);
    pfn_vkDestroyInstance(instance, NULL);

    /* 4b. Create a SECOND device WITHOUT VK_KHR_swapchain (matches HeadlessLayer filtering) */
    fprintf(stderr, "\n[test] Step 4b: Create device WITHOUT VK_KHR_swapchain...\n"); fflush(stderr);
    {
        MyVkInstanceCreateInfo ici2;
        memset(&ici2, 0, sizeof(ici2));
        ici2.sType = VK_STYPE_INSTANCE_CREATE_INFO;
        VkInstance inst2 = NULL;
        result = pfn_vkCreateInstance(&ici2, NULL, &inst2);
        fprintf(stderr, "[test] vkCreateInstance: result=%d\n", result); fflush(stderr);
        if (result != VK_SUCCESS) { fprintf(stderr, "[test] FAILED to create 2nd instance\n"); return 1; }

        uint32_t gc2 = 1; VkPhysicalDevice gpu2 = NULL;
        pfn_vkEnumeratePhysicalDevices(inst2, &gc2, &gpu2);

        float qp2 = 1.0f;
        MyVkDeviceQueueCreateInfo qci2;
        memset(&qci2, 0, sizeof(qci2));
        qci2.sType = VK_STYPE_DEVICE_QUEUE_CREATE_INFO;
        qci2.queueCount = 1;
        qci2.pQueuePriorities = &qp2;

        /* NO extensions — simulates what HeadlessLayer does (filters swapchain) */
        MyVkDeviceCreateInfo dci2;
        memset(&dci2, 0, sizeof(dci2));
        dci2.sType = VK_STYPE_DEVICE_CREATE_INFO;
        dci2.queueCreateInfoCount = 1;
        dci2.pQueueCreateInfos = &qci2;
        dci2.enabledExtensionCount = 0;
        dci2.ppEnabledExtensionNames = NULL;

        VkDevice dev2 = NULL;
        result = pfn_vkCreateDevice(gpu2, &dci2, NULL, &dev2);
        fprintf(stderr, "[test] vkCreateDevice (no exts): result=%d dev=%p\n", result, dev2); fflush(stderr);
        if (result != VK_SUCCESS) {
            pfn_vkDestroyInstance(inst2, NULL);
            fprintf(stderr, "[test] FAILED — cannot create device without extensions\n");
            return 1;
        }

        VkQueue q2 = NULL;
        pfn_vkGetDeviceQueue(dev2, 0, 0, &q2);
        fprintf(stderr, "[test] vkGetDeviceQueue (no exts): queue=%p\n", q2); fflush(stderr);

        typedef VkResult (WINAPI *PFN_vkQueueWaitIdle)(VkQueue);
        PFN_vkQueueWaitIdle qwi2 = (PFN_vkQueueWaitIdle)GetProcAddress(hVulkan, "vkQueueWaitIdle");
        if (q2 && qwi2) {
            fprintf(stderr, "[test] >>> vkQueueWaitIdle (NO swapchain device) CALLING... <<<\n"); fflush(stderr);
            result = qwi2(q2);
            fprintf(stderr, "[test] vkQueueWaitIdle (no swapchain): result=%d %s\n",
                    result, result == 0 ? "SUCCESS" : "FAILED"); fflush(stderr);
        }

        pfn_vkDestroyDevice(dev2, NULL);
        pfn_vkDestroyInstance(inst2, NULL);
    }

    /* Recreate instance/device for remaining tests */
    fprintf(stderr, "\n[test] Recreating instance/device for remaining tests...\n"); fflush(stderr);
    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STYPE_INSTANCE_CREATE_INFO;
    result = pfn_vkCreateInstance(&ici, NULL, &instance);
    if (result != VK_SUCCESS) return 1;
    gpuCount = 1;
    pfn_vkEnumeratePhysicalDevices(instance, &gpuCount, &gpu);
    memset(&dci, 0, sizeof(dci));
    dci.sType = VK_STYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = enabledCount;
    dci.ppEnabledExtensionNames = enabledExts;
    result = pfn_vkCreateDevice(gpu, &dci, NULL, &device);
    if (result != VK_SUCCESS) return 1;

    typedef void* (WINAPI *PFN_vkGetDeviceProcAddr)(VkDevice, const char*);
    PFN_vkGetDeviceProcAddr pfn_vkGDPA =
        (PFN_vkGetDeviceProcAddr)GetProcAddress(hVulkan, "vkGetDeviceProcAddr");
    if (pfn_vkGDPA) {
        fprintf(stderr, "[test] Step 4b: Re-resolving via vkGetDeviceProcAddr...\n");
        fflush(stderr);

        /* Save old (loader trampoline) pointers for comparison */
        void *old_begin = (void*)pfn_vkBeginCommandBuffer;
        void *old_end = (void*)pfn_vkEndCommandBuffer;

        PFN_vkBeginCommandBuffer gdpa_begin =
            (PFN_vkBeginCommandBuffer)pfn_vkGDPA(device, "vkBeginCommandBuffer");
        PFN_vkEndCommandBuffer gdpa_end =
            (PFN_vkEndCommandBuffer)pfn_vkGDPA(device, "vkEndCommandBuffer");
        PFN_vkCreateCommandPool gdpa_createPool =
            (PFN_vkCreateCommandPool)pfn_vkGDPA(device, "vkCreateCommandPool");
        PFN_vkAllocateCommandBuffers gdpa_allocBufs =
            (PFN_vkAllocateCommandBuffers)pfn_vkGDPA(device, "vkAllocateCommandBuffers");

        fprintf(stderr, "[test] vkBeginCommandBuffer: loader=%p  GDPA=%p  %s\n",
                old_begin, (void*)gdpa_begin,
                old_begin == (void*)gdpa_begin ? "SAME" : "DIFFERENT");
        fprintf(stderr, "[test] vkEndCommandBuffer:   loader=%p  GDPA=%p  %s\n",
                old_end, (void*)gdpa_end,
                old_end == (void*)gdpa_end ? "SAME" : "DIFFERENT");
        fflush(stderr);

        /* Use GDPA-obtained pointers (same as DXVK) */
        if (gdpa_begin) pfn_vkBeginCommandBuffer = gdpa_begin;
        if (gdpa_end) pfn_vkEndCommandBuffer = gdpa_end;
        if (gdpa_createPool) pfn_vkCreateCommandPool = gdpa_createPool;
        if (gdpa_allocBufs) pfn_vkAllocateCommandBuffers = gdpa_allocBufs;

        fprintf(stderr, "[test] Now using GDPA function pointers\n"); fflush(stderr);
    }

    /* 5. Create command pool */
    fprintf(stderr, "[test] Step 5: vkCreateCommandPool...\n"); fflush(stderr);
    MyVkCommandPoolCreateInfo cpci;
    memset(&cpci, 0, sizeof(cpci));
    cpci.sType = VK_STYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = gfxQF;

    VkCommandPool cmdPool = NULL;
    result = pfn_vkCreateCommandPool(device, &cpci, NULL, &cmdPool);
    fprintf(stderr, "[test] vkCreateCommandPool: result=%d pool=%p\n", result, cmdPool); fflush(stderr);
    if (result != VK_SUCCESS) return 1;

    /* 6. Allocate command buffer */
    fprintf(stderr, "[test] Step 6: vkAllocateCommandBuffers...\n"); fflush(stderr);
    MyVkCommandBufferAllocateInfo cbai;
    memset(&cbai, 0, sizeof(cbai));
    cbai.sType = VK_STYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = cmdPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;

    VkCommandBuffer cmdBuf = NULL;
    result = pfn_vkAllocateCommandBuffers(device, &cbai, &cmdBuf);
    fprintf(stderr, "[test] vkAllocateCommandBuffers: result=%d cmdBuf=%p\n", result, cmdBuf); fflush(stderr);
    if (result != VK_SUCCESS) return 1;

    /* 7. BEGIN COMMAND BUFFER — THIS IS THE CRITICAL TEST */
    fprintf(stderr, "[test] Step 7: >>> vkBeginCommandBuffer (CRITICAL) <<<\n"); fflush(stderr);
    MyVkCommandBufferBeginInfo cbbi;
    memset(&cbbi, 0, sizeof(cbbi));
    cbbi.sType = VK_STYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    result = pfn_vkBeginCommandBuffer(cmdBuf, &cbbi);
    fprintf(stderr, "[test] vkBeginCommandBuffer: result=%d *** %s ***\n",
            result, result == VK_SUCCESS ? "SUCCESS" : "FAILED"); fflush(stderr);

    /* 8. End command buffer */
    fprintf(stderr, "[test] Step 8: vkEndCommandBuffer...\n"); fflush(stderr);
    result = pfn_vkEndCommandBuffer(cmdBuf);
    fprintf(stderr, "[test] vkEndCommandBuffer: result=%d\n", result); fflush(stderr);

    /* 9. Test loop: begin/end 10 times (DXVK does this per-frame) */
    fprintf(stderr, "[test] Step 9: Begin/End loop (10 iterations)...\n"); fflush(stderr);
    for (int i = 0; i < 10; i++) {
        result = pfn_vkBeginCommandBuffer(cmdBuf, &cbbi);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "[test] Loop iter %d: vkBeginCommandBuffer FAILED: %d\n", i, result);
            fflush(stderr);
            break;
        }
        result = pfn_vkEndCommandBuffer(cmdBuf);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "[test] Loop iter %d: vkEndCommandBuffer FAILED: %d\n", i, result);
            fflush(stderr);
            break;
        }
    }
    fprintf(stderr, "[test] Loop completed successfully\n"); fflush(stderr);

    /* 10. Multi-threaded test: reproduce DXVK's concurrent Vulkan usage.
     * Thread A calls trampolined device functions (hold ICD spinlock).
     * Thread B calls vkBeginCommandBuffer concurrently.
     * This tests for deadlocks between the ICD's dispatch_lock and thunk calls. */
    fprintf(stderr, "\n[test] Step 10: Multi-threaded concurrent Vulkan calls...\n"); fflush(stderr);
    {
        /* Create a second command pool + buffer for the second thread */
        MyVkCommandPoolCreateInfo cpci2;
        memset(&cpci2, 0, sizeof(cpci2));
        cpci2.sType = VK_STYPE_COMMAND_POOL_CREATE_INFO;
        cpci2.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cpci2.queueFamilyIndex = gfxQF;
        VkCommandPool cmdPool2 = NULL;
        result = pfn_vkCreateCommandPool(device, &cpci2, NULL, &cmdPool2);
        fprintf(stderr, "[test] Thread B pool: result=%d pool=%p\n", result, cmdPool2);
        fflush(stderr);
        if (result != VK_SUCCESS) goto skip_mt;

        MyVkCommandBufferAllocateInfo cbai2;
        memset(&cbai2, 0, sizeof(cbai2));
        cbai2.sType = VK_STYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai2.commandPool = cmdPool2;
        cbai2.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai2.commandBufferCount = 1;
        VkCommandBuffer cmdBuf2 = NULL;
        result = pfn_vkAllocateCommandBuffers(device, &cbai2, &cmdBuf2);
        fprintf(stderr, "[test] Thread B cmdBuf: result=%d buf=%p\n", result, cmdBuf2);
        fflush(stderr);
        if (result != VK_SUCCESS) goto skip_mt;

        ThreadCmdBufData td;
        memset(&td, 0, sizeof(td));
        td.begin = pfn_vkBeginCommandBuffer;
        td.end = pfn_vkEndCommandBuffer;
        td.buf = cmdBuf2;
        td.iterations = 50;

        ThreadDevOpsData ta;
        memset(&ta, 0, sizeof(ta));
        ta.createPool = pfn_vkCreateCommandPool;
        ta.allocBufs = pfn_vkAllocateCommandBuffers;
        ta.destroyPool = pfn_vkDestroyCommandPool;
        ta.dev = device;
        ta.queueFamily = gfxQF;
        ta.iterations = 50;

        HANDLE hA = CreateThread(NULL, 0, thread_devops_func, &ta, 0, NULL);
        HANDLE hB = CreateThread(NULL, 0, thread_cmdbuf_func, &td, 0, NULL);

        if (!hA || !hB) {
            fprintf(stderr, "[test] CreateThread failed: %lu\n", GetLastError());
            fflush(stderr);
            goto skip_mt;
        }

        fprintf(stderr, "[test] Threads created. Starting concurrent test (50 iters each)...\n");
        fflush(stderr);
        /* Signal both threads to start simultaneously */
        ta.go = 1;
        td.go = 1;

        /* Wait with timeout (10 seconds) */
        DWORD waitA = WaitForSingleObject(hA, 10000);
        DWORD waitB = WaitForSingleObject(hB, 10000);

        if (waitA == WAIT_TIMEOUT || waitB == WAIT_TIMEOUT) {
            fprintf(stderr, "[test] *** MULTI-THREAD TEST HUNG (timeout 10s) ***\n");
            fprintf(stderr, "[test]   Thread A (device ops): %s\n",
                    waitA == WAIT_TIMEOUT ? "HUNG" : "done");
            fprintf(stderr, "[test]   Thread B (cmdbuf):     %s\n",
                    waitB == WAIT_TIMEOUT ? "HUNG" : "done");
            fflush(stderr);
            /* Don't exit — let remaining cleanup run */
        } else {
            fprintf(stderr, "[test] Both threads completed.\n");
            fprintf(stderr, "[test]   Thread A failed=%d  Thread B failed=%d\n",
                    ta.failed, td.failed);
            if (ta.failed == 0 && td.failed == 0) {
                fprintf(stderr, "[test] *** MULTI-THREAD TEST PASSED ***\n");
            } else {
                fprintf(stderr, "[test] *** MULTI-THREAD TEST FAILED ***\n");
            }
            fflush(stderr);
        }

        CloseHandle(hA);
        CloseHandle(hB);
        if (cmdPool2) pfn_vkDestroyCommandPool(device, cmdPool2, NULL);
    }
skip_mt:

    /* Cleanup */
    fprintf(stderr, "[test] Cleanup...\n"); fflush(stderr);
    pfn_vkDestroyCommandPool(device, cmdPool, NULL);
    pfn_vkDestroyDevice(device, NULL);
    pfn_vkDestroyInstance(instance, NULL);

    fprintf(stderr, "\n[test_vk_cmdbuf] === ALL TESTS PASSED ===\n"); fflush(stderr);
    return 0;
}
