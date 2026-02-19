/**
 * Wine Vulkan Pipeline Test: 7 progressive stages exercising the full
 * Wine Vulkan dispatch chain (winevulkan.dll → UNIX_CALL → winevulkan.so
 * → Vulkan loader → headless layer → ICD).
 *
 * Usage: wine64 test_wine_vulkan.exe [max_stage]
 *   max_stage defaults to 7 (all stages).
 *
 * Compile:
 *   x86_64-w64-mingw32-gcc -o test_wine_vulkan.exe test_wine_vulkan.c \
 *       -O2 -mconsole -lgdi32
 */

#include <windows.h>
#include <excpt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <setjmp.h>

/* ===== Vulkan types (inline, no SDK headers) ===== */

typedef void*    VkInstance;
typedef void*    VkPhysicalDevice;
typedef void*    VkDevice;
typedef void*    VkCommandPool;
typedef void*    VkCommandBuffer;
typedef void*    VkQueue;
typedef uint64_t VkSurfaceKHR;
typedef uint64_t VkSwapchainKHR;
typedef uint64_t VkImage;
typedef uint64_t VkSemaphore;
typedef uint64_t VkFence;
typedef uint64_t VkDeviceMemory;
typedef uint64_t VkBuffer;
typedef uint32_t VkFlags;
typedef int32_t  VkResult;
typedef uint32_t VkStructureType;
typedef uint32_t VkFormat;
typedef uint32_t VkColorSpaceKHR;
typedef uint32_t VkPresentModeKHR;
typedef uint64_t VkDeviceSize;

#define VK_SUCCESS                              0
#define VK_NOT_READY                            1
#define VK_TIMEOUT                              2
#define VK_SUBOPTIMAL_KHR                       1000001003

/* Structure types */
#define VK_STYPE_INSTANCE_CREATE_INFO           1
#define VK_STYPE_DEVICE_QUEUE_CREATE_INFO       2
#define VK_STYPE_DEVICE_CREATE_INFO             3
#define VK_STYPE_COMMAND_POOL_CREATE_INFO       39
#define VK_STYPE_COMMAND_BUFFER_ALLOCATE_INFO   40
#define VK_STYPE_COMMAND_BUFFER_BEGIN_INFO       42
#define VK_STYPE_SUBMIT_INFO                    4
#define VK_STYPE_FENCE_CREATE_INFO              8
#define VK_STYPE_SEMAPHORE_CREATE_INFO          9
#define VK_STYPE_IMAGE_MEMORY_BARRIER           45
#define VK_STYPE_WIN32_SURFACE_CREATE_INFO_KHR  1000009000
#define VK_STYPE_SWAPCHAIN_CREATE_INFO_KHR      1000001000
#define VK_STYPE_PRESENT_INFO_KHR               1000001001

/* Flags and enums */
#define VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT 0x00000002
#define VK_COMMAND_BUFFER_LEVEL_PRIMARY         0
#define VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT 0x00000001
#define VK_QUEUE_GRAPHICS_BIT                   0x00000001
#define VK_FENCE_CREATE_SIGNALED_BIT            0x00000001

/* Image layouts */
#define VK_IMAGE_LAYOUT_UNDEFINED               0
#define VK_IMAGE_LAYOUT_GENERAL                 1
#define VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL 2
#define VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL    7
#define VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL    6
#define VK_IMAGE_LAYOUT_PRESENT_SRC_KHR         1000001002

/* Pipeline stage flags */
#define VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT       0x00000001
#define VK_PIPELINE_STAGE_TRANSFER_BIT          0x00001000
#define VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT    0x00002000
#define VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT 0x00000400

/* Access flags */
#define VK_ACCESS_TRANSFER_READ_BIT             0x00000800
#define VK_ACCESS_TRANSFER_WRITE_BIT            0x00001000
#define VK_ACCESS_HOST_READ_BIT                 0x00002000
#define VK_ACCESS_MEMORY_READ_BIT               0x00008000

/* Image aspect */
#define VK_IMAGE_ASPECT_COLOR_BIT               0x00000001

/* Format */
#define VK_FORMAT_B8G8R8A8_UNORM                44
#define VK_FORMAT_B8G8R8A8_SRGB                 50

/* Color space */
#define VK_COLOR_SPACE_SRGB_NONLINEAR_KHR       0

/* Present mode */
#define VK_PRESENT_MODE_FIFO_KHR                2

/* Composite alpha */
#define VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR       0x00000001
#define VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR      0x00000008

/* Image usage */
#define VK_IMAGE_USAGE_TRANSFER_SRC_BIT         0x00000001
#define VK_IMAGE_USAGE_TRANSFER_DST_BIT         0x00000002
#define VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT     0x00000010

/* Buffer usage */
#define VK_BUFFER_USAGE_TRANSFER_DST_BIT        0x00000002

/* Image/buffer creation sTypes */
#define VK_STYPE_IMAGE_CREATE_INFO              14
#define VK_STYPE_MEMORY_ALLOCATE_INFO           5
#define VK_STYPE_BUFFER_CREATE_INFO             12

/* Image type / tiling / samples */
#define VK_IMAGE_TYPE_2D                        1
#define VK_SAMPLE_COUNT_1_BIT                   1
#define VK_IMAGE_TILING_OPTIMAL                 0

/* Memory property flags */
#define VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT     0x01
#define VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT     0x02
#define VK_MEMORY_PROPERTY_HOST_COHERENT_BIT    0x04

/* Sharing mode */
#define VK_SHARING_MODE_EXCLUSIVE               0

/* Surface transform */
#define VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR   0x00000001

/* Null handle */
#define VK_NULL_HANDLE 0

/* ===== Structures ===== */

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

typedef struct {
    uint32_t apiVersion;
    uint32_t driverVersion;
    uint32_t vendorID;
    uint32_t deviceID;
    uint32_t deviceType;
    char     deviceName[256];
    uint8_t  pipelineCacheUUID[16];
    /* VkPhysicalDeviceLimits and VkPhysicalDeviceSparseProperties follow,
       but we don't need them — just pad enough for the name */
    uint8_t  _pad[1024];
} MyVkPhysicalDeviceProperties;

typedef struct {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    HINSTANCE       hinstance;
    HWND            hwnd;
} MyVkWin32SurfaceCreateInfoKHR;

typedef struct {
    uint32_t    minImageCount;
    uint32_t    maxImageCount;
    uint32_t    currentExtentW;
    uint32_t    currentExtentH;
    uint32_t    minImageExtentW;
    uint32_t    minImageExtentH;
    uint32_t    maxImageExtentW;
    uint32_t    maxImageExtentH;
    uint32_t    maxImageArrayLayers;
    VkFlags     supportedTransforms;
    VkFlags     currentTransform;
    VkFlags     supportedCompositeAlpha;
    VkFlags     supportedUsageFlags;
} MyVkSurfaceCapabilitiesKHR;

typedef struct {
    VkFormat        format;
    VkColorSpaceKHR colorSpace;
} MyVkSurfaceFormatKHR;

typedef struct {
    VkStructureType     sType;
    const void*         pNext;
    VkFlags             flags;
    VkSurfaceKHR        surface;
    uint32_t            minImageCount;
    VkFormat            imageFormat;
    VkColorSpaceKHR     imageColorSpace;
    uint32_t            imageExtentW;
    uint32_t            imageExtentH;
    uint32_t            imageArrayLayers;
    VkFlags             imageUsage;
    uint32_t            imageSharingMode;
    uint32_t            queueFamilyIndexCount;
    const uint32_t*     pQueueFamilyIndices;
    VkFlags             preTransform;
    VkFlags             compositeAlpha;
    VkPresentModeKHR    presentMode;
    uint32_t            clipped;
    VkSwapchainKHR      oldSwapchain;
} MyVkSwapchainCreateInfoKHR;

typedef struct {
    VkStructureType sType;
    const void*     pNext;
} MyVkSemaphoreCreateInfo;

typedef struct {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
} MyVkFenceCreateInfo;

typedef struct {
    VkStructureType     sType;
    const void*         pNext;
    uint32_t            waitSemaphoreCount;
    const VkSemaphore*  pWaitSemaphores;
    const VkFlags*      pWaitDstStageMask;
    uint32_t            commandBufferCount;
    const VkCommandBuffer* pCommandBuffers;
    uint32_t            signalSemaphoreCount;
    const VkSemaphore*  pSignalSemaphores;
} MyVkSubmitInfo;

typedef struct {
    VkStructureType     sType;
    const void*         pNext;
    uint32_t            waitSemaphoreCount;
    const VkSemaphore*  pWaitSemaphores;
    uint32_t            swapchainCount;
    const VkSwapchainKHR* pSwapchains;
    const uint32_t*     pImageIndices;
    VkResult*           pResults;
} MyVkPresentInfoKHR;

typedef struct {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         srcAccessMask;
    VkFlags         dstAccessMask;
    uint32_t        oldLayout;
    uint32_t        newLayout;
    uint32_t        srcQueueFamilyIndex;
    uint32_t        dstQueueFamilyIndex;
    VkImage         image;
    struct {
        VkFlags aspectMask;
        uint32_t baseMipLevel;
        uint32_t levelCount;
        uint32_t baseArrayLayer;
        uint32_t layerCount;
    } subresourceRange;
} MyVkImageMemoryBarrier;

typedef union {
    float    float32[4];
    int32_t  int32[4];
    uint32_t uint32[4];
} MyVkClearColorValue;

typedef struct {
    VkFlags  aspectMask;
    uint32_t baseMipLevel;
    uint32_t levelCount;
    uint32_t baseArrayLayer;
    uint32_t layerCount;
} MyVkImageSubresourceRange;

typedef struct {
    char     extensionName[256];
    uint32_t specVersion;
} VkExtensionProperties;

/* ===== Structures for stage 7 buffer dump ===== */

typedef struct {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    uint32_t        imageType;
    VkFormat        format;
    uint32_t        width;
    uint32_t        height;
    uint32_t        depth;
    uint32_t        mipLevels;
    uint32_t        arrayLayers;
    uint32_t        samples;
    uint32_t        tiling;
    VkFlags         usage;
    uint32_t        sharingMode;
    uint32_t        queueFamilyIndexCount;
    const uint32_t* pQueueFamilyIndices;
    uint32_t        initialLayout;
} MyVkImageCreateInfo;

typedef struct {
    VkDeviceSize size;
    VkDeviceSize alignment;
    uint32_t     memoryTypeBits;
} MyVkMemoryRequirements;

typedef struct {
    VkStructureType sType;
    const void*     pNext;
    VkDeviceSize    allocationSize;
    uint32_t        memoryTypeIndex;
} MyVkMemoryAllocateInfo;

typedef struct {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    VkDeviceSize    size;
    VkFlags         usage;
    uint32_t        sharingMode;
    uint32_t        queueFamilyIndexCount;
    const uint32_t* pQueueFamilyIndices;
} MyVkBufferCreateInfo;

typedef struct {
    VkFlags  propertyFlags;
    uint32_t heapIndex;
} MyVkMemoryType;

typedef struct {
    VkDeviceSize size;
    VkFlags      flags;
} MyVkMemoryHeap;

typedef struct {
    uint32_t       memoryTypeCount;
    MyVkMemoryType memoryTypes[32];
    uint32_t       memoryHeapCount;
    MyVkMemoryHeap memoryHeaps[16];
} MyVkPhysicalDeviceMemoryProperties;

typedef struct {
    VkDeviceSize bufferOffset;
    uint32_t     bufferRowLength;
    uint32_t     bufferImageHeight;
    struct {
        VkFlags  aspectMask;
        uint32_t mipLevel;
        uint32_t baseArrayLayer;
        uint32_t layerCount;
    } imageSubresource;
    int32_t  imageOffsetX;
    int32_t  imageOffsetY;
    int32_t  imageOffsetZ;
    uint32_t imageExtentW;
    uint32_t imageExtentH;
    uint32_t imageExtentD;
} MyVkBufferImageCopy;

/* ===== Function pointer types ===== */

/* Instance-level (via GetProcAddress on vulkan-1.dll) */
typedef VkResult (WINAPI *PFN_vkEnumerateInstanceExtensionProperties)(const char*, uint32_t*, VkExtensionProperties*);
typedef VkResult (WINAPI *PFN_vkCreateInstance)(const MyVkInstanceCreateInfo*, const void*, VkInstance*);
typedef VkResult (WINAPI *PFN_vkEnumeratePhysicalDevices)(VkInstance, uint32_t*, VkPhysicalDevice*);
typedef void     (WINAPI *PFN_vkGetPhysicalDeviceProperties)(VkPhysicalDevice, MyVkPhysicalDeviceProperties*);
typedef void     (WINAPI *PFN_vkGetPhysicalDeviceQueueFamilyProperties)(VkPhysicalDevice, uint32_t*, MyVkQueueFamilyProperties*);
typedef VkResult (WINAPI *PFN_vkCreateDevice)(VkPhysicalDevice, const MyVkDeviceCreateInfo*, const void*, VkDevice*);
typedef void     (WINAPI *PFN_vkDestroyInstance)(VkInstance, const void*);
typedef void*    (WINAPI *PFN_vkGetDeviceProcAddr)(VkDevice, const char*);
typedef VkResult (WINAPI *PFN_vkCreateWin32SurfaceKHR)(VkInstance, const MyVkWin32SurfaceCreateInfoKHR*, const void*, VkSurfaceKHR*);
typedef void     (WINAPI *PFN_vkDestroySurfaceKHR)(VkInstance, VkSurfaceKHR, const void*);
typedef VkResult (WINAPI *PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)(VkPhysicalDevice, VkSurfaceKHR, MyVkSurfaceCapabilitiesKHR*);
typedef VkResult (WINAPI *PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)(VkPhysicalDevice, VkSurfaceKHR, uint32_t*, MyVkSurfaceFormatKHR*);
typedef VkResult (WINAPI *PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)(VkPhysicalDevice, VkSurfaceKHR, uint32_t*, VkPresentModeKHR*);
typedef VkResult (WINAPI *PFN_vkGetPhysicalDeviceSurfaceSupportKHR)(VkPhysicalDevice, uint32_t, VkSurfaceKHR, uint32_t*);

/* Device-level (via vkGetDeviceProcAddr — the DXVK path) */
typedef void     (WINAPI *PFN_vkDestroyDevice)(VkDevice, const void*);
typedef void     (WINAPI *PFN_vkGetDeviceQueue)(VkDevice, uint32_t, uint32_t, VkQueue*);
typedef VkResult (WINAPI *PFN_vkCreateCommandPool)(VkDevice, const MyVkCommandPoolCreateInfo*, const void*, VkCommandPool*);
typedef void     (WINAPI *PFN_vkDestroyCommandPool)(VkDevice, VkCommandPool, const void*);
typedef VkResult (WINAPI *PFN_vkAllocateCommandBuffers)(VkDevice, const MyVkCommandBufferAllocateInfo*, VkCommandBuffer*);
typedef void     (WINAPI *PFN_vkFreeCommandBuffers)(VkDevice, VkCommandPool, uint32_t, const VkCommandBuffer*);
typedef VkResult (WINAPI *PFN_vkBeginCommandBuffer)(VkCommandBuffer, const MyVkCommandBufferBeginInfo*);
typedef VkResult (WINAPI *PFN_vkEndCommandBuffer)(VkCommandBuffer);
typedef VkResult (WINAPI *PFN_vkResetCommandBuffer)(VkCommandBuffer, VkFlags);
typedef VkResult (WINAPI *PFN_vkCreateSwapchainKHR)(VkDevice, const MyVkSwapchainCreateInfoKHR*, const void*, VkSwapchainKHR*);
typedef void     (WINAPI *PFN_vkDestroySwapchainKHR)(VkDevice, VkSwapchainKHR, const void*);
typedef VkResult (WINAPI *PFN_vkGetSwapchainImagesKHR)(VkDevice, VkSwapchainKHR, uint32_t*, VkImage*);
typedef VkResult (WINAPI *PFN_vkAcquireNextImageKHR)(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t*);
typedef VkResult (WINAPI *PFN_vkQueuePresentKHR)(VkQueue, const MyVkPresentInfoKHR*);
typedef VkResult (WINAPI *PFN_vkQueueSubmit)(VkQueue, uint32_t, const MyVkSubmitInfo*, VkFence);
typedef VkResult (WINAPI *PFN_vkQueueWaitIdle)(VkQueue);
typedef VkResult (WINAPI *PFN_vkDeviceWaitIdle)(VkDevice);
typedef VkResult (WINAPI *PFN_vkCreateSemaphore)(VkDevice, const MyVkSemaphoreCreateInfo*, const void*, VkSemaphore*);
typedef void     (WINAPI *PFN_vkDestroySemaphore)(VkDevice, VkSemaphore, const void*);
typedef VkResult (WINAPI *PFN_vkCreateFence)(VkDevice, const MyVkFenceCreateInfo*, const void*, VkFence*);
typedef void     (WINAPI *PFN_vkDestroyFence)(VkDevice, VkFence, const void*);
typedef VkResult (WINAPI *PFN_vkWaitForFences)(VkDevice, uint32_t, const VkFence*, uint32_t, uint64_t);
typedef VkResult (WINAPI *PFN_vkResetFences)(VkDevice, uint32_t, const VkFence*);
typedef void     (WINAPI *PFN_vkCmdPipelineBarrier)(VkCommandBuffer, VkFlags, VkFlags, VkFlags, uint32_t, const void*, uint32_t, const void*, uint32_t, const MyVkImageMemoryBarrier*);
typedef void     (WINAPI *PFN_vkCmdClearColorImage)(VkCommandBuffer, VkImage, uint32_t, const MyVkClearColorValue*, uint32_t, const MyVkImageSubresourceRange*);

/* Stage 7 buffer dump function pointers */
typedef VkResult (WINAPI *PFN_vkCreateImage)(VkDevice, const MyVkImageCreateInfo*, const void*, VkImage*);
typedef void     (WINAPI *PFN_vkDestroyImage)(VkDevice, VkImage, const void*);
typedef void     (WINAPI *PFN_vkGetImageMemoryRequirements)(VkDevice, VkImage, MyVkMemoryRequirements*);
typedef VkResult (WINAPI *PFN_vkAllocateMemory)(VkDevice, const MyVkMemoryAllocateInfo*, const void*, VkDeviceMemory*);
typedef void     (WINAPI *PFN_vkFreeMemory)(VkDevice, VkDeviceMemory, const void*);
typedef VkResult (WINAPI *PFN_vkBindImageMemory)(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize);
typedef VkResult (WINAPI *PFN_vkCreateBuffer)(VkDevice, const MyVkBufferCreateInfo*, const void*, VkBuffer*);
typedef void     (WINAPI *PFN_vkDestroyBuffer)(VkDevice, VkBuffer, const void*);
typedef void     (WINAPI *PFN_vkGetBufferMemoryRequirements)(VkDevice, VkBuffer, MyVkMemoryRequirements*);
typedef VkResult (WINAPI *PFN_vkBindBufferMemory)(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize);
typedef VkResult (WINAPI *PFN_vkMapMemory)(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkFlags, void**);
typedef void     (WINAPI *PFN_vkUnmapMemory)(VkDevice, VkDeviceMemory);
typedef void     (WINAPI *PFN_vkGetPhysicalDeviceMemoryProperties)(VkPhysicalDevice, MyVkPhysicalDeviceMemoryProperties*);
typedef void     (WINAPI *PFN_vkCmdCopyImageToBuffer)(VkCommandBuffer, VkImage, uint32_t, VkBuffer, uint32_t, const MyVkBufferImageCopy*);

/* ===== Macros ===== */

#define LOAD(name) do { \
    pfn_##name = (PFN_##name)GetProcAddress(hVulkan, #name); \
    if (!pfn_##name) { fprintf(stderr, "FAIL: GetProcAddress(%s) = NULL\n", #name); fflush(stderr); return 1; } \
} while(0)

#define LOAD_OPTIONAL(name) do { \
    pfn_##name = (PFN_##name)GetProcAddress(hVulkan, #name); \
} while(0)

#define DLOAD(name) do { \
    pfn_##name = (PFN_##name)pfn_vkGetDeviceProcAddr(device, #name); \
    if (!pfn_##name) { fprintf(stderr, "FAIL: vkGetDeviceProcAddr(%s) = NULL\n", #name); fflush(stderr); return 1; } \
    fprintf(stderr, "  GDPA: %s = %p\n", #name, (void*)pfn_##name); \
} while(0)

#define DLOAD_OPTIONAL(name) do { \
    pfn_##name = (PFN_##name)pfn_vkGetDeviceProcAddr(device, #name); \
} while(0)

#define OK_OR_DIE(call, msg) do { \
    VkResult _r = (call); \
    if (_r != VK_SUCCESS) { fprintf(stderr, "FAIL: %s = %d\n", msg, _r); fflush(stderr); return 1; } \
} while(0)

#define STAGE(n, desc) do { \
    if (max_stage < (n)) goto cleanup; \
    fprintf(stderr, "\n===== STAGE %d: %s =====\n", (n), (desc)); fflush(stderr); \
} while(0)

/* ===== SEH workaround for MinGW (stage 6 exception handling) ===== */

static jmp_buf g_stage6_jmpbuf;
static DWORD   g_stage6_exception_code = 0;

static LONG WINAPI stage6_veh(EXCEPTION_POINTERS *ep) {
    g_stage6_exception_code = ep->ExceptionRecord->ExceptionCode;
    longjmp(g_stage6_jmpbuf, 1);
    return EXCEPTION_CONTINUE_EXECUTION; /* unreachable */
}

/* ===== Multi-threaded ACB stress test (stage 5) ===== */

typedef struct {
    PFN_vkCreateCommandPool    createPool;
    PFN_vkAllocateCommandBuffers allocBufs;
    PFN_vkBeginCommandBuffer   begin;
    PFN_vkEndCommandBuffer     end;
    PFN_vkFreeCommandBuffers   freeBufs;
    PFN_vkDestroyCommandPool   destroyPool;
    VkDevice                   dev;
    uint32_t                   queueFamily;
    int                        threadId;
    int                        iterations;
    volatile LONG              *go;
    volatile int               failed;
    int                        failIter;
} ThreadACBData;

static DWORD WINAPI thread_acb_func(LPVOID arg) {
    ThreadACBData *d = (ThreadACBData*)arg;

    /* Spin-wait until go signal */
    while (!InterlockedCompareExchange((volatile LONG*)d->go, 0, 0))
        Sleep(0);

    for (int i = 0; i < d->iterations; i++) {
        /* Create pool */
        MyVkCommandPoolCreateInfo cpci;
        memset(&cpci, 0, sizeof(cpci));
        cpci.sType = VK_STYPE_COMMAND_POOL_CREATE_INFO;
        cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cpci.queueFamilyIndex = d->queueFamily;
        VkCommandPool pool = NULL;
        VkResult r = d->createPool(d->dev, &cpci, NULL, &pool);
        if (r != VK_SUCCESS) { d->failed = r; d->failIter = i; return 1; }

        /* Allocate 1 cmdbuf */
        MyVkCommandBufferAllocateInfo ai;
        memset(&ai, 0, sizeof(ai));
        ai.sType = VK_STYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer buf = NULL;
        r = d->allocBufs(d->dev, &ai, &buf);
        if (r != VK_SUCCESS) { d->failed = r; d->failIter = i; d->destroyPool(d->dev, pool, NULL); return 1; }

        /* Begin */
        MyVkCommandBufferBeginInfo bi;
        memset(&bi, 0, sizeof(bi));
        bi.sType = VK_STYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        r = d->begin(buf, &bi);
        if (r != VK_SUCCESS) { d->failed = r; d->failIter = i; d->destroyPool(d->dev, pool, NULL); return 1; }

        /* End */
        r = d->end(buf);
        if (r != VK_SUCCESS) { d->failed = r; d->failIter = i; d->destroyPool(d->dev, pool, NULL); return 1; }

        /* Free + destroy */
        d->freeBufs(d->dev, pool, 1, &buf);
        d->destroyPool(d->dev, pool, NULL);
    }
    return 0;
}

/* ===== Main ===== */

int main(int argc, char **argv) {
    int max_stage = 7;
    if (argc > 1) max_stage = atoi(argv[1]);
    if (max_stage < 1) max_stage = 1;
    if (max_stage > 7) max_stage = 7;

    fprintf(stderr, "\n[test_wine_vulkan] === Wine Vulkan Pipeline Test (stages 1-%d) ===\n", max_stage);
    fflush(stderr);

    /* State that persists across stages */
    VkInstance       instance = NULL;
    VkPhysicalDevice gpu      = NULL;
    VkDevice         device   = NULL;
    VkQueue          queue    = NULL;
    VkSurfaceKHR     surface  = VK_NULL_HANDLE;
    VkSwapchainKHR   swapchain = VK_NULL_HANDLE;
    VkSemaphore      acquireSem = VK_NULL_HANDLE;
    VkSemaphore      renderSem  = VK_NULL_HANDLE;
    VkFence          fence      = VK_NULL_HANDLE;
    VkCommandPool    cmdPool    = NULL;
    VkCommandBuffer  cmdBuf     = NULL;
    HWND             hwnd       = NULL;
    HMODULE          hVulkan    = NULL;
    uint32_t         gfxQF      = 0;
    VkImage          swapImages[8];
    uint32_t         swapImageCount = 0;

    /* Function pointers — instance level */
    PFN_vkCreateInstance                        pfn_vkCreateInstance;
    PFN_vkEnumeratePhysicalDevices              pfn_vkEnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceProperties           pfn_vkGetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties pfn_vkGetPhysicalDeviceQueueFamilyProperties;
    PFN_vkCreateDevice                          pfn_vkCreateDevice;
    PFN_vkDestroyInstance                       pfn_vkDestroyInstance;
    PFN_vkGetDeviceProcAddr                     pfn_vkGetDeviceProcAddr;
    PFN_vkCreateWin32SurfaceKHR                 pfn_vkCreateWin32SurfaceKHR;
    PFN_vkDestroySurfaceKHR                     pfn_vkDestroySurfaceKHR;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR pfn_vkGetPhysicalDeviceSurfaceCapabilitiesKHR;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR    pfn_vkGetPhysicalDeviceSurfaceFormatsKHR;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR pfn_vkGetPhysicalDeviceSurfacePresentModesKHR;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR    pfn_vkGetPhysicalDeviceSurfaceSupportKHR;

    /* Function pointers — device level (resolved via GDPA in stage 3) */
    PFN_vkDestroyDevice             pfn_vkDestroyDevice = NULL;
    PFN_vkGetDeviceQueue            pfn_vkGetDeviceQueue = NULL;
    PFN_vkCreateCommandPool         pfn_vkCreateCommandPool = NULL;
    PFN_vkDestroyCommandPool        pfn_vkDestroyCommandPool = NULL;
    PFN_vkAllocateCommandBuffers    pfn_vkAllocateCommandBuffers = NULL;
    PFN_vkFreeCommandBuffers        pfn_vkFreeCommandBuffers = NULL;
    PFN_vkBeginCommandBuffer        pfn_vkBeginCommandBuffer = NULL;
    PFN_vkEndCommandBuffer          pfn_vkEndCommandBuffer = NULL;
    PFN_vkResetCommandBuffer        pfn_vkResetCommandBuffer = NULL;
    PFN_vkCreateSwapchainKHR        pfn_vkCreateSwapchainKHR = NULL;
    PFN_vkDestroySwapchainKHR       pfn_vkDestroySwapchainKHR = NULL;
    PFN_vkGetSwapchainImagesKHR     pfn_vkGetSwapchainImagesKHR = NULL;
    PFN_vkAcquireNextImageKHR       pfn_vkAcquireNextImageKHR = NULL;
    PFN_vkQueuePresentKHR           pfn_vkQueuePresentKHR = NULL;
    PFN_vkQueueSubmit               pfn_vkQueueSubmit = NULL;
    PFN_vkQueueWaitIdle             pfn_vkQueueWaitIdle = NULL;
    PFN_vkDeviceWaitIdle            pfn_vkDeviceWaitIdle = NULL;
    PFN_vkCreateSemaphore           pfn_vkCreateSemaphore = NULL;
    PFN_vkDestroySemaphore          pfn_vkDestroySemaphore = NULL;
    PFN_vkCreateFence               pfn_vkCreateFence = NULL;
    PFN_vkDestroyFence              pfn_vkDestroyFence = NULL;
    PFN_vkWaitForFences             pfn_vkWaitForFences = NULL;
    PFN_vkResetFences               pfn_vkResetFences = NULL;
    PFN_vkCmdPipelineBarrier        pfn_vkCmdPipelineBarrier = NULL;
    PFN_vkCmdClearColorImage        pfn_vkCmdClearColorImage = NULL;

    /* Stage 7 buffer dump function pointers */
    PFN_vkCreateImage               pfn_vkCreateImage = NULL;
    PFN_vkDestroyImage              pfn_vkDestroyImage = NULL;
    PFN_vkGetImageMemoryRequirements pfn_vkGetImageMemoryRequirements = NULL;
    PFN_vkAllocateMemory            pfn_vkAllocateMemory = NULL;
    PFN_vkFreeMemory                pfn_vkFreeMemory = NULL;
    PFN_vkBindImageMemory           pfn_vkBindImageMemory = NULL;
    PFN_vkCreateBuffer              pfn_vkCreateBuffer = NULL;
    PFN_vkDestroyBuffer             pfn_vkDestroyBuffer = NULL;
    PFN_vkGetBufferMemoryRequirements pfn_vkGetBufferMemoryRequirements = NULL;
    PFN_vkBindBufferMemory          pfn_vkBindBufferMemory = NULL;
    PFN_vkMapMemory                 pfn_vkMapMemory = NULL;
    PFN_vkUnmapMemory               pfn_vkUnmapMemory = NULL;
    PFN_vkGetPhysicalDeviceMemoryProperties pfn_vkGetPhysicalDeviceMemoryProperties = NULL;
    PFN_vkCmdCopyImageToBuffer      pfn_vkCmdCopyImageToBuffer = NULL;

    /* Stage 7 resources */
    VkImage        dumpImage = VK_NULL_HANDLE;
    VkDeviceMemory dumpImageMem = VK_NULL_HANDLE;
    VkBuffer       dumpBuffer = VK_NULL_HANDLE;
    VkDeviceMemory dumpBufferMem = VK_NULL_HANDLE;

    /* Load vulkan-1.dll */
    fprintf(stderr, "[test] Loading vulkan-1.dll...\n"); fflush(stderr);
    hVulkan = LoadLibraryA("vulkan-1.dll");
    if (!hVulkan) {
        fprintf(stderr, "FAIL: LoadLibrary(vulkan-1.dll) error %lu\n", GetLastError());
        fflush(stderr);
        return 1;
    }
    fprintf(stderr, "[test] vulkan-1.dll loaded at %p\n", hVulkan); fflush(stderr);

    /* Resolve instance-level functions */
    LOAD(vkCreateInstance);
    LOAD(vkEnumeratePhysicalDevices);
    LOAD(vkGetPhysicalDeviceProperties);
    LOAD(vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD(vkCreateDevice);
    LOAD(vkDestroyInstance);
    LOAD(vkGetDeviceProcAddr);
    LOAD_OPTIONAL(vkCreateWin32SurfaceKHR);
    LOAD_OPTIONAL(vkDestroySurfaceKHR);
    LOAD_OPTIONAL(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    LOAD_OPTIONAL(vkGetPhysicalDeviceSurfaceFormatsKHR);
    LOAD_OPTIONAL(vkGetPhysicalDeviceSurfacePresentModesKHR);
    LOAD_OPTIONAL(vkGetPhysicalDeviceSurfaceSupportKHR);
    LOAD_OPTIONAL(vkGetPhysicalDeviceMemoryProperties);
    fprintf(stderr, "[test] Instance-level function pointers resolved\n"); fflush(stderr);

    /* Enumerate instance extensions to see what Wine exposes */
    {
        PFN_vkEnumerateInstanceExtensionProperties pfn_enum =
            (PFN_vkEnumerateInstanceExtensionProperties)GetProcAddress(hVulkan,
                "vkEnumerateInstanceExtensionProperties");
        if (pfn_enum) {
            uint32_t extCount = 0;
            pfn_enum(NULL, &extCount, NULL);
            fprintf(stderr, "[test] Instance extensions available: %u\n", extCount);
            VkExtensionProperties exts[128];
            uint32_t n = extCount < 128 ? extCount : 128;
            pfn_enum(NULL, &n, exts);
            int has_surface = 0, has_win32 = 0;
            for (uint32_t i = 0; i < n; i++) {
                if (strstr(exts[i].extensionName, "surface") ||
                    strstr(exts[i].extensionName, "Surface"))
                    fprintf(stderr, "[test]   %s (v%u)\n", exts[i].extensionName, exts[i].specVersion);
                if (strcmp(exts[i].extensionName, "VK_KHR_surface") == 0) has_surface = 1;
                if (strcmp(exts[i].extensionName, "VK_KHR_win32_surface") == 0) has_win32 = 1;
            }
            fprintf(stderr, "[test] VK_KHR_surface: %s  VK_KHR_win32_surface: %s\n",
                    has_surface ? "YES" : "NO", has_win32 ? "YES" : "NO");
            fflush(stderr);
        }
    }

    /* ===== STAGE 1: Instance creation with surface extensions ===== */
    STAGE(1, "vkCreateInstance with surface extensions");
    {
        const char *instExts[] = {
            "VK_KHR_surface",
            "VK_KHR_win32_surface",
        };
        MyVkInstanceCreateInfo ici;
        memset(&ici, 0, sizeof(ici));
        ici.sType = VK_STYPE_INSTANCE_CREATE_INFO;
        ici.enabledExtensionCount = 2;
        ici.ppEnabledExtensionNames = instExts;

        VkResult r = pfn_vkCreateInstance(&ici, NULL, &instance);
        fprintf(stderr, "[stage1] vkCreateInstance: result=%d instance=%p\n", r, instance);
        fflush(stderr);
        if (r != VK_SUCCESS) {
            fprintf(stderr, "[stage1] Retrying without surface exts...\n"); fflush(stderr);
            ici.enabledExtensionCount = 0;
            ici.ppEnabledExtensionNames = NULL;
            r = pfn_vkCreateInstance(&ici, NULL, &instance);
            fprintf(stderr, "[stage1] vkCreateInstance (bare): result=%d instance=%p\n", r, instance);
            fflush(stderr);
            if (r != VK_SUCCESS) return 1;
        }
        fprintf(stderr, "[stage1] PASS\n"); fflush(stderr);
    }

    /* ===== STAGE 2: Physical device enumeration + properties ===== */
    STAGE(2, "vkEnumeratePhysicalDevices + Properties");
    {
        uint32_t gpuCount = 0;
        pfn_vkEnumeratePhysicalDevices(instance, &gpuCount, NULL);
        fprintf(stderr, "[stage2] GPU count: %u\n", gpuCount); fflush(stderr);
        if (gpuCount == 0) { fprintf(stderr, "[stage2] FAIL: no GPUs\n"); return 1; }

        gpuCount = 1;
        pfn_vkEnumeratePhysicalDevices(instance, &gpuCount, &gpu);
        fprintf(stderr, "[stage2] GPU handle: %p\n", gpu); fflush(stderr);

        MyVkPhysicalDeviceProperties props;
        memset(&props, 0, sizeof(props));
        pfn_vkGetPhysicalDeviceProperties(gpu, &props);
        fprintf(stderr, "[stage2] Device: %s\n", props.deviceName);
        fprintf(stderr, "[stage2] API version: %u.%u.%u\n",
                props.apiVersion >> 22, (props.apiVersion >> 12) & 0x3FF,
                props.apiVersion & 0xFFF);
        fprintf(stderr, "[stage2] Vendor: 0x%04X  Device: 0x%04X\n",
                props.vendorID, props.deviceID);
        fflush(stderr);

        /* Find graphics queue family */
        uint32_t qfCount = 0;
        pfn_vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qfCount, NULL);
        MyVkQueueFamilyProperties qfProps[16];
        if (qfCount > 16) qfCount = 16;
        pfn_vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qfCount, qfProps);
        for (uint32_t i = 0; i < qfCount; i++) {
            fprintf(stderr, "[stage2] QF[%u]: flags=0x%x count=%u\n",
                    i, qfProps[i].queueFlags, qfProps[i].queueCount);
            if (qfProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
                gfxQF = i;
        }
        fprintf(stderr, "[stage2] Using graphics queue family %u\n", gfxQF);
        fprintf(stderr, "[stage2] PASS\n"); fflush(stderr);
    }

    /* ===== STAGE 3: Device creation + GDPA resolution ===== */
    STAGE(3, "vkCreateDevice + vkGetDeviceProcAddr resolution");
    {
        float qp = 1.0f;
        MyVkDeviceQueueCreateInfo qci;
        memset(&qci, 0, sizeof(qci));
        qci.sType = VK_STYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = gfxQF;
        qci.queueCount = 1;
        qci.pQueuePriorities = &qp;

        const char *devExts[] = { "VK_KHR_swapchain" };
        MyVkDeviceCreateInfo dci;
        memset(&dci, 0, sizeof(dci));
        dci.sType = VK_STYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = devExts;

        VkResult r = pfn_vkCreateDevice(gpu, &dci, NULL, &device);
        fprintf(stderr, "[stage3] vkCreateDevice: result=%d device=%p\n", r, device);
        fflush(stderr);
        if (r != VK_SUCCESS) {
            fprintf(stderr, "[stage3] FAIL\n"); fflush(stderr);
            return 1;
        }

        /* Resolve ALL device functions via vkGetDeviceProcAddr (the DXVK path) */
        fprintf(stderr, "[stage3] Resolving device functions via vkGetDeviceProcAddr:\n");
        fflush(stderr);
        DLOAD(vkDestroyDevice);
        DLOAD(vkGetDeviceQueue);
        DLOAD(vkCreateCommandPool);
        DLOAD(vkDestroyCommandPool);
        DLOAD(vkAllocateCommandBuffers);
        DLOAD(vkFreeCommandBuffers);
        DLOAD(vkBeginCommandBuffer);
        DLOAD(vkEndCommandBuffer);
        DLOAD(vkResetCommandBuffer);
        DLOAD(vkQueueSubmit);
        DLOAD(vkQueueWaitIdle);
        DLOAD(vkDeviceWaitIdle);
        DLOAD(vkCreateSemaphore);
        DLOAD(vkDestroySemaphore);
        DLOAD(vkCreateFence);
        DLOAD(vkDestroyFence);
        DLOAD(vkWaitForFences);
        DLOAD(vkResetFences);
        DLOAD(vkCmdPipelineBarrier);
        DLOAD(vkCmdClearColorImage);
        DLOAD_OPTIONAL(vkCreateSwapchainKHR);
        DLOAD_OPTIONAL(vkDestroySwapchainKHR);
        DLOAD_OPTIONAL(vkGetSwapchainImagesKHR);
        DLOAD_OPTIONAL(vkAcquireNextImageKHR);
        DLOAD_OPTIONAL(vkQueuePresentKHR);

        /* Stage 7 buffer dump functions */
        DLOAD(vkCreateImage);
        DLOAD(vkDestroyImage);
        DLOAD(vkGetImageMemoryRequirements);
        DLOAD(vkAllocateMemory);
        DLOAD(vkFreeMemory);
        DLOAD(vkBindImageMemory);
        DLOAD(vkCreateBuffer);
        DLOAD(vkDestroyBuffer);
        DLOAD(vkGetBufferMemoryRequirements);
        DLOAD(vkBindBufferMemory);
        DLOAD(vkMapMemory);
        DLOAD(vkUnmapMemory);
        DLOAD(vkCmdCopyImageToBuffer);

        pfn_vkGetDeviceQueue(device, gfxQF, 0, &queue);
        fprintf(stderr, "[stage3] Queue: %p\n", queue); fflush(stderr);

        fprintf(stderr, "[stage3] PASS\n"); fflush(stderr);
    }

    /* ===== STAGE 4: Single-threaded ACB stress (20 cycles) ===== */
    STAGE(4, "Single-threaded command buffer stress (20 cycles)");
    {
        for (int i = 0; i < 20; i++) {
            fprintf(stderr, "[stage4] Cycle %d/20: ", i + 1); fflush(stderr);

            /* Create pool */
            MyVkCommandPoolCreateInfo cpci;
            memset(&cpci, 0, sizeof(cpci));
            cpci.sType = VK_STYPE_COMMAND_POOL_CREATE_INFO;
            cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            cpci.queueFamilyIndex = gfxQF;
            VkCommandPool pool = NULL;
            VkResult r = pfn_vkCreateCommandPool(device, &cpci, NULL, &pool);
            if (r != VK_SUCCESS) {
                fprintf(stderr, "CreatePool FAILED (%d)\n", r); fflush(stderr);
                return 1;
            }

            /* Allocate */
            MyVkCommandBufferAllocateInfo ai;
            memset(&ai, 0, sizeof(ai));
            ai.sType = VK_STYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            ai.commandPool = pool;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            VkCommandBuffer buf = NULL;
            r = pfn_vkAllocateCommandBuffers(device, &ai, &buf);
            if (r != VK_SUCCESS) {
                fprintf(stderr, "AllocCmdBuf FAILED (%d)\n", r); fflush(stderr);
                return 1;
            }

            /* Begin */
            MyVkCommandBufferBeginInfo bi;
            memset(&bi, 0, sizeof(bi));
            bi.sType = VK_STYPE_COMMAND_BUFFER_BEGIN_INFO;
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            r = pfn_vkBeginCommandBuffer(buf, &bi);
            if (r != VK_SUCCESS) {
                fprintf(stderr, "BeginCmdBuf FAILED (%d)\n", r); fflush(stderr);
                return 1;
            }

            /* End */
            r = pfn_vkEndCommandBuffer(buf);
            if (r != VK_SUCCESS) {
                fprintf(stderr, "EndCmdBuf FAILED (%d)\n", r); fflush(stderr);
                return 1;
            }

            /* Free + destroy */
            pfn_vkFreeCommandBuffers(device, pool, 1, &buf);
            pfn_vkDestroyCommandPool(device, pool, NULL);

            fprintf(stderr, "OK\n"); fflush(stderr);
        }
        fprintf(stderr, "[stage4] PASS — 20/20 cycles completed\n"); fflush(stderr);
    }

    /* ===== STAGE 5: Multi-threaded ACB stress (3 threads × 10 cycles) ===== */
    STAGE(5, "Multi-threaded ACB stress (3 threads x 10 cycles)");
    {
        #define MT_THREADS 3
        #define MT_ITERS 10
        volatile LONG go = 0;
        ThreadACBData td[MT_THREADS];
        HANDLE hThreads[MT_THREADS];

        for (int t = 0; t < MT_THREADS; t++) {
            memset(&td[t], 0, sizeof(td[t]));
            td[t].createPool  = pfn_vkCreateCommandPool;
            td[t].allocBufs   = pfn_vkAllocateCommandBuffers;
            td[t].begin       = pfn_vkBeginCommandBuffer;
            td[t].end         = pfn_vkEndCommandBuffer;
            td[t].freeBufs    = pfn_vkFreeCommandBuffers;
            td[t].destroyPool = pfn_vkDestroyCommandPool;
            td[t].dev         = device;
            td[t].queueFamily = gfxQF;
            td[t].threadId    = t;
            td[t].iterations  = MT_ITERS;
            td[t].go          = &go;

            hThreads[t] = CreateThread(NULL, 0, thread_acb_func, &td[t], 0, NULL);
            if (!hThreads[t]) {
                fprintf(stderr, "[stage5] CreateThread(%d) failed: %lu\n", t, GetLastError());
                fflush(stderr);
                return 1;
            }
        }

        fprintf(stderr, "[stage5] %d threads created, starting race...\n", MT_THREADS);
        fflush(stderr);
        InterlockedExchange((volatile LONG*)&go, 1);

        DWORD wait = WaitForMultipleObjects(MT_THREADS, hThreads, TRUE, 10000);
        if (wait == WAIT_TIMEOUT) {
            fprintf(stderr, "[stage5] TIMEOUT (10s) — possible deadlock!\n");
            for (int t = 0; t < MT_THREADS; t++)
                fprintf(stderr, "  Thread %d: failed=%d\n", t, td[t].failed);
            fflush(stderr);
        } else {
            int allOk = 1;
            for (int t = 0; t < MT_THREADS; t++) {
                if (td[t].failed) {
                    fprintf(stderr, "[stage5] Thread %d FAILED at iter %d: error=%d\n",
                            t, td[t].failIter, td[t].failed);
                    allOk = 0;
                }
            }
            if (allOk) {
                fprintf(stderr, "[stage5] PASS — %d threads × %d cycles all OK\n",
                        MT_THREADS, MT_ITERS);
            } else {
                fprintf(stderr, "[stage5] FAIL — see above\n");
            }
            fflush(stderr);
        }

        for (int t = 0; t < MT_THREADS; t++)
            CloseHandle(hThreads[t]);
    }

    /* ===== STAGE 6: Win32 surface + swapchain creation ===== */
    STAGE(6, "Win32 surface + swapchain creation");
    {
        VkResult r;
        int stage6_ok = 0;

        /* Install vectored exception handler to catch Wine's assertion when
         * X11 (libXlorie) is running but HWND→X11 Window mapping fails.
         * Wine's UNIX_CALL assertion becomes a structured exception. */
        void *veh = AddVectoredExceptionHandler(1, stage6_veh);
        g_stage6_exception_code = 0;

        if (setjmp(g_stage6_jmpbuf) == 0) {
            /* Try CreateWindowExA first — creates a proper X11-backed window.
             * Fall back to GetDesktopWindow if it fails. */
            hwnd = CreateWindowExA(
                0, "STATIC", "VulkanTest", WS_OVERLAPPEDWINDOW,
                0, 0, 1280, 720, NULL, NULL, GetModuleHandleA(NULL), NULL);
            if (!hwnd) {
                fprintf(stderr, "[stage6] CreateWindowExA failed (err=%lu), trying GetDesktopWindow\n",
                        GetLastError());
                fflush(stderr);
                hwnd = GetDesktopWindow();
            }
            fprintf(stderr, "[stage6] Window: hwnd=%p\n", hwnd); fflush(stderr);
            if (!hwnd) {
                fprintf(stderr, "[stage6] SKIP — no window available\n"); fflush(stderr);
                RemoveVectoredExceptionHandler(veh);
                goto stage7;
            }

            /* Create Win32 surface */
            if (!pfn_vkCreateWin32SurfaceKHR) {
                fprintf(stderr, "[stage6] SKIP — vkCreateWin32SurfaceKHR not available\n");
                fflush(stderr);
                RemoveVectoredExceptionHandler(veh);
                goto stage7;
            }

            MyVkWin32SurfaceCreateInfoKHR sci;
            memset(&sci, 0, sizeof(sci));
            sci.sType = VK_STYPE_WIN32_SURFACE_CREATE_INFO_KHR;
            sci.hinstance = GetModuleHandleA(NULL);
            sci.hwnd = hwnd;

            r = pfn_vkCreateWin32SurfaceKHR(instance, &sci, NULL, &surface);
            fprintf(stderr, "[stage6] vkCreateWin32SurfaceKHR: result=%d surface=0x%llx\n",
                    r, (unsigned long long)surface);
            fflush(stderr);
            if (r != VK_SUCCESS) {
                fprintf(stderr, "[stage6] Surface creation returned error %d, skipping\n", r);
                fflush(stderr);
                RemoveVectoredExceptionHandler(veh);
                goto stage7;
            }

            /* Check surface support */
            if (pfn_vkGetPhysicalDeviceSurfaceSupportKHR) {
                uint32_t supported = 0;
                pfn_vkGetPhysicalDeviceSurfaceSupportKHR(gpu, gfxQF, surface, &supported);
                fprintf(stderr, "[stage6] Surface support on QF %u: %s\n",
                        gfxQF, supported ? "YES" : "NO");
                fflush(stderr);
            }

            /* Query capabilities */
            if (pfn_vkGetPhysicalDeviceSurfaceCapabilitiesKHR) {
                MyVkSurfaceCapabilitiesKHR caps;
                memset(&caps, 0, sizeof(caps));
                r = pfn_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps);
                fprintf(stderr, "[stage6] Surface caps: result=%d\n", r);
                if (r == VK_SUCCESS) {
                    fprintf(stderr, "[stage6]   images: %u-%u  extent: %ux%u  usage: 0x%x\n",
                            caps.minImageCount, caps.maxImageCount,
                            caps.currentExtentW, caps.currentExtentH,
                            caps.supportedUsageFlags);
                }
                fflush(stderr);
            }

            /* Query formats */
            if (pfn_vkGetPhysicalDeviceSurfaceFormatsKHR) {
                uint32_t fmtCount = 0;
                pfn_vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &fmtCount, NULL);
                fprintf(stderr, "[stage6] Surface format count: %u\n", fmtCount);
                MyVkSurfaceFormatKHR fmts[16];
                if (fmtCount > 16) fmtCount = 16;
                if (fmtCount > 0) {
                    pfn_vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &fmtCount, fmts);
                    for (uint32_t i = 0; i < fmtCount; i++) {
                        fprintf(stderr, "[stage6]   fmt[%u]: format=%u colorSpace=%u\n",
                                i, fmts[i].format, fmts[i].colorSpace);
                    }
                }
                fflush(stderr);
            }

            stage6_ok = 1;

        } else {
            /* Caught exception via VEH + longjmp */
            fprintf(stderr, "[stage6] EXCEPTION caught (code=0x%lx) — Wine assertion in surface creation\n",
                    (unsigned long)g_stage6_exception_code);
            fprintf(stderr, "[stage6] This happens when X11 (libXlorie) is running.\n");
            fprintf(stderr, "[stage6] Surface tests skipped, continuing to stage 7...\n");
            fflush(stderr);
            surface = VK_NULL_HANDLE;
            hwnd = NULL;
        }

        RemoveVectoredExceptionHandler(veh);

        if (stage6_ok)
            fprintf(stderr, "[stage6] PASS\n");
        else
            fprintf(stderr, "[stage6] SKIP (exception or error)\n");
        fflush(stderr);
    }

    /* ===== STAGE 7: Buffer dump (render to image, copy to host, write PPM) ===== */
stage7:
    STAGE(7, "Buffer dump (ClearColorImage → CopyImageToBuffer → PPM file)");
    {
        #define DUMP_W 1280
        #define DUMP_H 720
        VkDeviceSize bufSize = (VkDeviceSize)DUMP_W * DUMP_H * 4;
        VkResult r;

        fprintf(stderr, "[stage7] Creating %dx%d test image + staging buffer...\n", DUMP_W, DUMP_H);
        fflush(stderr);

        /* Query memory properties */
        MyVkPhysicalDeviceMemoryProperties memProps;
        memset(&memProps, 0, sizeof(memProps));
        if (!pfn_vkGetPhysicalDeviceMemoryProperties) {
            fprintf(stderr, "[stage7] FAIL — vkGetPhysicalDeviceMemoryProperties not available\n");
            fflush(stderr);
            goto cleanup;
        }
        pfn_vkGetPhysicalDeviceMemoryProperties(gpu, &memProps);
        fprintf(stderr, "[stage7] Memory types: %u  heaps: %u\n",
                memProps.memoryTypeCount, memProps.memoryHeapCount);
        fflush(stderr);

        /* Create OPTIMAL image (TRANSFER_SRC for copy + TRANSFER_DST for clear) */
        MyVkImageCreateInfo ici;
        memset(&ici, 0, sizeof(ici));
        ici.sType = VK_STYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_B8G8R8A8_UNORM;
        ici.width = DUMP_W;
        ici.height = DUMP_H;
        ici.depth = 1;
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        r = pfn_vkCreateImage(device, &ici, NULL, &dumpImage);
        fprintf(stderr, "[stage7] vkCreateImage: result=%d image=0x%llx\n",
                r, (unsigned long long)dumpImage);
        fflush(stderr);
        if (r != VK_SUCCESS) {
            fprintf(stderr, "[stage7] FAIL — CreateImage\n"); fflush(stderr);
            goto cleanup;
        }

        /* Allocate + bind DEVICE_LOCAL memory for image */
        MyVkMemoryRequirements imgReqs;
        pfn_vkGetImageMemoryRequirements(device, dumpImage, &imgReqs);
        fprintf(stderr, "[stage7] Image mem: size=%llu align=%llu typeBits=0x%x\n",
                (unsigned long long)imgReqs.size, (unsigned long long)imgReqs.alignment,
                imgReqs.memoryTypeBits);
        fflush(stderr);

        uint32_t imgMemType = 0xFFFFFFFF;
        for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
            if ((imgReqs.memoryTypeBits & (1u << i)) &&
                (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                imgMemType = i;
                break;
            }
        }
        if (imgMemType == 0xFFFFFFFF) {
            /* Fallback: any compatible type */
            for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
                if (imgReqs.memoryTypeBits & (1u << i)) { imgMemType = i; break; }
            }
        }
        fprintf(stderr, "[stage7] Image memory type: %u\n", imgMemType); fflush(stderr);

        MyVkMemoryAllocateInfo mai;
        memset(&mai, 0, sizeof(mai));
        mai.sType = VK_STYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = imgReqs.size;
        mai.memoryTypeIndex = imgMemType;
        OK_OR_DIE(pfn_vkAllocateMemory(device, &mai, NULL, &dumpImageMem), "AllocMem(image)");
        OK_OR_DIE(pfn_vkBindImageMemory(device, dumpImage, dumpImageMem, 0), "BindImageMem");

        /* Create HOST_VISIBLE staging buffer */
        MyVkBufferCreateInfo bci;
        memset(&bci, 0, sizeof(bci));
        bci.sType = VK_STYPE_BUFFER_CREATE_INFO;
        bci.size = bufSize;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        r = pfn_vkCreateBuffer(device, &bci, NULL, &dumpBuffer);
        fprintf(stderr, "[stage7] vkCreateBuffer: result=%d buffer=0x%llx\n",
                r, (unsigned long long)dumpBuffer);
        fflush(stderr);
        if (r != VK_SUCCESS) {
            fprintf(stderr, "[stage7] FAIL — CreateBuffer\n"); fflush(stderr);
            goto cleanup;
        }

        MyVkMemoryRequirements bufReqs;
        pfn_vkGetBufferMemoryRequirements(device, dumpBuffer, &bufReqs);

        uint32_t bufMemType = 0xFFFFFFFF;
        for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
            if ((bufReqs.memoryTypeBits & (1u << i)) &&
                (memProps.memoryTypes[i].propertyFlags &
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                bufMemType = i;
                break;
            }
        }
        if (bufMemType == 0xFFFFFFFF) {
            for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
                if ((bufReqs.memoryTypeBits & (1u << i)) &&
                    (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                    bufMemType = i;
                    break;
                }
            }
        }
        fprintf(stderr, "[stage7] Buffer memory type: %u\n", bufMemType); fflush(stderr);

        mai.allocationSize = bufReqs.size;
        mai.memoryTypeIndex = bufMemType;
        OK_OR_DIE(pfn_vkAllocateMemory(device, &mai, NULL, &dumpBufferMem), "AllocMem(buffer)");
        OK_OR_DIE(pfn_vkBindBufferMemory(device, dumpBuffer, dumpBufferMem, 0), "BindBufferMem");

        /* Create command pool + buffer */
        if (!cmdPool) {
            MyVkCommandPoolCreateInfo cpci;
            memset(&cpci, 0, sizeof(cpci));
            cpci.sType = VK_STYPE_COMMAND_POOL_CREATE_INFO;
            cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            cpci.queueFamilyIndex = gfxQF;
            OK_OR_DIE(pfn_vkCreateCommandPool(device, &cpci, NULL, &cmdPool), "CreateCommandPool");

            MyVkCommandBufferAllocateInfo cbai;
            memset(&cbai, 0, sizeof(cbai));
            cbai.sType = VK_STYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            cbai.commandPool = cmdPool;
            cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cbai.commandBufferCount = 1;
            OK_OR_DIE(pfn_vkAllocateCommandBuffers(device, &cbai, &cmdBuf), "AllocateCmdBuf");
        }

        /* Create fence */
        if (fence == VK_NULL_HANDLE) {
            MyVkFenceCreateInfo fCI;
            memset(&fCI, 0, sizeof(fCI));
            fCI.sType = VK_STYPE_FENCE_CREATE_INFO;
            OK_OR_DIE(pfn_vkCreateFence(device, &fCI, NULL, &fence), "CreateFence");
        }

        /* Check device health */
        {
            VkResult dwi = pfn_vkDeviceWaitIdle(device);
            fprintf(stderr, "[stage7] Pre-render DeviceWaitIdle: %d\n", dwi);
            fflush(stderr);
            if (dwi != VK_SUCCESS) {
                fprintf(stderr, "[stage7] FAIL — device already lost!\n"); fflush(stderr);
                goto cleanup;
            }
        }

        /* Record command buffer:
         *   1. Barrier: UNDEFINED → GENERAL (for ClearColorImage)
         *   2. ClearColorImage (red)
         *   3. Barrier: GENERAL → TRANSFER_SRC (for CopyImageToBuffer)
         *   4. CopyImageToBuffer
         */
        fprintf(stderr, "[stage7] Recording command buffer...\n"); fflush(stderr);

        OK_OR_DIE(pfn_vkResetCommandBuffer(cmdBuf, 0), "ResetCmdBuf");
        MyVkCommandBufferBeginInfo bi;
        memset(&bi, 0, sizeof(bi));
        bi.sType = VK_STYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        OK_OR_DIE(pfn_vkBeginCommandBuffer(cmdBuf, &bi), "BeginCmdBuf");

        /* Barrier: UNDEFINED → GENERAL */
        MyVkImageMemoryBarrier barrier;
        memset(&barrier, 0, sizeof(barrier));
        barrier.sType = VK_STYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = 0xFFFFFFFF;
        barrier.dstQueueFamilyIndex = 0xFFFFFFFF;
        barrier.image = dumpImage;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;

        pfn_vkCmdPipelineBarrier(cmdBuf,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, NULL, 0, NULL, 1, &barrier);

        /* Clear with RED (B8G8R8A8: B=0, G=0, R=1.0, A=1.0) */
        MyVkClearColorValue clearColor;
        memset(&clearColor, 0, sizeof(clearColor));
        clearColor.float32[0] = 1.0f; /* R */
        clearColor.float32[3] = 1.0f; /* A */

        MyVkImageSubresourceRange range;
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.baseMipLevel = 0;
        range.levelCount = 1;
        range.baseArrayLayer = 0;
        range.layerCount = 1;

        pfn_vkCmdClearColorImage(cmdBuf, dumpImage,
            VK_IMAGE_LAYOUT_GENERAL, &clearColor, 1, &range);

        /* Barrier: GENERAL → TRANSFER_SRC_OPTIMAL */
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

        pfn_vkCmdPipelineBarrier(cmdBuf,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, NULL, 0, NULL, 1, &barrier);

        /* CopyImageToBuffer */
        MyVkBufferImageCopy region;
        memset(&region, 0, sizeof(region));
        region.bufferOffset = 0;
        region.bufferRowLength = 0;    /* tightly packed */
        region.bufferImageHeight = 0;  /* tightly packed */
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageExtentW = DUMP_W;
        region.imageExtentH = DUMP_H;
        region.imageExtentD = 1;

        pfn_vkCmdCopyImageToBuffer(cmdBuf, dumpImage,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            dumpBuffer, 1, &region);

        OK_OR_DIE(pfn_vkEndCommandBuffer(cmdBuf), "EndCmdBuf");

        /* Submit + wait */
        fprintf(stderr, "[stage7] Submitting GPU work...\n"); fflush(stderr);
        OK_OR_DIE(pfn_vkResetFences(device, 1, &fence), "ResetFences");

        MyVkSubmitInfo si;
        memset(&si, 0, sizeof(si));
        si.sType = VK_STYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmdBuf;

        OK_OR_DIE(pfn_vkQueueSubmit(queue, 1, &si, fence), "QueueSubmit");
        r = pfn_vkWaitForFences(device, 1, &fence, 1, 5000000000ULL);
        fprintf(stderr, "[stage7] WaitForFences: %d\n", r); fflush(stderr);
        if (r != VK_SUCCESS) {
            fprintf(stderr, "[stage7] FAIL — WaitForFences = %d\n", r); fflush(stderr);
            goto cleanup;
        }

        /* Map buffer and verify pixels */
        void *mapped = NULL;
        r = pfn_vkMapMemory(device, dumpBufferMem, 0, bufSize, 0, &mapped);
        fprintf(stderr, "[stage7] MapMemory: result=%d mapped=%p\n", r, mapped);
        fflush(stderr);
        if (r != VK_SUCCESS || !mapped) {
            fprintf(stderr, "[stage7] FAIL — MapMemory\n"); fflush(stderr);
            goto cleanup;
        }

        /* Verify: B8G8R8A8_UNORM with red clear → each pixel should be (B=0, G=0, R=255, A=255)
         * Byte layout: [B, G, R, A] = [0x00, 0x00, 0xFF, 0xFF] */
        uint8_t *pixels = (uint8_t *)mapped;
        int pixelOk = 1;
        for (int check = 0; check < 16; check++) {
            int px = check * (DUMP_W * DUMP_H / 16) * 4;
            uint8_t b = pixels[px+0], g = pixels[px+1], rv = pixels[px+2], a = pixels[px+3];
            if (check < 4) {
                fprintf(stderr, "[stage7] Pixel[%d]: B=%u G=%u R=%u A=%u %s\n",
                        check * (DUMP_W * DUMP_H / 16), b, g, rv, a,
                        (b == 0 && g == 0 && rv == 255 && a == 255) ? "OK" : "WRONG");
            }
            if (rv < 250 || b > 5 || g > 5 || a < 250) pixelOk = 0;
        }
        fflush(stderr);

        /* Write PPM file (P6 binary format) to /tmp/vulkan_dump.ppm */
        {
            const char *ppmPath = "Z:\\tmp\\vulkan_dump.ppm";
            FILE *f = fopen(ppmPath, "wb");
            if (f) {
                fprintf(f, "P6\n%d %d\n255\n", DUMP_W, DUMP_H);
                /* Convert B8G8R8A8 → R8G8B8 */
                for (int y = 0; y < DUMP_H; y++) {
                    for (int x = 0; x < DUMP_W; x++) {
                        int off = (y * DUMP_W + x) * 4;
                        uint8_t rgb[3] = { pixels[off+2], pixels[off+1], pixels[off+0] };
                        fwrite(rgb, 1, 3, f);
                    }
                }
                fclose(f);
                fprintf(stderr, "[stage7] PPM written to /tmp/vulkan_dump.ppm (%dx%d)\n", DUMP_W, DUMP_H);
            } else {
                fprintf(stderr, "[stage7] WARNING: Could not write PPM (fopen failed: %d)\n", errno);
            }
            fflush(stderr);
        }

        pfn_vkUnmapMemory(device, dumpBufferMem);

        if (pixelOk) {
            fprintf(stderr, "[stage7] PASS — rendering verified (red clear, 16 sample pixels OK)\n");
        } else {
            fprintf(stderr, "[stage7] FAIL — pixel verification failed (expected red)\n");
        }
        fflush(stderr);
    }

    /* ===== Cleanup ===== */
cleanup:
    fprintf(stderr, "\n[test] Cleanup...\n"); fflush(stderr);

    if (device) {
        pfn_vkDeviceWaitIdle(device);
        /* Stage 7 resources */
        if (dumpBuffer != VK_NULL_HANDLE && pfn_vkDestroyBuffer)
            pfn_vkDestroyBuffer(device, dumpBuffer, NULL);
        if (dumpBufferMem != VK_NULL_HANDLE && pfn_vkFreeMemory)
            pfn_vkFreeMemory(device, dumpBufferMem, NULL);
        if (dumpImage != VK_NULL_HANDLE && pfn_vkDestroyImage)
            pfn_vkDestroyImage(device, dumpImage, NULL);
        if (dumpImageMem != VK_NULL_HANDLE && pfn_vkFreeMemory)
            pfn_vkFreeMemory(device, dumpImageMem, NULL);
        /* General resources */
        if (cmdPool)                        pfn_vkDestroyCommandPool(device, cmdPool, NULL);
        if (fence != VK_NULL_HANDLE)        pfn_vkDestroyFence(device, fence, NULL);
        if (renderSem != VK_NULL_HANDLE)    pfn_vkDestroySemaphore(device, renderSem, NULL);
        if (acquireSem != VK_NULL_HANDLE)   pfn_vkDestroySemaphore(device, acquireSem, NULL);
        if (swapchain != VK_NULL_HANDLE && pfn_vkDestroySwapchainKHR)
            pfn_vkDestroySwapchainKHR(device, swapchain, NULL);
        pfn_vkDestroyDevice(device, NULL);
    }
    if (surface != VK_NULL_HANDLE && pfn_vkDestroySurfaceKHR)
        pfn_vkDestroySurfaceKHR(instance, surface, NULL);
    if (instance)
        pfn_vkDestroyInstance(instance, NULL);
    if (hwnd)
        DestroyWindow(hwnd);

    fprintf(stderr, "\n[test_wine_vulkan] === ALL STAGES COMPLETE ===\n"); fflush(stderr);
    return 0;
}
