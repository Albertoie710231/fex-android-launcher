/*
 * Vulkan Implicit Layer: Headless Surface Bridge
 * ================================================
 *
 * Provides VK_KHR_xcb_surface + VK_KHR_xlib_surface + VK_KHR_swapchain for
 * Wine/DXVK on FEX-Emu. Intercepts XCB/Xlib surface creation and emulates
 * swapchain with CPU readback + TCP frame sending to FrameSocketServer on Android.
 *
 * Rendering pipeline:
 *   Game -> DXVK (DX11->Vulkan) -> winevulkan (win32->xlib/xcb surface)
 *   -> THIS LAYER (xlib/xcb->headless, swapchain->frame capture)
 *   -> ICD (Vortek via FEX thunks -> Mali GPU)
 *   -> TCP 19850 -> FrameSocketServer -> Android SurfaceView
 *
 * Why a layer instead of LD_PRELOAD:
 *   Wine's preloader breaks LD_PRELOAD — the guest ld.so cannot open the .so
 *   file during early startup. A Vulkan layer is loaded later via dlopen()
 *   by the Vulkan loader, which works fine inside FEX.
 *
 * Enable:  export HEADLESS_LAYER=1
 * Disable: export DISABLE_HEADLESS_LAYER=1
 *
 * Build: gcc -shared -fPIC -o libvulkan_headless_layer.so vulkan_headless_layer.c
 *        -lpthread -ldl -fcf-protection=none
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <dlfcn.h>
#include <signal.h>
#include <sys/syscall.h>

/* ============================================================================
 * Section 1: Vulkan Types and Constants (inline, no SDK headers needed)
 * ============================================================================ */

typedef uint32_t VkFlags;
typedef uint32_t VkBool32;
typedef uint64_t VkDeviceSize;

#define VK_TRUE  1
#define VK_FALSE 0
#define VK_SUCCESS 0
#define VK_INCOMPLETE 5
#define VK_NOT_READY 1
#define VK_SUBOPTIMAL_KHR 1000001003
#define VK_ERROR_OUT_OF_HOST_MEMORY (-1)
#define VK_ERROR_INITIALIZATION_FAILED (-3)
#define VK_ERROR_EXTENSION_NOT_PRESENT (-7)
#define VK_MAX_EXTENSION_NAME_SIZE 256

#define VK_FORMAT_B8G8R8A8_UNORM 44
#define VK_COLOR_SPACE_SRGB_NONLINEAR_KHR 0
#define VK_PRESENT_MODE_FIFO_KHR 2
#define VK_PRESENT_MODE_IMMEDIATE_KHR 0

#define VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR 0x00000001
#define VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR 0x00000001
#define VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT 0x00000010
#define VK_IMAGE_USAGE_TRANSFER_SRC_BIT 0x00000001
#define VK_IMAGE_USAGE_TRANSFER_DST_BIT 0x00000002

#define VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR 1000005000
#define VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT 1000256000
#define VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO 1
#define VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO 14
#define VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO 5
#define VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR 1000001000
#define VK_STRUCTURE_TYPE_PRESENT_INFO_KHR 1000001001

/* Layer protocol sTypes */
#define VK_STRUCTURE_TYPE_SUBMIT_INFO 4
#define VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO 47
#define VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO 48

#define VK_IMAGE_TYPE_2D 1
#define VK_SAMPLE_COUNT_1_BIT 1
#define VK_IMAGE_TILING_LINEAR 1
#define VK_SHARING_MODE_EXCLUSIVE 0
#define VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT 0x01
#define VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT 0x02
#define VK_MEMORY_PROPERTY_HOST_COHERENT_BIT 0x04
#define VK_IMAGE_TILING_OPTIMAL 0
#define VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO 12
#define VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO 39
#define VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO 40
#define VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO 42
#define VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER 46
#define VK_BUFFER_USAGE_TRANSFER_DST_BIT 0x00000002
#define VK_COMMAND_BUFFER_LEVEL_PRIMARY 0
#define VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT 0x00000002
#define VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT 0x00000001
#define VK_IMAGE_LAYOUT_UNDEFINED 0
#define VK_IMAGE_LAYOUT_GENERAL 1
#define VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL 6
#define VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL 7
#define VK_IMAGE_LAYOUT_PRESENT_SRC_KHR 1000001002
#define VK_IMAGE_ASPECT_COLOR_BIT 0x00000001
#define VK_ACCESS_TRANSFER_READ_BIT 0x00000800
#define VK_ACCESS_TRANSFER_WRITE_BIT 0x00001000
#define VK_ACCESS_MEMORY_READ_BIT 0x00008000
#define VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT 0x00000100
#define VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT 0x00000400
#define VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT 0x00000001
#define VK_PIPELINE_STAGE_TRANSFER_BIT 0x00001000
#define VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT 0x00002000
#define VK_QUEUE_FAMILY_IGNORED 0xFFFFFFFF

typedef int VkResult;
typedef void* VkInstance;
typedef void* VkPhysicalDevice;
typedef void* VkDevice;
typedef void* VkQueue;
typedef uint64_t VkSurfaceKHR;
typedef uint64_t VkSwapchainKHR;
typedef uint64_t VkImage;
typedef uint64_t VkSemaphore;
typedef uint64_t VkFence;
typedef uint64_t VkDeviceMemory;
typedef uint64_t VkImageView;
typedef void (*PFN_vkVoidFunction)(void);
typedef void VkAllocationCallbacks;

typedef void* VkCommandBuffer;
typedef void* VkCommandPool;
typedef uint64_t VkBuffer;

/* Structures for staging buffer readback (OPTIMAL → CPU) */
typedef struct VkBufferCreateInfo {
    int sType; const void* pNext; VkFlags flags;
    VkDeviceSize size; VkFlags usage; int sharingMode;
    uint32_t queueFamilyIndexCount; const uint32_t* pQueueFamilyIndices;
} VkBufferCreateInfo;

typedef struct VkImageSubresourceLayers {
    uint32_t aspectMask; uint32_t mipLevel;
    uint32_t baseArrayLayer; uint32_t layerCount;
} VkImageSubresourceLayers;

typedef struct VkBufferImageCopy {
    VkDeviceSize bufferOffset; uint32_t bufferRowLength; uint32_t bufferImageHeight;
    VkImageSubresourceLayers imageSubresource;
    struct { int32_t x, y, z; } imageOffset;
    struct { uint32_t width, height, depth; } imageExtent;
} VkBufferImageCopy;

typedef struct VkImageSubresourceRange {
    VkFlags aspectMask; uint32_t baseMipLevel; uint32_t levelCount;
    uint32_t baseArrayLayer; uint32_t layerCount;
} VkImageSubresourceRange;

typedef struct VkImageMemoryBarrier {
    int sType; const void* pNext;
    VkFlags srcAccessMask; VkFlags dstAccessMask;
    int oldLayout; int newLayout;
    uint32_t srcQueueFamilyIndex; uint32_t dstQueueFamilyIndex;
    VkImage image;
    VkImageSubresourceRange subresourceRange;
} VkImageMemoryBarrier;

typedef struct VkCommandPoolCreateInfo_t {
    int sType; const void* pNext; VkFlags flags;
    uint32_t queueFamilyIndex;
} VkCommandPoolCreateInfo_t;

typedef struct VkCommandBufferAllocateInfo_t {
    int sType; const void* pNext;
    VkCommandPool commandPool; int level; uint32_t commandBufferCount;
} VkCommandBufferAllocateInfo_t;

typedef struct VkCommandBufferBeginInfo_t {
    int sType; const void* pNext; VkFlags flags;
    const void* pInheritanceInfo;
} VkCommandBufferBeginInfo_t;

typedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddr)(VkInstance, const char*);
typedef PFN_vkVoidFunction (*PFN_vkGetDeviceProcAddr)(VkDevice, const char*);

/* Command buffer function pointers for diagnostic interception */
typedef VkResult (*PFN_vkBeginCommandBuffer)(VkCommandBuffer, const void*);
typedef VkResult (*PFN_vkEndCommandBuffer)(VkCommandBuffer);
typedef VkResult (*PFN_vkAllocateCommandBuffers)(VkDevice, const void*, VkCommandBuffer*);
typedef VkResult (*PFN_vkQueueSubmit)(VkQueue, uint32_t, const void*, uint64_t);
typedef VkResult (*PFN_vkCreateCommandPool)(VkDevice, const void*, const void*, VkCommandPool*);

typedef struct VkExtent2D { uint32_t width; uint32_t height; } VkExtent2D;

typedef struct VkExtensionProperties {
    char extensionName[VK_MAX_EXTENSION_NAME_SIZE];
    uint32_t specVersion;
} VkExtensionProperties;

typedef struct VkSurfaceCapabilitiesKHR {
    uint32_t minImageCount;
    uint32_t maxImageCount;
    VkExtent2D currentExtent;
    VkExtent2D minImageExtent;
    VkExtent2D maxImageExtent;
    uint32_t maxImageArrayLayers;
    VkFlags supportedTransforms;
    VkFlags currentTransform;
    VkFlags supportedCompositeAlpha;
    VkFlags supportedUsageFlags;
} VkSurfaceCapabilitiesKHR;

typedef struct VkSurfaceFormatKHR { int format; int colorSpace; } VkSurfaceFormatKHR;
typedef int VkPresentModeKHR;

/* VK_KHR_get_surface_capabilities2 structs */
typedef struct VkPhysicalDeviceSurfaceInfo2KHR {
    int sType;          /* VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR = 1000119000 */
    const void* pNext;
    VkSurfaceKHR surface;
} VkPhysicalDeviceSurfaceInfo2KHR;

typedef struct VkSurfaceCapabilities2KHR {
    int sType;          /* VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR = 1000119001 */
    void* pNext;
    VkSurfaceCapabilitiesKHR surfaceCapabilities;
} VkSurfaceCapabilities2KHR;

typedef struct VkSurfaceFormat2KHR {
    int sType;          /* VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR = 1000119002 */
    void* pNext;
    VkSurfaceFormatKHR surfaceFormat;
} VkSurfaceFormat2KHR;

typedef struct VkApplicationInfo {
    int sType; const void* pNext;
    const char* pApplicationName; uint32_t applicationVersion;
    const char* pEngineName; uint32_t engineVersion;
    uint32_t apiVersion;
} VkApplicationInfo;

typedef struct VkInstanceCreateInfo {
    int sType; const void* pNext; VkFlags flags;
    const VkApplicationInfo* pApplicationInfo;
    uint32_t enabledLayerCount;
    const char* const* ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char* const* ppEnabledExtensionNames;
} VkInstanceCreateInfo;

typedef struct VkDeviceQueueCreateInfo {
    int sType; const void* pNext; VkFlags flags;
    uint32_t queueFamilyIndex; uint32_t queueCount;
    const float* pQueuePriorities;
} VkDeviceQueueCreateInfo;

typedef struct VkDeviceCreateInfo {
    int sType; const void* pNext; VkFlags flags;
    uint32_t queueCreateInfoCount;
    const VkDeviceQueueCreateInfo* pQueueCreateInfos;
    uint32_t enabledLayerCount;
    const char* const* ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char* const* ppEnabledExtensionNames;
    const void* pEnabledFeatures;
} VkDeviceCreateInfo;

typedef struct VkXcbSurfaceCreateInfoKHR {
    int sType; const void* pNext; VkFlags flags;
    void* connection; uint32_t window;
} VkXcbSurfaceCreateInfoKHR;

typedef struct VkHeadlessSurfaceCreateInfoEXT {
    int sType; const void* pNext; VkFlags flags;
} VkHeadlessSurfaceCreateInfoEXT;

typedef struct VkSwapchainCreateInfoKHR {
    int sType; const void* pNext; VkFlags flags;
    VkSurfaceKHR surface;
    uint32_t minImageCount; int imageFormat; int imageColorSpace;
    VkExtent2D imageExtent; uint32_t imageArrayLayers;
    VkFlags imageUsage; int imageSharingMode;
    uint32_t queueFamilyIndexCount; const uint32_t* pQueueFamilyIndices;
    VkFlags preTransform; VkFlags compositeAlpha;
    int presentMode; VkBool32 clipped;
    VkSwapchainKHR oldSwapchain;
} VkSwapchainCreateInfoKHR;

typedef struct VkPresentInfoKHR {
    int sType; const void* pNext;
    uint32_t waitSemaphoreCount; const VkSemaphore* pWaitSemaphores;
    uint32_t swapchainCount; const VkSwapchainKHR* pSwapchains;
    const uint32_t* pImageIndices; VkResult* pResults;
} VkPresentInfoKHR;

typedef struct VkSubmitInfo {
    int sType; const void* pNext;
    uint32_t waitSemaphoreCount;
    const VkSemaphore* pWaitSemaphores;
    const VkFlags* pWaitDstStageMask;
    uint32_t commandBufferCount;
    const VkCommandBuffer* pCommandBuffers;
    uint32_t signalSemaphoreCount;
    const VkSemaphore* pSignalSemaphores;
} VkSubmitInfo;

typedef struct VkImageCreateInfo {
    int sType; const void* pNext; VkFlags flags;
    int imageType; int format;
    struct { uint32_t width; uint32_t height; uint32_t depth; } extent;
    uint32_t mipLevels; uint32_t arrayLayers;
    int samples; int tiling; VkFlags usage;
    int sharingMode; uint32_t queueFamilyIndexCount;
    const uint32_t* pQueueFamilyIndices; int initialLayout;
} VkImageCreateInfo;

typedef struct VkMemoryRequirements {
    VkDeviceSize size; VkDeviceSize alignment; uint32_t memoryTypeBits;
} VkMemoryRequirements;

typedef struct VkMemoryAllocateInfo {
    int sType; const void* pNext;
    VkDeviceSize allocationSize; uint32_t memoryTypeIndex;
} VkMemoryAllocateInfo;

typedef struct VkMemoryType { uint32_t propertyFlags; uint32_t heapIndex; } VkMemoryType;
typedef struct VkMemoryHeap { VkDeviceSize size; uint32_t flags; } VkMemoryHeap;
typedef struct VkPhysicalDeviceMemoryProperties {
    uint32_t memoryTypeCount; VkMemoryType memoryTypes[32];
    uint32_t memoryHeapCount; VkMemoryHeap memoryHeaps[16];
} VkPhysicalDeviceMemoryProperties;

typedef struct VkImageSubresource { uint32_t aspectMask; uint32_t mipLevel; uint32_t arrayLayer; } VkImageSubresource;
typedef struct VkSubresourceLayout {
    VkDeviceSize offset; VkDeviceSize size; VkDeviceSize rowPitch;
    VkDeviceSize arrayPitch; VkDeviceSize depthPitch;
} VkSubresourceLayout;

/* Physical device features — full struct needed for textureCompressionBC spoofing */
typedef struct VkPhysicalDeviceFeatures {
    VkBool32 robustBufferAccess;
    VkBool32 fullDrawIndexUint32;
    VkBool32 imageCubeArray;
    VkBool32 independentBlend;
    VkBool32 geometryShader;
    VkBool32 tessellationShader;
    VkBool32 sampleRateShading;
    VkBool32 dualSrcBlend;
    VkBool32 logicOp;
    VkBool32 multiDrawIndirect;
    VkBool32 drawIndirectFirstInstance;
    VkBool32 depthClamp;
    VkBool32 depthBiasClamp;
    VkBool32 fillModeNonSolid;
    VkBool32 depthBounds;
    VkBool32 wideLines;
    VkBool32 largePoints;
    VkBool32 alphaToOne;
    VkBool32 multiViewport;
    VkBool32 samplerAnisotropy;
    VkBool32 textureCompressionETC2;
    VkBool32 textureCompressionASTC_LDR;
    VkBool32 textureCompressionBC;
    VkBool32 occlusionQueryPrecise;
    VkBool32 pipelineStatisticsQuery;
    VkBool32 vertexPipelineStoresAndAtomics;
    VkBool32 fragmentStoresAndAtomics;
    VkBool32 shaderTessellationAndGeometryPointSize;
    VkBool32 shaderImageGatherExtended;
    VkBool32 shaderStorageImageExtendedFormats;
    VkBool32 shaderStorageImageMultisample;
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
    VkBool32 variableMultisampleRate;
    VkBool32 inheritedQueries;
} VkPhysicalDeviceFeatures;

typedef struct VkPhysicalDeviceFeatures2 {
    int sType; void* pNext;
    VkPhysicalDeviceFeatures features;
} VkPhysicalDeviceFeatures2;

typedef struct VkFormatProperties {
    VkFlags linearTilingFeatures;
    VkFlags optimalTilingFeatures;
    VkFlags bufferFeatures;
} VkFormatProperties;

typedef struct VkFormatProperties2 {
    int sType; void* pNext;
    VkFormatProperties formatProperties;
} VkFormatProperties2;

/* BC (S3TC/DXT) texture format range: VK_FORMAT_BC1_RGB_UNORM_BLOCK .. VK_FORMAT_BC7_SRGB_BLOCK */
#define VK_FORMAT_BC1_RGB_UNORM_BLOCK   131
#define VK_FORMAT_BC7_SRGB_BLOCK        146

/* Format feature bits for BC spoofing */
#define VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT                 0x00000001
#define VK_FORMAT_FEATURE_BLIT_SRC_BIT                      0x00000004
#define VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT   0x00001000
#define VK_FORMAT_FEATURE_TRANSFER_SRC_BIT                  0x00004000
#define VK_FORMAT_FEATURE_TRANSFER_DST_BIT                  0x00008000

/* ============================================================================
 * Section 2: Vulkan Layer Protocol Types
 * ============================================================================ */

typedef enum VkLayerFunction_ {
    VK_LAYER_LINK_INFO = 0,
    VK_LOADER_DATA_CALLBACK = 1
} VkLayerFunction;

typedef struct VkLayerInstanceLink_ {
    struct VkLayerInstanceLink_* pNext;
    PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr;
    PFN_vkVoidFunction pfnNextGetPhysicalDeviceProcAddr; /* unused by us */
} VkLayerInstanceLink;

typedef struct VkLayerInstanceCreateInfo_ {
    int sType;
    const void* pNext;
    VkLayerFunction function;
    union {
        VkLayerInstanceLink* pLayerInfo;
        void* pfnSetInstanceLoaderData;
    } u;
} VkLayerInstanceCreateInfo;

typedef struct VkLayerDeviceLink_ {
    struct VkLayerDeviceLink_* pNext;
    PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr pfnNextGetDeviceProcAddr;
} VkLayerDeviceLink;

typedef struct VkLayerDeviceCreateInfo_ {
    int sType;
    const void* pNext;
    VkLayerFunction function;
    union {
        VkLayerDeviceLink* pLayerInfo;
        void* pfnSetDeviceLoaderData;
    } u;
} VkLayerDeviceCreateInfo;

/* Layer negotiation */
typedef enum {
    LAYER_NEGOTIATE_UNINTIALIZED = 0,
    LAYER_NEGOTIATE_INTERFACE_STRUCT = 1
} VkNegotiateLayerStructType;

typedef struct VkNegotiateLayerInterface_ {
    VkNegotiateLayerStructType sType;
    void* pNext;
    uint32_t loaderLayerInterfaceVersion;
    PFN_vkGetInstanceProcAddr pfnGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr pfnGetDeviceProcAddr;
    PFN_vkVoidFunction pfnGetPhysicalDeviceProcAddr;
} VkNegotiateLayerInterface;

/* ============================================================================
 * Section 3: Layer Dispatch State
 * ============================================================================ */

/* Next-layer function pointers (saved during vkCreateInstance/vkCreateDevice) */
static PFN_vkGetInstanceProcAddr g_next_gipa = NULL;
static PFN_vkGetDeviceProcAddr g_next_gdpa = NULL;

/* Per-device dispatch table — needed because DXVK creates multiple devices
 * (probe device + real device) and destroying the probe clears globals. */
#define MAX_LAYER_DEVICES 8
static struct {
    VkDevice device;
    PFN_vkGetDeviceProcAddr gdpa;
} g_device_table[MAX_LAYER_DEVICES];
static int g_device_count = 0;

/* Diagnostic: Vulkan command buffer interception to find where vkBeginCommandBuffer hangs */
static PFN_vkBeginCommandBuffer g_real_BeginCmdBuf = NULL;
static PFN_vkEndCommandBuffer g_real_EndCmdBuf = NULL;
static PFN_vkAllocateCommandBuffers g_real_AllocCmdBufs = NULL;
static PFN_vkQueueSubmit g_real_QueueSubmit = NULL;
static PFN_vkCreateCommandPool g_real_CreateCmdPool = NULL;
static volatile int g_beginCmdBuf_count = 0;
static volatile int g_endCmdBuf_count = 0;
static VkInstance g_instance = NULL;
static VkDevice g_device = NULL;
static VkPhysicalDevice g_physical_device = NULL;
static int g_instance_count = 0; /* tracks how many CreateInstance calls succeeded */

/* Real function pointers for feature/format spoofing (resolved in CreateInstance) */
typedef void (*PFN_GetFeatures)(VkPhysicalDevice, VkPhysicalDeviceFeatures*);
typedef void (*PFN_GetFeatures2)(VkPhysicalDevice, VkPhysicalDeviceFeatures2*);
typedef void (*PFN_GetFormatProps)(VkPhysicalDevice, int, VkFormatProperties*);
typedef void (*PFN_GetFormatProps2)(VkPhysicalDevice, int, VkFormatProperties2*);
static PFN_GetFeatures g_real_get_features = NULL;
static PFN_GetFeatures2 g_real_get_features2 = NULL;
static PFN_GetFormatProps g_real_get_format_props = NULL;
static PFN_GetFormatProps2 g_real_get_format_props2 = NULL;

/* Logging */
#define LOG_TAG "[HeadlessLayer] "
#define LOG(...) do { fprintf(stderr, LOG_TAG __VA_ARGS__); fflush(stderr); } while(0)

/* File-based debug markers — survives even if stderr is lost */
static void layer_marker(const char* msg) {
    FILE* f = fopen("/tmp/layer_trace.log", "a");
    if (f) { fprintf(f, "%s\n", msg); fclose(f); }
}

/* Global call tracker — identifies last Vulkan function called before crash */
static volatile const char* g_last_fn = "none";
static volatile int g_call_seq = 0;

#define TRACE_FN(name) do { \
    g_last_fn = name; \
    int _seq = __sync_add_and_fetch(&g_call_seq, 1); \
    long _tid = syscall(SYS_gettid); \
    char _tb[160]; snprintf(_tb, sizeof(_tb), "[%d] T%ld " name, _seq, _tid); \
    layer_marker(_tb); \
} while(0)

/* SIGABRT handler — Wine's _wassert calls abort() which raises SIGABRT.
 *
 * Wine 10's loader.c has assert(!status) after every UNIX_CALL. If the
 * unix-side Vulkan handler crashes (e.g. from a driver issue), status is
 * non-zero and the assert fires. This is a known issue (Proton #7323)
 * that kills the entire process even though only one thread is affected.
 *
 * FIX: Use syscall(SYS_exit, 0) to terminate ONLY the offending thread.
 * SYS_exit (60) kills just the calling thread; SYS_exit_group (231) would
 * kill the whole process. The DXVK rendering thread survives and the game
 * can continue.
 *
 * Risk: Thread 0090 might hold Wine locks. If so, other threads will
 * deadlock on those locks. But empirically, the game progresses further
 * than it does with the assertion killing the whole process. */
static void sigabrt_handler(int sig) {
    (void)sig;
    long tid = syscall(SYS_gettid);
    FILE* f = fopen("/tmp/vk_abort_info.log", "a");
    if (f) {
        fprintf(f, "SIGABRT caught on thread %ld! Killing ONLY this thread.\n", tid);
        fprintf(f, "Last Vulkan function: %s\n", (const char*)g_last_fn);
        fprintf(f, "Call sequence: %d\n", g_call_seq);
        fclose(f);
    }
    /* Log to stderr too */
    fprintf(stderr, LOG_TAG "SIGABRT on T%ld — killing thread only (last fn: %s)\n",
            tid, (const char*)g_last_fn);
    fflush(stderr);
    /* Kill ONLY this thread, not the whole process.
     * SYS_exit = 60 on x86-64. Does NOT call atexit handlers or _exit(). */
    syscall(SYS_exit, 0);
}

/* ============================================================================
 * Section 4: TCP Frame Socket (frame capture → FrameSocketServer)
 * ============================================================================ */

#define FRAME_SOCKET_PORT 19850
#define TARGET_FRAME_NS (8333333ULL)  /* ~120 FPS */

static int g_frame_socket = -1;
static int g_frame_connected = 0;
static uint64_t g_last_present_ns = 0;

static uint8_t* g_pending_buf = NULL;
static size_t g_pending_cap = 0;
static size_t g_pending_total = 0;
static size_t g_pending_sent = 0;

/* Dump mode: write first N presented frames as PPM files to /tmp/ */
static int g_dump_max_frames = 0;   /* 0=disabled, >0=dump first N frames */
static int g_dump_frame_count = 0;  /* frames dumped so far */
static int g_dump_mode = 0;         /* 1=active (skip TCP) */
static FILE* g_dump_summary = NULL; /* /tmp/frame_summary.txt */

static uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static int connect_frame_socket(void) {
    if (g_frame_connected) return 1;

    g_frame_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (g_frame_socket < 0) return 0;

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(FRAME_SOCKET_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(g_frame_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        static int err_count = 0;
        if (err_count++ < 3)
            LOG("Failed to connect to frame socket port %d: %s\n", FRAME_SOCKET_PORT, strerror(errno));
        close(g_frame_socket);
        g_frame_socket = -1;
        return 0;
    }

    int flags = fcntl(g_frame_socket, F_GETFL, 0);
    fcntl(g_frame_socket, F_SETFL, flags | O_NONBLOCK);
    int nodelay = 1;
    setsockopt(g_frame_socket, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    int sndbuf = 4 * 1024 * 1024;
    setsockopt(g_frame_socket, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    g_frame_connected = 1;
    g_pending_total = g_pending_sent = 0;
    LOG("Connected to frame socket on port %d\n", FRAME_SOCKET_PORT);
    return 1;
}

static void disconnect_frame_socket(void) {
    if (g_frame_socket >= 0) close(g_frame_socket);
    g_frame_socket = -1;
    g_frame_connected = 0;
    g_pending_total = g_pending_sent = 0;
}

static int drain_pending(void) {
    while (g_pending_sent < g_pending_total) {
        ssize_t n = write(g_frame_socket, g_pending_buf + g_pending_sent,
                          g_pending_total - g_pending_sent);
        if (n > 0) { g_pending_sent += n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (n < 0 && errno == EINTR) continue;
        return -1;
    }
    g_pending_total = g_pending_sent = 0;
    return 1;
}

static void send_frame(uint32_t width, uint32_t height, const void* pixels, size_t row_pitch) {
    if (!g_frame_connected && !connect_frame_socket()) return;

    if (g_pending_total > 0) {
        int r = drain_pending();
        if (r < 0) { disconnect_frame_socket(); return; }
        if (r == 0) return; /* drop frame */
    }

    size_t expected_pitch = width * 4;
    size_t pixel_size = width * height * 4;
    size_t frame_size = 8 + pixel_size;

    if (g_pending_cap < frame_size) {
        free(g_pending_buf);
        g_pending_buf = malloc(frame_size);
        g_pending_cap = frame_size;
    }

    uint32_t header[2] = { width, height };
    memcpy(g_pending_buf, header, 8);

    if (row_pitch == expected_pitch) {
        memcpy(g_pending_buf + 8, pixels, pixel_size);
    } else {
        uint8_t* dst = g_pending_buf + 8;
        const uint8_t* src = pixels;
        for (uint32_t y = 0; y < height; y++) {
            memcpy(dst, src, expected_pitch);
            dst += expected_pitch;
            src += row_pitch;
        }
    }

    g_pending_total = frame_size;
    g_pending_sent = 0;
    if (drain_pending() < 0) disconnect_frame_socket();
}

/* ============================================================================
 * Section 5: Surface Tracking
 * ============================================================================ */

typedef struct SurfaceEntry {
    VkSurfaceKHR handle;
    uint32_t width;
    uint32_t height;
    struct SurfaceEntry* next;
} SurfaceEntry;

static SurfaceEntry* g_surfaces = NULL;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_next_handle = 0xBEEF000000000001ULL;

static SurfaceEntry* find_surface(VkSurfaceKHR handle) {
    pthread_mutex_lock(&g_mutex);
    for (SurfaceEntry* s = g_surfaces; s; s = s->next) {
        if (s->handle == handle) { pthread_mutex_unlock(&g_mutex); return s; }
    }
    pthread_mutex_unlock(&g_mutex);
    return NULL;
}

static SurfaceEntry* add_surface(uint32_t w, uint32_t h) {
    SurfaceEntry* e = calloc(1, sizeof(SurfaceEntry));
    if (!e) return NULL;
    e->handle = g_next_handle++;
    e->width = w;
    e->height = h;
    pthread_mutex_lock(&g_mutex);
    e->next = g_surfaces;
    g_surfaces = e;
    pthread_mutex_unlock(&g_mutex);
    return e;
}

static void remove_surface(VkSurfaceKHR handle) {
    pthread_mutex_lock(&g_mutex);
    SurfaceEntry** pp = &g_surfaces;
    while (*pp) {
        if ((*pp)->handle == handle) {
            SurfaceEntry* f = *pp;
            *pp = f->next;
            free(f);
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_mutex);
}

/* ============================================================================
 * Section 6: Swapchain Tracking
 * ============================================================================ */

#define MAX_SC_IMAGES 8

typedef struct SwapchainEntry {
    VkSwapchainKHR handle;
    VkSurfaceKHR surface;
    VkDevice device;
    uint32_t image_count;
    VkImage images[MAX_SC_IMAGES];
    VkDeviceMemory memory[MAX_SC_IMAGES];
    VkDeviceSize row_pitch[MAX_SC_IMAGES];
    uint32_t width, height;
    int format;
    uint32_t current_image;
    VkQueue signal_queue;           /* for signaling acquire semaphore/fence */
    /* Staging buffer for OPTIMAL image → CPU readback */
    VkBuffer staging_buf;
    VkDeviceMemory staging_mem;
    VkDeviceSize staging_size;
    VkCommandPool copy_pool;
    VkCommandBuffer copy_cmd;
    struct SwapchainEntry* next;
} SwapchainEntry;

static SwapchainEntry* g_swapchains = NULL;
static uint64_t g_next_sc = 0xDEAD000000000001ULL;

static SwapchainEntry* find_swapchain(VkSwapchainKHR h) {
    pthread_mutex_lock(&g_mutex);
    for (SwapchainEntry* s = g_swapchains; s; s = s->next) {
        if (s->handle == h) { pthread_mutex_unlock(&g_mutex); return s; }
    }
    pthread_mutex_unlock(&g_mutex);
    return NULL;
}

static int is_our_swapchain(VkSwapchainKHR h) {
    return (h & 0xFFFF000000000000ULL) == 0xDEAD000000000000ULL;
}

/* Memory properties cache */
static VkPhysicalDeviceMemoryProperties g_mem_props = {0};
static int g_mem_props_queried = 0;

/* ============================================================================
 * Section 7: Helper — get function pointer from next layer
 * ============================================================================ */

static PFN_vkVoidFunction next_instance_proc(const char* name) {
    if (g_next_gipa && g_instance)
        return g_next_gipa(g_instance, name);
    return NULL;
}

/* Look up GDPA for a specific device from the per-device table */
static PFN_vkGetDeviceProcAddr gdpa_for_device(VkDevice device) {
    for (int i = 0; i < g_device_count; i++) {
        if (g_device_table[i].device == device)
            return g_device_table[i].gdpa;
    }
    /* Fallback to global (last-known) GDPA */
    return g_next_gdpa;
}

/* Resolve device function using specific device's dispatch chain */
static PFN_vkVoidFunction next_device_proc_for(VkDevice device, const char* name) {
    PFN_vkGetDeviceProcAddr gdpa = gdpa_for_device(device);
    if (gdpa && device)
        return gdpa(device, name);
    return NULL;
}

/* Legacy: resolve using any known device (for code without a device param) */
static PFN_vkVoidFunction next_device_proc(const char* name) {
    /* Try global first */
    if (g_next_gdpa && g_device)
        return g_next_gdpa(g_device, name);
    /* Fallback: try any device in the table */
    for (int i = 0; i < g_device_count; i++) {
        if (g_device_table[i].device && g_device_table[i].gdpa) {
            PFN_vkVoidFunction fn = g_device_table[i].gdpa(g_device_table[i].device, name);
            if (fn) return fn;
        }
    }
    return NULL;
}

/* ============================================================================
 * Section 7b: Passthrough with logging for physical device enumeration
 * ============================================================================ */

static VkResult headless_EnumeratePhysicalDevices(
    VkInstance instance, uint32_t* pCount, VkPhysicalDevice* pDevices)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "EnumPD_ENTER inst=%p g_inst=%p pDev=%p",
             instance, g_instance, (void*)pDevices);
    LOG("%s\n", buf);
    layer_marker(buf);

    typedef VkResult (*PFN)(VkInstance, uint32_t*, VkPhysicalDevice*);
    PFN fn = (PFN)next_instance_proc("vkEnumeratePhysicalDevices");
    if (!fn) {
        LOG("ERROR: vkEnumeratePhysicalDevices not found in next layer!\n");
        layer_marker("EnumPD_fn_NULL");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    snprintf(buf, sizeof(buf), "EnumPD_CALL fn=%p g_instance=%p", (void*)fn, g_instance);
    LOG("%s\n", buf);
    layer_marker(buf);

    /* Use g_instance (ICD's handle) — the function pointer was resolved for g_instance */
    VkResult res = fn(g_instance, pCount, pDevices);

    snprintf(buf, sizeof(buf), "EnumPD_RETURN res=%d count=%u", res, pCount ? *pCount : 0);
    LOG("%s\n", buf);
    layer_marker(buf);

    if (res == VK_SUCCESS && pDevices && *pCount > 0) {
        g_physical_device = pDevices[0];
        LOG("Saved physical device: %p\n", pDevices[0]);
    }
    return res;
}

static VkResult headless_GetPhysicalDeviceProperties(
    VkPhysicalDevice pd, void* pProperties)
{
    LOG("vkGetPhysicalDeviceProperties called (pd=%p)\n", pd);
    typedef void (*PFN)(VkPhysicalDevice, void*);
    PFN fn = (PFN)next_instance_proc("vkGetPhysicalDeviceProperties");
    if (fn) fn(pd, pProperties);
    LOG("vkGetPhysicalDeviceProperties done\n");
    return VK_SUCCESS;
}

/* ============================================================================
 * Section 7c: Physical Device Feature & Format Spoofing
 *
 * Spoof features that DXVK requires but the thunk chain may not expose:
 * - textureCompressionBC: Mali doesn't support BC natively, but Vortek may
 *   transcode BC→ASTC/RGBA internally. DXVK requires this to accept device.
 * - depthClipEnable (VK_EXT_depth_clip_enable): Required for D3D11's
 *   DepthClipEnable rasterizer state. Mali supports this natively but
 *   FEX thunks may not expose the extension.
 * - customBorderColors (VK_EXT_custom_border_color): Required for D3D11
 *   sampler border colors.
 * ============================================================================ */

/* sType values for pNext chain feature structs */
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT 1000102000
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT 1000287002
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT 1000028000
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT 1000286000

/* Generic pNext chain walker: find a struct by sType */
static void* find_pnext(void* pFeatures, int target_stype) {
    /* pFeatures points to VkPhysicalDeviceFeatures2 or similar with {sType, pNext, ...} */
    typedef struct { int sType; void* pNext; } VkBaseOutStructure;
    VkBaseOutStructure* s = (VkBaseOutStructure*)pFeatures;
    s = (VkBaseOutStructure*)s->pNext; /* skip the root struct */
    while (s) {
        if (s->sType == target_stype)
            return s;
        s = (VkBaseOutStructure*)s->pNext;
    }
    return NULL;
}

/* Feature struct layouts for spoofing (only the fields we need) */
typedef struct {
    int sType;
    void* pNext;
    VkBool32 depthClipEnable;
} VkPhysicalDeviceDepthClipEnableFeaturesEXT;

typedef struct {
    int sType;
    void* pNext;
    VkBool32 customBorderColors;
    VkBool32 customBorderColorWithoutFormatFeature;
} VkPhysicalDeviceCustomBorderColorFeaturesEXT;

typedef struct {
    int sType;
    void* pNext;
    VkBool32 transformFeedback;
    VkBool32 geometryStreams;
} VkPhysicalDeviceTransformFeedbackFeaturesEXT;

typedef struct {
    int sType;
    void* pNext;
    VkBool32 robustBufferAccess2;
    VkBool32 robustImageAccess2;
    VkBool32 nullDescriptor;
} VkPhysicalDeviceRobustness2FeaturesEXT;

typedef struct {
    int sType;
    void* pNext;
    VkBool32 maintenance5;
} VkPhysicalDeviceMaintenance5FeaturesKHR;

typedef struct {
    int sType;
    void* pNext;
    VkBool32 maintenance6;
} VkPhysicalDeviceMaintenance6FeaturesKHR;

#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR 1000470000
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR 1000545000

typedef struct {
    int sType;
    void* pNext;
    VkBool32 nonSeamlessCubeMap;
} VkPhysicalDeviceNonSeamlessCubeMapFeaturesEXT;

#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_NON_SEAMLESS_CUBE_MAP_FEATURES_EXT 1000411000

static int is_bc_format(int format) {
    return format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && format <= VK_FORMAT_BC7_SRGB_BLOCK;
}

/* Spoofed BC format features: sampling + linear filter + transfer */
#define BC_FORMAT_FEATURES \
    (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | \
     VK_FORMAT_FEATURE_BLIT_SRC_BIT | \
     VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | \
     VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | \
     VK_FORMAT_FEATURE_TRANSFER_DST_BIT)

static void headless_GetPhysicalDeviceFeatures(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures* pFeatures)
{
    LOG(">>> GetPhysicalDeviceFeatures CALLED pd=%p pF=%p g_real=%p\n",
        physicalDevice, pFeatures, (void*)g_real_get_features);
    layer_marker("CALL_GetFeatures");

    if (g_real_get_features)
        g_real_get_features(physicalDevice, pFeatures);
    else
        LOG("!!! GetPhysicalDeviceFeatures: g_real_get_features is NULL!\n");

    if (pFeatures) {
        LOG("    BC before spoof: %d\n", pFeatures->textureCompressionBC);
        if (!pFeatures->textureCompressionBC) {
            pFeatures->textureCompressionBC = VK_TRUE;
            LOG("Spoofed textureCompressionBC = VK_TRUE\n");
            layer_marker("SPOOF_BC_FEATURES");
        }
        if (!pFeatures->vertexPipelineStoresAndAtomics) {
            pFeatures->vertexPipelineStoresAndAtomics = VK_TRUE;
            LOG("Spoofed vertexPipelineStoresAndAtomics = VK_TRUE\n");
        }
    }
}

static void headless_GetPhysicalDeviceFeatures2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures)
{
    LOG(">>> GetPhysicalDeviceFeatures2 CALLED pd=%p pF=%p g_real=%p\n",
        physicalDevice, pFeatures, (void*)g_real_get_features2);
    layer_marker("CALL_GetFeatures2");

    if (g_real_get_features2)
        g_real_get_features2(physicalDevice, pFeatures);
    else
        LOG("!!! GetPhysicalDeviceFeatures2: g_real_get_features2 is NULL!\n");

    if (pFeatures) {
        LOG("    BC before spoof: %d\n", pFeatures->features.textureCompressionBC);
        if (!pFeatures->features.textureCompressionBC) {
            pFeatures->features.textureCompressionBC = VK_TRUE;
            LOG("Spoofed textureCompressionBC = VK_TRUE (Features2)\n");
            layer_marker("SPOOF_BC_FEATURES2");
        }
        if (!pFeatures->features.vertexPipelineStoresAndAtomics) {
            pFeatures->features.vertexPipelineStoresAndAtomics = VK_TRUE;
            LOG("Spoofed vertexPipelineStoresAndAtomics = VK_TRUE (Features2)\n");
        }

        /* Walk pNext chain to spoof extension features DXVK requires */
        VkPhysicalDeviceDepthClipEnableFeaturesEXT* dce =
            (VkPhysicalDeviceDepthClipEnableFeaturesEXT*)find_pnext(pFeatures,
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT);
        if (dce && !dce->depthClipEnable) {
            dce->depthClipEnable = VK_TRUE;
            LOG("Spoofed depthClipEnable = VK_TRUE\n");
        }

        VkPhysicalDeviceCustomBorderColorFeaturesEXT* cbc =
            (VkPhysicalDeviceCustomBorderColorFeaturesEXT*)find_pnext(pFeatures,
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT);
        if (cbc) {
            if (!cbc->customBorderColors) {
                cbc->customBorderColors = VK_TRUE;
                LOG("Spoofed customBorderColors = VK_TRUE\n");
            }
            if (!cbc->customBorderColorWithoutFormatFeature) {
                cbc->customBorderColorWithoutFormatFeature = VK_TRUE;
                LOG("Spoofed customBorderColorWithoutFormatFeature = VK_TRUE\n");
            }
        }

        VkPhysicalDeviceTransformFeedbackFeaturesEXT* tfb =
            (VkPhysicalDeviceTransformFeedbackFeaturesEXT*)find_pnext(pFeatures,
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT);
        if (tfb) {
            if (!tfb->transformFeedback) {
                tfb->transformFeedback = VK_TRUE;
                LOG("Spoofed transformFeedback = VK_TRUE\n");
            }
            if (!tfb->geometryStreams) {
                tfb->geometryStreams = VK_TRUE;
                LOG("Spoofed geometryStreams = VK_TRUE\n");
            }
        }

        VkPhysicalDeviceRobustness2FeaturesEXT* rb2 =
            (VkPhysicalDeviceRobustness2FeaturesEXT*)find_pnext(pFeatures,
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT);
        if (rb2) {
            /* DISABLED: Don't spoof robustness2/nullDescriptor.
             * These cause DXVK to take code paths that may crash when the
             * real driver doesn't support them. DXVK falls back gracefully
             * when these are FALSE (creates dummy resources instead of
             * using VK_NULL_HANDLE descriptors). */
            LOG("robustness2: robustBuf=%d robustImg=%d nullDesc=%d (NOT spoofed)\n",
                rb2->robustBufferAccess2, rb2->robustImageAccess2, rb2->nullDescriptor);
        }

        VkPhysicalDeviceMaintenance5FeaturesKHR* m5 =
            (VkPhysicalDeviceMaintenance5FeaturesKHR*)find_pnext(pFeatures,
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR);
        if (m5) {
            if (!m5->maintenance5) {
                m5->maintenance5 = VK_TRUE;
                LOG("Spoofed maintenance5 = VK_TRUE\n");
            }
        }

        VkPhysicalDeviceMaintenance6FeaturesKHR* m6 =
            (VkPhysicalDeviceMaintenance6FeaturesKHR*)find_pnext(pFeatures,
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR);
        if (m6) {
            if (!m6->maintenance6) {
                m6->maintenance6 = VK_TRUE;
                LOG("Spoofed maintenance6 = VK_TRUE\n");
            }
        }

        VkPhysicalDeviceNonSeamlessCubeMapFeaturesEXT* nscm =
            (VkPhysicalDeviceNonSeamlessCubeMapFeaturesEXT*)find_pnext(pFeatures,
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_NON_SEAMLESS_CUBE_MAP_FEATURES_EXT);
        if (nscm) {
            if (!nscm->nonSeamlessCubeMap) {
                nscm->nonSeamlessCubeMap = VK_TRUE;
                LOG("Spoofed nonSeamlessCubeMap = VK_TRUE\n");
            }
        }

        /* Log ALL sTypes in pNext chain so we can see what DXVK queries */
        {
            typedef struct { int sType; void* pNext; } BaseS;
            BaseS* s = (BaseS*)pFeatures->pNext;
            int idx = 0;
            while (s) {
                LOG("  pNext[%d] sType=%d (0x%x)\n", idx, s->sType, s->sType);
                s = (BaseS*)s->pNext;
                idx++;
            }
            LOG("  pNext chain total: %d structs\n", idx);
        }
    }
}

static void headless_GetPhysicalDeviceFormatProperties(
    VkPhysicalDevice physicalDevice,
    int format,
    VkFormatProperties* pFormatProperties)
{
    if (is_bc_format(format)) {
        LOG(">>> GetFormatProperties CALLED format=%d (BC!) pd=%p g_real=%p\n",
            format, physicalDevice, (void*)g_real_get_format_props);
    }

    if (g_real_get_format_props)
        g_real_get_format_props(physicalDevice, format, pFormatProperties);

    if (pFormatProperties && is_bc_format(format) &&
        pFormatProperties->optimalTilingFeatures == 0) {
        pFormatProperties->optimalTilingFeatures = BC_FORMAT_FEATURES;
        LOG("Spoofed BC format %d optimal tiling features\n", format);
    }
}

static void headless_GetPhysicalDeviceFormatProperties2(
    VkPhysicalDevice physicalDevice,
    int format,
    VkFormatProperties2* pFormatProperties)
{
    if (is_bc_format(format)) {
        LOG(">>> GetFormatProperties2 CALLED format=%d (BC!) pd=%p g_real=%p\n",
            format, physicalDevice, (void*)g_real_get_format_props2);
    }

    if (g_real_get_format_props2)
        g_real_get_format_props2(physicalDevice, format, pFormatProperties);

    if (pFormatProperties && is_bc_format(format) &&
        pFormatProperties->formatProperties.optimalTilingFeatures == 0) {
        pFormatProperties->formatProperties.optimalTilingFeatures = BC_FORMAT_FEATURES;
        LOG("Spoofed BC format %d optimal tiling features (FP2)\n", format);
    }
}

/* ============================================================================
 * Section 8: Surface Functions
 * ============================================================================ */

static VkResult headless_CreateXcbSurfaceKHR(
    VkInstance instance,
    const VkXcbSurfaceCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSurfaceKHR* pSurface)
{
    (void)instance; (void)pCreateInfo; (void)pAllocator;
    TRACE_FN("vkCreateXcbSurfaceKHR");
    SurfaceEntry* e = add_surface(1920, 1080);
    if (!e) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *pSurface = e->handle;
    char sbuf[128];
    snprintf(sbuf, sizeof(sbuf), "CreateXcbSurface_OK handle=0x%lx", (unsigned long)e->handle);
    layer_marker(sbuf);
    LOG("vkCreateXcbSurfaceKHR -> headless surface 0x%lx (1920x1080)\n", (unsigned long)e->handle);
    return VK_SUCCESS;
}

static VkResult headless_CreateHeadlessSurfaceEXT(
    VkInstance instance,
    const VkHeadlessSurfaceCreateInfoEXT* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSurfaceKHR* pSurface)
{
    (void)instance; (void)pCreateInfo; (void)pAllocator;
    SurfaceEntry* e = add_surface(1920, 1080);
    if (!e) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *pSurface = e->handle;
    LOG("vkCreateHeadlessSurfaceEXT -> surface 0x%lx\n", (unsigned long)e->handle);
    return VK_SUCCESS;
}

static VkBool32 headless_GetPhysicalDeviceXcbPresentationSupportKHR(
    VkPhysicalDevice pd, uint32_t qfi, void* conn, uint32_t vid)
{
    (void)pd; (void)qfi; (void)conn; (void)vid;
    return VK_TRUE;
}

/* Xlib surface — Wine/Proton-GE maps VK_KHR_win32_surface to VK_KHR_xlib_surface */
static VkResult headless_CreateXlibSurfaceKHR(
    VkInstance instance,
    const void* pCreateInfo,  /* VkXlibSurfaceCreateInfoKHR* */
    const VkAllocationCallbacks* pAllocator,
    VkSurfaceKHR* pSurface)
{
    (void)instance; (void)pCreateInfo; (void)pAllocator;
    TRACE_FN("vkCreateXlibSurfaceKHR");
    SurfaceEntry* e = add_surface(1920, 1080);
    if (!e) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *pSurface = e->handle;
    char sbuf[128];
    snprintf(sbuf, sizeof(sbuf), "CreateXlibSurface_OK handle=0x%lx", (unsigned long)e->handle);
    layer_marker(sbuf);
    LOG("vkCreateXlibSurfaceKHR -> headless surface 0x%lx (1920x1080)\n", (unsigned long)e->handle);
    return VK_SUCCESS;
}

static VkBool32 headless_GetPhysicalDeviceXlibPresentationSupportKHR(
    VkPhysicalDevice pd, uint32_t qfi, void* dpy, unsigned long vid)
{
    (void)pd; (void)qfi; (void)dpy; (void)vid;
    return VK_TRUE;
}

static void headless_DestroySurfaceKHR(
    VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks* pAllocator)
{
    TRACE_FN("vkDestroySurfaceKHR");
    if (find_surface(surface)) {
        LOG("DestroySurfaceKHR: headless surface 0x%lx\n", (unsigned long)surface);
        remove_surface(surface);
        return;
    }
    /* Forward unknown surfaces — use g_instance (ICD's handle, not loader's wrapper) */
    typedef void (*PFN)(VkInstance, VkSurfaceKHR, const VkAllocationCallbacks*);
    PFN fn = (PFN)next_instance_proc("vkDestroySurfaceKHR");
    if (fn) fn(g_instance, surface, pAllocator);
}

static VkResult headless_GetPhysicalDeviceSurfaceSupportKHR(
    VkPhysicalDevice pd, uint32_t qfi, VkSurfaceKHR surface, VkBool32* pSupported)
{
    TRACE_FN("vkGetPhysicalDeviceSurfaceSupportKHR");
    if (!g_physical_device) g_physical_device = pd;
    if (find_surface(surface)) { *pSupported = VK_TRUE; return VK_SUCCESS; }
    typedef VkResult (*PFN)(VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32*);
    PFN fn = (PFN)next_instance_proc("vkGetPhysicalDeviceSurfaceSupportKHR");
    if (fn) return fn(pd, qfi, surface, pSupported);
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

static VkResult headless_GetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice pd, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR* caps)
{
    TRACE_FN("vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    SurfaceEntry* e = find_surface(surface);
    if (e) {
        caps->minImageCount = 2;
        caps->maxImageCount = 8;
        caps->currentExtent.width = e->width;
        caps->currentExtent.height = e->height;
        caps->minImageExtent = (VkExtent2D){1, 1};
        caps->maxImageExtent = (VkExtent2D){16384, 16384};
        caps->maxImageArrayLayers = 1;
        caps->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        caps->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        caps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        caps->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        return VK_SUCCESS;
    }
    typedef VkResult (*PFN)(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR*);
    PFN fn = (PFN)next_instance_proc("vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    if (fn) return fn(pd, surface, caps);
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

static VkResult headless_GetPhysicalDeviceSurfaceFormatsKHR(
    VkPhysicalDevice pd, VkSurfaceKHR surface, uint32_t* pCount, VkSurfaceFormatKHR* pFormats)
{
    TRACE_FN("vkGetPhysicalDeviceSurfaceFormatsKHR");
    if (find_surface(surface)) {
        if (!pFormats) { *pCount = 1; return VK_SUCCESS; }
        if (*pCount >= 1) {
            pFormats[0].format = VK_FORMAT_B8G8R8A8_UNORM;
            pFormats[0].colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            *pCount = 1;
        }
        return VK_SUCCESS;
    }
    typedef VkResult (*PFN)(VkPhysicalDevice, VkSurfaceKHR, uint32_t*, VkSurfaceFormatKHR*);
    PFN fn = (PFN)next_instance_proc("vkGetPhysicalDeviceSurfaceFormatsKHR");
    if (fn) return fn(pd, surface, pCount, pFormats);
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

static VkResult headless_GetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice pd, VkSurfaceKHR surface, uint32_t* pCount, VkPresentModeKHR* pModes)
{
    TRACE_FN("vkGetPhysicalDeviceSurfacePresentModesKHR");
    if (find_surface(surface)) {
        if (!pModes) { *pCount = 2; return VK_SUCCESS; }
        uint32_t n = *pCount < 2 ? *pCount : 2;
        VkPresentModeKHR modes[] = { VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR };
        memcpy(pModes, modes, n * sizeof(VkPresentModeKHR));
        *pCount = n;
        return n < 2 ? VK_INCOMPLETE : VK_SUCCESS;
    }
    typedef VkResult (*PFN)(VkPhysicalDevice, VkSurfaceKHR, uint32_t*, VkPresentModeKHR*);
    PFN fn = (PFN)next_instance_proc("vkGetPhysicalDeviceSurfacePresentModesKHR");
    if (fn) return fn(pd, surface, pCount, pModes);
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

/* --- VK_KHR_get_surface_capabilities2 --- */

#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR 1000119000
#define VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR 1000119001
#define VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR 1000119002

static VkResult headless_GetPhysicalDeviceSurfaceCapabilities2KHR(
    VkPhysicalDevice pd, const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo,
    VkSurfaceCapabilities2KHR* pSurfaceCapabilities)
{
    TRACE_FN("vkGetPhysicalDeviceSurfaceCapabilities2KHR");
    LOG("vkGetPhysicalDeviceSurfaceCapabilities2KHR: surface=0x%llx pNext=%p\n",
        (unsigned long long)(pSurfaceInfo ? pSurfaceInfo->surface : 0),
        pSurfaceCapabilities ? pSurfaceCapabilities->pNext : NULL);

    /* Log pNext chain for diagnostics — Wine 10's win32u may pass extension structs */
    if (pSurfaceCapabilities && pSurfaceCapabilities->pNext) {
        const struct { int sType; void* pNext; } *chain = pSurfaceCapabilities->pNext;
        int depth = 0;
        while (chain && depth < 8) {
            char pbuf[128];
            snprintf(pbuf, sizeof(pbuf), "SC2KHR_pNext[%d] sType=%d ptr=%p next=%p",
                     depth, chain->sType, (void*)chain, chain->pNext);
            layer_marker(pbuf);
            LOG("  pNext[%d]: sType=%d\n", depth, chain->sType);
            chain = chain->pNext;
            depth++;
        }
    }

    if (pSurfaceInfo) {
        /* Delegate to our existing capabilities handler */
        VkResult r = headless_GetPhysicalDeviceSurfaceCapabilitiesKHR(
            pd, pSurfaceInfo->surface, &pSurfaceCapabilities->surfaceCapabilities);
        if (r == VK_SUCCESS) return VK_SUCCESS;
    }

    /* Fall through to next layer/ICD for non-headless surfaces */
    typedef VkResult (*PFN)(VkPhysicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR*,
                            VkSurfaceCapabilities2KHR*);
    PFN fn = (PFN)next_instance_proc("vkGetPhysicalDeviceSurfaceCapabilities2KHR");
    if (fn) return fn(pd, pSurfaceInfo, pSurfaceCapabilities);
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

static VkResult headless_GetPhysicalDeviceSurfaceFormats2KHR(
    VkPhysicalDevice pd, const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo,
    uint32_t* pSurfaceFormatCount, VkSurfaceFormat2KHR* pSurfaceFormats)
{
    TRACE_FN("vkGetPhysicalDeviceSurfaceFormats2KHR");
    VkSurfaceKHR surface = pSurfaceInfo ? pSurfaceInfo->surface : 0;
    LOG("vkGetPhysicalDeviceSurfaceFormats2KHR: surface=0x%llx count=%p formats=%p\n",
        (unsigned long long)surface, (void*)pSurfaceFormatCount, (void*)pSurfaceFormats);

    if (find_surface(surface)) {
        if (!pSurfaceFormats) {
            *pSurfaceFormatCount = 1;
            return VK_SUCCESS;
        }
        if (*pSurfaceFormatCount >= 1) {
            pSurfaceFormats[0].sType = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR;
            pSurfaceFormats[0].pNext = NULL;
            pSurfaceFormats[0].surfaceFormat.format = VK_FORMAT_B8G8R8A8_UNORM;
            pSurfaceFormats[0].surfaceFormat.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            *pSurfaceFormatCount = 1;
        }
        return VK_SUCCESS;
    }

    typedef VkResult (*PFN)(VkPhysicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR*,
                            uint32_t*, VkSurfaceFormat2KHR*);
    PFN fn = (PFN)next_instance_proc("vkGetPhysicalDeviceSurfaceFormats2KHR");
    if (fn) return fn(pd, pSurfaceInfo, pSurfaceFormatCount, pSurfaceFormats);
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

/* ============================================================================
 * Section 9: Swapchain Functions
 * ============================================================================ */

static void query_mem_props(void) {
    if (g_mem_props_queried || !g_physical_device) return;
    typedef void (*PFN)(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties*);
    PFN fn = (PFN)next_instance_proc("vkGetPhysicalDeviceMemoryProperties");
    if (fn) {
        fn(g_physical_device, &g_mem_props);
        g_mem_props_queried = 1;
        LOG("Memory types: %u\n", g_mem_props.memoryTypeCount);
    }
}

static uint32_t find_host_visible_mem(uint32_t typeBits) {
    query_mem_props();
    uint32_t want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < g_mem_props.memoryTypeCount; i++) {
        if ((typeBits & (1u << i)) && (g_mem_props.memoryTypes[i].propertyFlags & want) == want)
            return i;
    }
    /* Fallback: first compatible type */
    for (uint32_t i = 0; i < 32; i++) {
        if (typeBits & (1u << i)) return i;
    }
    return 0;
}

static uint32_t find_device_local_mem(uint32_t typeBits) {
    query_mem_props();
    uint32_t want = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    for (uint32_t i = 0; i < g_mem_props.memoryTypeCount; i++) {
        if ((typeBits & (1u << i)) && (g_mem_props.memoryTypes[i].propertyFlags & want) == want)
            return i;
    }
    /* Fallback: first compatible type */
    for (uint32_t i = 0; i < 32; i++) {
        if (typeBits & (1u << i)) return i;
    }
    return 0;
}

static VkResult headless_CreateSwapchainKHR(
    VkDevice device,
    const VkSwapchainCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSwapchainKHR* pSwapchain)
{
    TRACE_FN("vkCreateSwapchainKHR");
    /* Only handle our surfaces */
    char scbuf[256];
    snprintf(scbuf, sizeof(scbuf), "SC_ENTER surface=0x%lx dev=%p %ux%u fmt=%d",
             (unsigned long)pCreateInfo->surface, device,
             pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height,
             pCreateInfo->imageFormat);
    layer_marker(scbuf);

    SurfaceEntry* surf = find_surface(pCreateInfo->surface);
    if (!surf) {
        layer_marker("SC_NOT_OUR_SURFACE_forwarding");
        typedef VkResult (*PFN)(VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*);
        PFN fn = (PFN)next_device_proc_for(device, "vkCreateSwapchainKHR");
        if (fn) return fn(device, pCreateInfo, pAllocator, pSwapchain);
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    layer_marker("SC_OUR_SURFACE");
    LOG("CreateSwapchainKHR: %ux%u, %u images, format=%d\n",
        pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height,
        pCreateInfo->minImageCount, pCreateInfo->imageFormat);

    /* Update surface size */
    surf->width = pCreateInfo->imageExtent.width;
    surf->height = pCreateInfo->imageExtent.height;

    SwapchainEntry* sc = calloc(1, sizeof(SwapchainEntry));
    if (!sc) return VK_ERROR_OUT_OF_HOST_MEMORY;

    sc->handle = g_next_sc++;
    sc->surface = pCreateInfo->surface;
    sc->device = device;
    sc->width = pCreateInfo->imageExtent.width;
    sc->height = pCreateInfo->imageExtent.height;
    sc->format = pCreateInfo->imageFormat;
    sc->image_count = pCreateInfo->minImageCount;
    if (sc->image_count > MAX_SC_IMAGES) sc->image_count = MAX_SC_IMAGES;

    /* Get Vulkan functions for image/buffer creation — use THIS device's dispatch */
    typedef VkResult (*PFN_CI)(VkDevice, const VkImageCreateInfo*, const VkAllocationCallbacks*, VkImage*);
    typedef void (*PFN_GMR)(VkDevice, VkImage, VkMemoryRequirements*);
    typedef VkResult (*PFN_AM)(VkDevice, const VkMemoryAllocateInfo*, const VkAllocationCallbacks*, VkDeviceMemory*);
    typedef VkResult (*PFN_BIM)(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize);
    typedef VkResult (*PFN_CB)(VkDevice, const VkBufferCreateInfo*, const VkAllocationCallbacks*, VkBuffer*);
    typedef void (*PFN_GBMR)(VkDevice, VkBuffer, VkMemoryRequirements*);
    typedef VkResult (*PFN_BBM)(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize);

    layer_marker("SC_RESOLVE_FN_START");
    PFN_CI fn_ci = (PFN_CI)next_device_proc_for(device, "vkCreateImage");
    PFN_GMR fn_gmr = (PFN_GMR)next_device_proc_for(device, "vkGetImageMemoryRequirements");
    PFN_AM fn_am = (PFN_AM)next_device_proc_for(device, "vkAllocateMemory");
    PFN_BIM fn_bim = (PFN_BIM)next_device_proc_for(device, "vkBindImageMemory");
    PFN_CB fn_cb = (PFN_CB)next_device_proc_for(device, "vkCreateBuffer");
    PFN_GBMR fn_gbmr = (PFN_GBMR)next_device_proc_for(device, "vkGetBufferMemoryRequirements");
    PFN_BBM fn_bbm = (PFN_BBM)next_device_proc_for(device, "vkBindBufferMemory");

    char dbuf[256];
    snprintf(dbuf, sizeof(dbuf), "SC_FNS ci=%p gmr=%p am=%p bim=%p cb=%p",
             (void*)fn_ci, (void*)fn_gmr, (void*)fn_am, (void*)fn_bim, (void*)fn_cb);
    layer_marker(dbuf);

    if (!fn_ci || !fn_gmr || !fn_am || !fn_bim) {
        LOG("Missing core Vulkan functions! ci=%p gmr=%p am=%p bim=%p dev=%p gdpa=%p\n",
            (void*)fn_ci, (void*)fn_gmr, (void*)fn_am, (void*)fn_bim,
            device, (void*)gdpa_for_device(device));
        layer_marker("SC_MISSING_FNS");
    }

    /* Query memory properties early for diagnostics */
    query_mem_props();
    snprintf(dbuf, sizeof(dbuf), "SC_MEMTYPES=%u phys=%p",
             g_mem_props.memoryTypeCount, g_physical_device);
    layer_marker(dbuf);

    /* Create OPTIMAL images — LINEAR + COLOR_ATTACHMENT causes device loss on Mali */
    for (uint32_t i = 0; i < sc->image_count; i++) {
        if (!fn_ci || !fn_gmr || !fn_am || !fn_bim) {
            break;
        }

        VkImageCreateInfo ici = {0};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = pCreateInfo->imageFormat;
        ici.extent.width = sc->width;
        ici.extent.height = sc->height;
        ici.extent.depth = 1;
        ici.mipLevels = 1;
        ici.arrayLayers = pCreateInfo->imageArrayLayers;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = pCreateInfo->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = 0; /* UNDEFINED */

        snprintf(dbuf, sizeof(dbuf), "SC_IMG%u_CREATE %ux%u fmt=%d usage=0x%x tiling=OPTIMAL",
                 i, sc->width, sc->height, ici.format, ici.usage);
        layer_marker(dbuf);

        VkResult res = fn_ci(device, &ici, NULL, &sc->images[i]);
        snprintf(dbuf, sizeof(dbuf), "SC_IMG%u_RESULT res=%d img=0x%lx",
                 i, res, (unsigned long)sc->images[i]);
        layer_marker(dbuf);
        if (res != VK_SUCCESS) {
            LOG("vkCreateImage[%u] failed: %d\n", i, res);
            continue;
        }

        VkMemoryRequirements memReq = {0};
        fn_gmr(device, sc->images[i], &memReq);
        snprintf(dbuf, sizeof(dbuf), "SC_IMG%u_MEMREQ size=%lu align=%lu bits=0x%x",
                 i, (unsigned long)memReq.size, (unsigned long)memReq.alignment, memReq.memoryTypeBits);
        layer_marker(dbuf);

        VkMemoryAllocateInfo ai = {0};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = memReq.size;
        ai.memoryTypeIndex = find_device_local_mem(memReq.memoryTypeBits);

        snprintf(dbuf, sizeof(dbuf), "SC_IMG%u_ALLOC size=%lu typeIdx=%u (device-local)",
                 i, (unsigned long)ai.allocationSize, ai.memoryTypeIndex);
        layer_marker(dbuf);

        res = fn_am(device, &ai, NULL, &sc->memory[i]);
        snprintf(dbuf, sizeof(dbuf), "SC_IMG%u_ALLOC_RESULT res=%d mem=0x%lx",
                 i, res, (unsigned long)sc->memory[i]);
        layer_marker(dbuf);
        if (res != VK_SUCCESS) {
            LOG("vkAllocateMemory[%u] failed: %d\n", i, res);
            continue;
        }

        res = fn_bim(device, sc->images[i], sc->memory[i], 0);
        snprintf(dbuf, sizeof(dbuf), "SC_IMG%u_BIND_RESULT res=%d", i, res);
        layer_marker(dbuf);
        if (res != VK_SUCCESS) {
            LOG("vkBindImageMemory[%u] failed: %d\n", i, res);
            continue;
        }

        /* OPTIMAL images — tightly packed staging buffer, pitch = width * 4 */
        sc->row_pitch[i] = sc->width * 4;

        LOG("Image[%u]: 0x%lx, mem=0x%lx (OPTIMAL, device-local)\n",
            i, (unsigned long)sc->images[i], (unsigned long)sc->memory[i]);
        snprintf(dbuf, sizeof(dbuf), "SC_IMG%u_DONE tiling=OPTIMAL", i);
        layer_marker(dbuf);
    }

    /* Create staging buffer for OPTIMAL→CPU readback during Present */
    sc->staging_size = (VkDeviceSize)sc->width * sc->height * 4;
    sc->staging_buf = 0;
    sc->staging_mem = 0;
    sc->copy_pool = NULL;
    sc->copy_cmd = NULL;

    if (fn_cb && fn_gbmr && fn_bbm) {
        VkBufferCreateInfo bci = {0};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = sc->staging_size;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkResult bres = fn_cb(device, &bci, NULL, &sc->staging_buf);
        LOG("Staging buffer: size=%lu result=%d buf=0x%lx\n",
            (unsigned long)sc->staging_size, bres, (unsigned long)sc->staging_buf);

        if (bres == VK_SUCCESS && sc->staging_buf) {
            VkMemoryRequirements bmr = {0};
            fn_gbmr(device, sc->staging_buf, &bmr);

            VkMemoryAllocateInfo bai = {0};
            bai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            bai.allocationSize = bmr.size;
            bai.memoryTypeIndex = find_host_visible_mem(bmr.memoryTypeBits);

            bres = fn_am(device, &bai, NULL, &sc->staging_mem);
            LOG("Staging memory: size=%lu typeIdx=%u result=%d\n",
                (unsigned long)bmr.size, bai.memoryTypeIndex, bres);

            if (bres == VK_SUCCESS && sc->staging_mem) {
                fn_bbm(device, sc->staging_buf, sc->staging_mem, 0);
                /* Pre-fill staging buffer with sentinel pattern so we can tell
                 * if CopyImageToBuffer actually executed (zeros = copy ran but
                 * blank; 0xDE = copy never ran; other = real data) */
                {
                    typedef VkResult (*PFN_MM)(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkFlags, void**);
                    typedef void (*PFN_UM)(VkDevice, VkDeviceMemory);
                    PFN_MM fmm = (PFN_MM)next_device_proc_for(device, "vkMapMemory");
                    PFN_UM fum = (PFN_UM)next_device_proc_for(device, "vkUnmapMemory");
                    if (fmm && fum) {
                        void *p = NULL;
                        if (fmm(device, sc->staging_mem, 0, sc->staging_size, 0, &p) == VK_SUCCESS && p) {
                            memset(p, 0xDE, (size_t)sc->staging_size);
                            fum(device, sc->staging_mem);
                            LOG("Staging buffer pre-filled with 0xDE sentinel (%lu bytes)\n",
                                (unsigned long)sc->staging_size);
                        }
                    }
                }
            }
        }

        /* Create command pool + command buffer for copy operations */
        typedef VkResult (*PFN_CCP)(VkDevice, const VkCommandPoolCreateInfo_t*, const VkAllocationCallbacks*, VkCommandPool*);
        typedef VkResult (*PFN_ACB)(VkDevice, const VkCommandBufferAllocateInfo_t*, VkCommandBuffer*);
        PFN_CCP fn_ccp = (PFN_CCP)next_device_proc_for(device, "vkCreateCommandPool");
        PFN_ACB fn_acb = (PFN_ACB)next_device_proc_for(device, "vkAllocateCommandBuffers");

        if (fn_ccp && fn_acb) {
            VkCommandPoolCreateInfo_t cpci = {0};
            cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            cpci.queueFamilyIndex = 0;

            VkResult cpres = fn_ccp(device, &cpci, NULL, &sc->copy_pool);
            LOG("Copy command pool: result=%d pool=%p\n", cpres, sc->copy_pool);

            if (cpres == VK_SUCCESS && sc->copy_pool) {
                VkCommandBufferAllocateInfo_t cbai = {0};
                cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                cbai.commandPool = sc->copy_pool;
                cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                cbai.commandBufferCount = 1;

                fn_acb(device, &cbai, &sc->copy_cmd);
                LOG("Copy command buffer: cmd=%p\n", sc->copy_cmd);
            }
        }
    } else {
        LOG("WARNING: Missing buffer functions, no staging readback available\n");
    }

    /* Get a queue for signaling acquire semaphore/fence in AcquireNextImage.
     * Without this, DXVK's vkQueueSubmit waits forever on the unsignaled semaphore. */
    sc->signal_queue = NULL;
    {
        typedef void (*PFN_GDQ)(VkDevice, uint32_t, uint32_t, VkQueue*);
        PFN_GDQ fn_gdq = (PFN_GDQ)next_device_proc_for(device, "vkGetDeviceQueue");
        if (fn_gdq) {
            fn_gdq(device, 0, 0, &sc->signal_queue);
            LOG("Got signal_queue=%p for acquire sync\n", sc->signal_queue);
        }
    }

    pthread_mutex_lock(&g_mutex);
    sc->next = g_swapchains;
    g_swapchains = sc;
    pthread_mutex_unlock(&g_mutex);

    *pSwapchain = sc->handle;

    /* Health check: verify the device is not lost after all image/buffer creation */
    {
        typedef VkResult (*PFN_DWI)(VkDevice);
        PFN_DWI fn_dwi = (PFN_DWI)next_device_proc_for(device, "vkDeviceWaitIdle");
        if (fn_dwi) {
            VkResult wires = fn_dwi(device);
            LOG("Post-swapchain DeviceWaitIdle: %d\n", wires);
            if (wires != VK_SUCCESS) {
                LOG("WARNING: Device may be LOST after swapchain creation! result=%d\n", wires);
            }
        }
    }

    snprintf(scbuf, sizeof(scbuf), "SC_OK handle=0x%lx images=%u staging=%s",
             (unsigned long)sc->handle, sc->image_count,
             sc->staging_buf ? "YES" : "NO");
    layer_marker(scbuf);
    LOG("Created swapchain 0x%lx with %u OPTIMAL images, staging=%s\n",
        (unsigned long)sc->handle, sc->image_count,
        sc->staging_buf ? "YES" : "NO");
    return VK_SUCCESS;
}

static void headless_DestroySwapchainKHR(
    VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator)
{
    TRACE_FN("vkDestroySwapchainKHR");
    if (!is_our_swapchain(swapchain)) {
        typedef void (*PFN)(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*);
        PFN fn = (PFN)next_device_proc_for(device, "vkDestroySwapchainKHR");
        if (fn) fn(device, swapchain, pAllocator);
        return;
    }

    /* Remove from list */
    pthread_mutex_lock(&g_mutex);
    SwapchainEntry* to_free = NULL;
    SwapchainEntry** pp = &g_swapchains;
    while (*pp) {
        if ((*pp)->handle == swapchain) {
            to_free = *pp;
            *pp = to_free->next;
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_mutex);

    if (!to_free) return;

    VkDevice dev = device ? device : to_free->device;
    typedef VkResult (*PFN_WI)(VkDevice);
    typedef void (*PFN_DI)(VkDevice, VkImage, const VkAllocationCallbacks*);
    typedef void (*PFN_FM)(VkDevice, VkDeviceMemory, const VkAllocationCallbacks*);

    PFN_WI fn_wait = (PFN_WI)next_device_proc_for(dev, "vkDeviceWaitIdle");
    PFN_DI fn_di = (PFN_DI)next_device_proc_for(dev, "vkDestroyImage");
    PFN_FM fn_fm = (PFN_FM)next_device_proc_for(dev, "vkFreeMemory");

    if (fn_wait) fn_wait(dev);

    /* Destroy staging resources */
    if (to_free->copy_pool) {
        typedef void (*PFN_DCP)(VkDevice, VkCommandPool, const VkAllocationCallbacks*);
        PFN_DCP fn_dcp = (PFN_DCP)next_device_proc_for(dev, "vkDestroyCommandPool");
        if (fn_dcp) fn_dcp(dev, to_free->copy_pool, NULL);
    }
    if (to_free->staging_buf) {
        typedef void (*PFN_DB)(VkDevice, VkBuffer, const VkAllocationCallbacks*);
        PFN_DB fn_db = (PFN_DB)next_device_proc_for(dev, "vkDestroyBuffer");
        if (fn_db) fn_db(dev, to_free->staging_buf, NULL);
    }
    if (to_free->staging_mem && fn_fm) fn_fm(dev, to_free->staging_mem, NULL);

    for (uint32_t i = 0; i < to_free->image_count; i++) {
        if (to_free->images[i] && fn_di) fn_di(dev, to_free->images[i], NULL);
        if (to_free->memory[i] && fn_fm) fn_fm(dev, to_free->memory[i], NULL);
    }
    free(to_free);
    LOG("Destroyed swapchain 0x%lx\n", (unsigned long)swapchain);
}

static VkResult headless_GetSwapchainImagesKHR(
    VkDevice device, VkSwapchainKHR swapchain, uint32_t* pCount, VkImage* pImages)
{
    TRACE_FN("vkGetSwapchainImagesKHR");
    SwapchainEntry* sc = find_swapchain(swapchain);
    if (!sc) {
        typedef VkResult (*PFN)(VkDevice, VkSwapchainKHR, uint32_t*, VkImage*);
        PFN fn = (PFN)next_device_proc_for(device, "vkGetSwapchainImagesKHR");
        if (fn) return fn(device, swapchain, pCount, pImages);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (!pImages) { *pCount = sc->image_count; return VK_SUCCESS; }
    uint32_t n = *pCount < sc->image_count ? *pCount : sc->image_count;
    for (uint32_t i = 0; i < n; i++) pImages[i] = sc->images[i];
    *pCount = n;
    return n < sc->image_count ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult headless_AcquireNextImageKHR(
    VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
    VkSemaphore sem, VkFence fence, uint32_t* pImageIndex)
{
    TRACE_FN("vkAcquireNextImageKHR");
    SwapchainEntry* sc = find_swapchain(swapchain);
    if (!sc) {
        typedef VkResult (*PFN)(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t*);
        PFN fn = (PFN)next_device_proc_for(device, "vkAcquireNextImageKHR");
        if (fn) return fn(device, swapchain, timeout, sem, fence, pImageIndex);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *pImageIndex = sc->current_image;
    sc->current_image = (sc->current_image + 1) % sc->image_count;

    /* Signal the acquire semaphore and/or fence via a no-op queue submit.
     * Without this, DXVK's vkQueueSubmit waits forever on the unsignaled
     * semaphore — the headless "presentation engine" is always ready. */
    {
        char abuf[256];
        snprintf(abuf, sizeof(abuf), "ANI img=%u sem=0x%llx fence=0x%llx queue=%p dev=%p",
                 pImageIndex ? *pImageIndex : 99,
                 (unsigned long long)sem, (unsigned long long)fence,
                 sc->signal_queue, device);
        layer_marker(abuf);
    }

    if ((sem || fence) && sc->signal_queue) {
        VkSubmitInfo si;
        memset(&si, 0, sizeof(si));
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        if (sem) {
            si.signalSemaphoreCount = 1;
            si.pSignalSemaphores = &sem;
        }
        typedef VkResult (*PFN_QS)(VkQueue, uint32_t, const VkSubmitInfo*, VkFence);
        PFN_QS fn_qs = (PFN_QS)next_device_proc_for(device, "vkQueueSubmit");
        {
            char abuf[128];
            snprintf(abuf, sizeof(abuf), "ANI_SIGNAL fn_qs=%p", (void*)fn_qs);
            layer_marker(abuf);
        }
        if (fn_qs) {
            VkResult r = fn_qs(sc->signal_queue, 1, &si, fence);
            {
                char abuf[128];
                snprintf(abuf, sizeof(abuf), "ANI_SIGNAL_RESULT=%d", r);
                layer_marker(abuf);
            }
        }
    } else {
        layer_marker("ANI_NO_SIGNAL");
    }

    return VK_SUCCESS;
}

static void dump_frame_ppm(int frame_num, uint32_t width, uint32_t height, const void* mapped) {
    const uint8_t *px = (const uint8_t *)mapped;
    uint32_t total_pixels = width * height;
    uint32_t nonzero = 0;

    /* Count non-zero pixels for diagnostics */
    for (uint32_t i = 0; i < total_pixels; i++) {
        uint32_t off = i * 4;
        if (px[off] || px[off+1] || px[off+2])
            nonzero++;
    }

    /* Sample first + center pixel */
    uint32_t center_off = (height/2 * width + width/2) * 4;
    LOG("[DUMP] Frame %04d: %ux%u, nonzero=%u/%u (%.1f%%)\n",
        frame_num, width, height, nonzero, total_pixels,
        total_pixels ? (100.0f * nonzero / total_pixels) : 0.0f);
    LOG("[DUMP]   pixel[0,0] BGRA=%02x,%02x,%02x,%02x  center BGRA=%02x,%02x,%02x,%02x\n",
        px[0], px[1], px[2], px[3],
        px[center_off], px[center_off+1], px[center_off+2], px[center_off+3]);

    /* Write PPM file */
    char path[64];
    snprintf(path, sizeof(path), "/tmp/frame_%04d.ppm", frame_num);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%u %u\n255\n", width, height);
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x++) {
                uint32_t off = (y * width + x) * 4;
                /* B8G8R8A8 → RGB */
                uint8_t rgb[3] = { px[off+2], px[off+1], px[off+0] };
                fwrite(rgb, 1, 3, f);
            }
        }
        fclose(f);
        LOG("[DUMP] Wrote %s\n", path);
    } else {
        LOG("[DUMP] ERROR: fopen(%s) failed: %s\n", path, strerror(errno));
    }

    /* Append to summary */
    if (g_dump_summary) {
        fprintf(g_dump_summary, "frame=%04d size=%ux%u nonzero=%u/%u (%.1f%%) "
                "px0=(%02x,%02x,%02x,%02x) center=(%02x,%02x,%02x,%02x) file=%s\n",
                frame_num, width, height, nonzero, total_pixels,
                total_pixels ? (100.0f * nonzero / total_pixels) : 0.0f,
                px[0], px[1], px[2], px[3],
                px[center_off], px[center_off+1], px[center_off+2], px[center_off+3],
                path);
        fflush(g_dump_summary);
    }

    g_dump_frame_count++;
    if (g_dump_frame_count >= g_dump_max_frames) {
        LOG("[DUMP] All %d frames captured! Done.\n", g_dump_max_frames);
        if (g_dump_summary) {
            fprintf(g_dump_summary, "=== DUMP COMPLETE: %d frames ===\n", g_dump_max_frames);
            fclose(g_dump_summary);
            g_dump_summary = NULL;
        }
    }
}

static int g_present_count = 0;

static VkResult headless_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
{
    TRACE_FN("vkQueuePresentKHR");

    /* Lazy init: check env var here in case constructor missed it
     * (FEX child process re-exec may not run constructors with full env) */
    if (!g_dump_mode && !g_dump_max_frames) {
        const char *dump_env = getenv("HEADLESS_DUMP_FRAMES");
        if (dump_env) {
            g_dump_max_frames = atoi(dump_env);
            if (g_dump_max_frames > 0) {
                g_dump_mode = 1;
                g_dump_frame_count = 0;
                if (!g_dump_summary) {
                    g_dump_summary = fopen("/tmp/frame_summary.txt", "w");
                    if (g_dump_summary) {
                        fprintf(g_dump_summary, "=== DUMP MODE (lazy init): capturing %d frames ===\n", g_dump_max_frames);
                        fflush(g_dump_summary);
                    }
                }
                LOG("DUMP MODE enabled (lazy init in QueuePresent): %d frames\n", g_dump_max_frames);
            }
        }
    }

    if (g_present_count < 3) {
        char pbuf[128];
        snprintf(pbuf, sizeof(pbuf), "QueuePresent #%d swapchains=%u", g_present_count, pPresentInfo->swapchainCount);
        layer_marker(pbuf);
    }
    g_present_count++;

    for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
        SwapchainEntry* sc = find_swapchain(pPresentInfo->pSwapchains[i]);
        if (!sc) {
            /* Forward to ICD */
            typedef VkResult (*PFN)(VkQueue, const VkPresentInfoKHR*);
            PFN fn = (PFN)next_device_proc("vkQueuePresentKHR");
            if (fn) return fn(queue, pPresentInfo);
            continue;
        }

        uint32_t idx = pPresentInfo->pImageIndices[i];
        if (idx < sc->image_count && sc->images[idx] &&
            sc->staging_buf && sc->copy_cmd && queue) {

            /* Resolve command recording functions */
            typedef VkResult (*PFN_BCB)(VkCommandBuffer, const VkCommandBufferBeginInfo_t*);
            typedef VkResult (*PFN_ECB)(VkCommandBuffer);
            typedef VkResult (*PFN_RCB)(VkCommandBuffer, VkFlags);
            typedef void (*PFN_CPB)(VkCommandBuffer, VkFlags, VkFlags, VkFlags,
                                    uint32_t, const void*, uint32_t, const void*,
                                    uint32_t, const VkImageMemoryBarrier*);
            typedef void (*PFN_CITB)(VkCommandBuffer, VkImage, int, VkBuffer,
                                     uint32_t, const VkBufferImageCopy*);
            typedef VkResult (*PFN_QS)(VkQueue, uint32_t, const VkSubmitInfo*, uint64_t);
            typedef VkResult (*PFN_QWI)(VkQueue);
            typedef VkResult (*PFN_MM)(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkFlags, void**);
            typedef void (*PFN_UM)(VkDevice, VkDeviceMemory);

            PFN_RCB fn_rcb = (PFN_RCB)next_device_proc_for(sc->device, "vkResetCommandBuffer");
            PFN_BCB fn_bcb = (PFN_BCB)next_device_proc_for(sc->device, "vkBeginCommandBuffer");
            PFN_ECB fn_ecb = (PFN_ECB)next_device_proc_for(sc->device, "vkEndCommandBuffer");
            PFN_CPB fn_cpb = (PFN_CPB)next_device_proc_for(sc->device, "vkCmdPipelineBarrier");
            PFN_CITB fn_citb = (PFN_CITB)next_device_proc_for(sc->device, "vkCmdCopyImageToBuffer");
            PFN_QS fn_qs = (PFN_QS)next_device_proc_for(sc->device, "vkQueueSubmit");
            PFN_QWI fn_qwi = (PFN_QWI)next_device_proc_for(sc->device, "vkQueueWaitIdle");
            PFN_MM fn_map = (PFN_MM)next_device_proc_for(sc->device, "vkMapMemory");
            PFN_UM fn_unmap = (PFN_UM)next_device_proc_for(sc->device, "vkUnmapMemory");

            if (fn_rcb && fn_bcb && fn_ecb && fn_citb && fn_cpb && fn_qs && fn_qwi) {
                /* Record: barrier(PRESENT_SRC→TRANSFER_SRC) + CopyImageToBuffer
                 * Barriers work on ARM64 host side (no handle wrapping issues) */
                VkResult rcb_res = fn_rcb(sc->copy_cmd, 0);
                LOG("[COPY] ResetCB=%d cmd=%p\n", rcb_res, sc->copy_cmd);

                VkCommandBufferBeginInfo_t bi = {0};
                bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                VkResult bcb_res = fn_bcb(sc->copy_cmd, &bi);
                LOG("[COPY] BeginCB=%d\n", bcb_res);

                /* Barrier: PRESENT_SRC → TRANSFER_SRC */
                {
                    VkImageMemoryBarrier imb = {0};
                    imb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    imb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                    imb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                    imb.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                    imb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                    imb.srcQueueFamilyIndex = 0xFFFFFFFF;
                    imb.dstQueueFamilyIndex = 0xFFFFFFFF;
                    imb.image = sc->images[idx];
                    imb.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    imb.subresourceRange.levelCount = 1;
                    imb.subresourceRange.layerCount = 1;
                    fn_cpb(sc->copy_cmd,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, 0, NULL, 0, NULL, 1, &imb);
                }

                /* Copy image to staging buffer */
                VkBufferImageCopy region = {0};
                region.bufferRowLength = 0;      /* tightly packed */
                region.bufferImageHeight = 0;
                region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                region.imageSubresource.layerCount = 1;
                region.imageExtent.width = sc->width;
                region.imageExtent.height = sc->height;
                region.imageExtent.depth = 1;

                LOG("[COPY] CopyImageToBuffer: img=0x%lx buf=0x%lx %ux%u\n",
                    (unsigned long)sc->images[idx], (unsigned long)sc->staging_buf,
                    sc->width, sc->height);
                fn_citb(sc->copy_cmd, sc->images[idx],
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        sc->staging_buf, 1, &region);
                LOG("[COPY] CopyImageToBuffer recorded\n");

                /* Barrier: TRANSFER_SRC → PRESENT_SRC (restore for next frame) */
                {
                    VkImageMemoryBarrier rb = {0};
                    rb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    rb.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                    rb.dstAccessMask = 0;
                    rb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                    rb.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                    rb.srcQueueFamilyIndex = 0xFFFFFFFF;
                    rb.dstQueueFamilyIndex = 0xFFFFFFFF;
                    rb.image = sc->images[idx];
                    rb.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    rb.subresourceRange.levelCount = 1;
                    rb.subresourceRange.layerCount = 1;
                    fn_cpb(sc->copy_cmd,
                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                           0, 0, NULL, 0, NULL, 1, &rb);
                }
                LOG("[COPY] Barrier TRANSFER_SRC→PRESENT_SRC recorded\n");

                VkResult ecb_res = fn_ecb(sc->copy_cmd);
                LOG("[COPY] EndCB=%d\n", ecb_res);

                /* Submit copy and wait.
                 * CRITICAL: consume the present's wait semaphores here so
                 * binary semaphores transition to unsignaled.  Otherwise the
                 * next QueueSubmit that signals them hits a spec violation
                 * (signaling an already-signaled binary sem) → DEVICE_LOST. */
                VkSubmitInfo si;
                memset(&si, 0, sizeof(si));
                si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                si.commandBufferCount = 1;
                si.pCommandBuffers = &sc->copy_cmd;
                VkFlags wait_stages[8];
                if (i == 0 && pPresentInfo->waitSemaphoreCount > 0) {
                    uint32_t wc = pPresentInfo->waitSemaphoreCount;
                    if (wc > 8) wc = 8;
                    si.waitSemaphoreCount = wc;
                    si.pWaitSemaphores = pPresentInfo->pWaitSemaphores;
                    for (uint32_t w = 0; w < wc; w++)
                        wait_stages[w] = VK_PIPELINE_STAGE_TRANSFER_BIT;
                    si.pWaitDstStageMask = wait_stages;
                }
                VkResult qs_res = fn_qs(queue, 1, &si, 0);
                LOG("[COPY] QueueSubmit=%d (waitSems=%u)\n", qs_res, si.waitSemaphoreCount);
                VkResult qwi_res = fn_qwi(queue);
                LOG("[COPY] QueueWaitIdle=%d\n", qwi_res);

                /* Map staging buffer and send frame */
                if (fn_map && fn_unmap) {
                    void* mapped = NULL;
                    VkResult mres = fn_map(sc->device, sc->staging_mem, 0,
                                           sc->staging_size, 0, &mapped);
                    LOG("[COPY] MapMemory=%d ptr=%p\n", mres, mapped);
                    if (mres == VK_SUCCESS && mapped) {
                        /* Check first 16 bytes for sentinel vs real data */
                        const uint8_t *px = (const uint8_t *)mapped;
                        LOG("[COPY] First 16 bytes: %02x %02x %02x %02x %02x %02x %02x %02x "
                            "%02x %02x %02x %02x %02x %02x %02x %02x\n",
                            px[0], px[1], px[2], px[3], px[4], px[5], px[6], px[7],
                            px[8], px[9], px[10], px[11], px[12], px[13], px[14], px[15]);
                        /* Check center pixel too */
                        uint32_t center_off = (sc->height/2 * sc->width + sc->width/2) * 4;
                        LOG("[COPY] Center pixel @%u: %02x %02x %02x %02x\n",
                            center_off, px[center_off], px[center_off+1],
                            px[center_off+2], px[center_off+3]);

                        if (g_dump_mode) {
                            /* Dump mode: write PPM files, skip TCP */
                            if (g_dump_frame_count < g_dump_max_frames) {
                                dump_frame_ppm(g_dump_frame_count, sc->width, sc->height, mapped);
                            }
                        } else {
                            /* Normal mode: send via TCP */
                            send_frame(sc->width, sc->height, mapped, sc->width * 4);

                            /* Legacy single-frame dump (backward compat) */
                            {
                                static int dumped = 0;
                                if (!dumped && getenv("HEADLESS_DUMP_PPM")) {
                                    dumped = 1;
                                    FILE *f = fopen("/tmp/frame_dump.ppm", "wb");
                                    if (f) {
                                        fprintf(f, "P6\n%u %u\n255\n", sc->width, sc->height);
                                        for (uint32_t y = 0; y < sc->height; y++) {
                                            for (uint32_t x = 0; x < sc->width; x++) {
                                                uint32_t off = (y * sc->width + x) * 4;
                                                uint8_t rgb[3] = { px[off+2], px[off+1], px[off+0] };
                                                fwrite(rgb, 1, 3, f);
                                            }
                                        }
                                        fclose(f);
                                        LOG("PPM frame dumped: /tmp/frame_dump.ppm (%ux%u)\n",
                                            sc->width, sc->height);
                                    }
                                }
                            }
                        }

                        fn_unmap(sc->device, sc->staging_mem);
                    }
                }
            } else {
                /* Fallback: just wait idle (no readback) */
                typedef VkResult (*PFN_QWI2)(VkQueue);
                PFN_QWI2 fn_qwi2 = (PFN_QWI2)next_device_proc_for(sc->device, "vkQueueWaitIdle");
                if (fn_qwi2 && queue) fn_qwi2(queue);
            }
        }

        if (pPresentInfo->pResults)
            pPresentInfo->pResults[i] = VK_SUCCESS;
    }

    /* Vsync emulation */
    uint64_t now = get_time_ns();
    if (g_last_present_ns > 0) {
        uint64_t elapsed = now - g_last_present_ns;
        if (elapsed < TARGET_FRAME_NS) {
            struct timespec ts = {0, (long)(TARGET_FRAME_NS - elapsed)};
            nanosleep(&ts, NULL);
        }
    }
    g_last_present_ns = get_time_ns();

    return VK_SUCCESS;
}

/* ============================================================================
 * Section 10: Extension Enumeration
 * ============================================================================ */

static VkResult headless_EnumerateInstanceExtensionProperties(
    const char* pLayerName, uint32_t* pCount, VkExtensionProperties* pProps)
{
    /* If querying our layer specifically, return our extensions */
    if (pLayerName && strcmp(pLayerName, "VK_LAYER_HEADLESS_surface") == 0) {
        static const VkExtensionProperties exts[] = {
            { "VK_KHR_surface", 25 },
            { "VK_KHR_xcb_surface", 6 },
            { "VK_KHR_xlib_surface", 6 },
            { "VK_EXT_headless_surface", 1 }
        };
        if (!pProps) { *pCount = 4; return VK_SUCCESS; }
        uint32_t n = *pCount < 4 ? *pCount : 4;
        memcpy(pProps, exts, n * sizeof(VkExtensionProperties));
        *pCount = n;
        return n < 4 ? VK_INCOMPLETE : VK_SUCCESS;
    }

    /* Forward to next layer/ICD */
    typedef VkResult (*PFN)(const char*, uint32_t*, VkExtensionProperties*);
    PFN fn = NULL;
    if (g_next_gipa) fn = (PFN)g_next_gipa(NULL, "vkEnumerateInstanceExtensionProperties");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;

    /* If querying a specific other layer, just forward */
    if (pLayerName) return fn(pLayerName, pCount, pProps);

    /* Global query (pLayerName == NULL): merge our extensions into the ICD list.
     * Loader 1.3.283 doesn't merge implicit layer extensions into the global
     * list, so we must do it ourselves for VK_KHR_xlib_surface etc. to be
     * visible during vkCreateInstance extension validation. */
    static const VkExtensionProperties layer_exts[] = {
        { "VK_KHR_surface", 25 },
        { "VK_KHR_xcb_surface", 6 },
        { "VK_KHR_xlib_surface", 6 },
        { "VK_EXT_headless_surface", 1 }
    };
    static const uint32_t layer_ext_count = sizeof(layer_exts) / sizeof(layer_exts[0]);

    /* Get ICD/next-layer extension count */
    uint32_t icd_count = 0;
    VkResult res = fn(NULL, &icd_count, NULL);
    if (res != VK_SUCCESS) return res;

    /* Count how many of our extensions are NOT already in the ICD list */
    VkExtensionProperties icd_exts[64];
    uint32_t tmp_count = icd_count < 64 ? icd_count : 64;
    fn(NULL, &tmp_count, icd_exts);

    uint32_t new_count = 0;
    for (uint32_t i = 0; i < layer_ext_count; i++) {
        int found = 0;
        for (uint32_t j = 0; j < tmp_count; j++) {
            if (strcmp(layer_exts[i].extensionName, icd_exts[j].extensionName) == 0) {
                found = 1; break;
            }
        }
        if (!found) new_count++;
    }

    uint32_t total = icd_count + new_count;
    if (!pProps) { *pCount = total; return VK_SUCCESS; }

    /* Fill: ICD extensions first, then our unique ones */
    uint32_t avail = *pCount;
    uint32_t filled = avail < icd_count ? avail : icd_count;
    res = fn(NULL, &filled, pProps);

    uint32_t pos = filled;
    for (uint32_t i = 0; i < layer_ext_count && pos < avail; i++) {
        int found = 0;
        for (uint32_t j = 0; j < filled; j++) {
            if (strcmp(layer_exts[i].extensionName, pProps[j].extensionName) == 0) {
                found = 1; break;
            }
        }
        if (!found) pProps[pos++] = layer_exts[i];
    }

    *pCount = pos;
    return pos < total ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult headless_EnumerateDeviceExtensionProperties(
    VkPhysicalDevice pd, const char* pLayerName, uint32_t* pCount, VkExtensionProperties* pProps)
{
    char mbuf[256];
    snprintf(mbuf, sizeof(mbuf), "EDEP_ENTER pd=%p layer=%s pProps=%p g_inst=%p",
             pd, pLayerName ? pLayerName : "(null)", (void*)pProps, g_instance);
    layer_marker(mbuf);
    LOG("EnumDevExtProps: pd=%p layer=%s pProps=%p\n", pd, pLayerName ? pLayerName : "(null)", (void*)pProps);

    typedef VkResult (*PFN)(VkPhysicalDevice, const char*, uint32_t*, VkExtensionProperties*);
    PFN fn = (PFN)next_instance_proc("vkEnumerateDeviceExtensionProperties");
    if (!fn) {
        layer_marker("EDEP_NO_FN");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    /* Get real count */
    uint32_t real_count = 0;
    VkResult res = fn(pd, pLayerName, &real_count, NULL);
    if (res != VK_SUCCESS) return res;

    /* Extensions to filter OUT — these cause crashes through FEX thunks.
     * VK_KHR_map_memory2 + VK_EXT_map_memory_placed: Wine uses placed memory
     * mapping (vkMapMemory2KHR with VK_MEMORY_MAP_PLACED_BIT_EXT) when it sees
     * these, but the placed path crashes through FEX thunks/Vortek. */
    static const char* filter_exts[] = {
        "VK_KHR_map_memory2",
        "VK_EXT_map_memory_placed",
    };
    static const int num_filter = sizeof(filter_exts) / sizeof(filter_exts[0]);

    /* Extensions to inject if missing */
    static const struct { const char* name; uint32_t specVersion; } inject_exts[] = {
        { "VK_KHR_swapchain", 70 },
        { "VK_EXT_depth_clip_enable", 1 },
        { "VK_EXT_custom_border_color", 12 },
        { "VK_EXT_transform_feedback", 1 },
        { "VK_EXT_robustness2", 1 },
        { "VK_KHR_maintenance5", 1 },
        { "VK_KHR_maintenance6", 1 },
        { "VK_KHR_pipeline_library", 1 },
        { "VK_EXT_non_seamless_cube_map", 1 },
        { "VK_EXT_graphics_pipeline_library", 1 },
    };
    static const int num_inject = sizeof(inject_exts) / sizeof(inject_exts[0]);

    /* Fetch all real extensions into temp buffer for filtering */
    int has_ext[sizeof(inject_exts) / sizeof(inject_exts[0])];
    memset(has_ext, 0, sizeof(has_ext));
    int need_inject = 0;
    int num_filtered = 0;

    VkExtensionProperties* tmp = NULL;
    uint32_t tc = 0;
    if (real_count > 0) {
        tmp = malloc(real_count * sizeof(VkExtensionProperties));
        if (tmp) {
            tc = real_count;
            fn(pd, pLayerName, &tc, tmp);
            for (uint32_t i = 0; i < tc; i++) {
                /* Check if this extension should be filtered out */
                int filtered = 0;
                for (int f = 0; f < num_filter; f++) {
                    if (strcmp(tmp[i].extensionName, filter_exts[f]) == 0) {
                        filtered = 1;
                        num_filtered++;
                        LOG("Filtering out device extension: %s\n", tmp[i].extensionName);
                        break;
                    }
                }
                if (!filtered) {
                    for (int j = 0; j < num_inject; j++) {
                        if (strcmp(tmp[i].extensionName, inject_exts[j].name) == 0)
                            has_ext[j] = 1;
                    }
                }
            }
        }
    }

    for (int j = 0; j < num_inject; j++)
        if (!has_ext[j]) need_inject++;

    uint32_t total = (tc - num_filtered) + need_inject;
    if (!pProps) { *pCount = total; free(tmp); return VK_SUCCESS; }

    /* Copy non-filtered extensions */
    uint32_t idx = 0;
    for (uint32_t i = 0; i < tc && idx < *pCount; i++) {
        int filtered = 0;
        for (int f = 0; f < num_filter; f++) {
            if (strcmp(tmp[i].extensionName, filter_exts[f]) == 0) {
                filtered = 1;
                break;
            }
        }
        if (!filtered)
            pProps[idx++] = tmp[i];
    }
    free(tmp);

    /* Append missing injected extensions */
    for (int j = 0; j < num_inject && idx < *pCount; j++) {
        if (!has_ext[j]) {
            strncpy(pProps[idx].extensionName, inject_exts[j].name, VK_MAX_EXTENSION_NAME_SIZE);
            pProps[idx].specVersion = inject_exts[j].specVersion;
            LOG("Injected device extension: %s\n", inject_exts[j].name);
            idx++;
        }
    }
    *pCount = idx;

    snprintf(mbuf, sizeof(mbuf), "EDEP_DONE total=%u injected=%d", *pCount, need_inject);
    layer_marker(mbuf);
    return VK_SUCCESS;
}

/* ============================================================================
 * Section 11: vkCreateInstance — Layer Dispatch Chain
 * ============================================================================ */

/* Generic Vulkan base struct for pNext chain traversal.
 * All Vulkan structs have sType (int32) + pNext (void*) at the start.
 * On x86-64, pNext is at offset 8 due to pointer alignment, NOT offset 4! */
typedef struct VkBaseOutStructure_ {
    int sType;
    const struct VkBaseOutStructure_* pNext;
} VkBaseOutStructure;

/* Helper: find VkLayerInstanceCreateInfo in pNext chain */
static VkLayerInstanceCreateInfo* find_instance_layer_info(const VkInstanceCreateInfo* pCreateInfo) {
    const VkBaseOutStructure* pNext = (const VkBaseOutStructure*)pCreateInfo->pNext;
    while (pNext) {
        const VkLayerInstanceCreateInfo* info = (const VkLayerInstanceCreateInfo*)pNext;
        if (info->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
            info->function == VK_LAYER_LINK_INFO) {
            return (VkLayerInstanceCreateInfo*)info;
        }
        pNext = pNext->pNext;
    }
    return NULL;
}

static VkResult headless_CreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance)
{
    TRACE_FN("vkCreateInstance");
    LOG("vkCreateInstance intercepted (%u extensions requested)\n",
        pCreateInfo->enabledExtensionCount);

    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++)
        LOG("  requested ext[%u]: %s\n", i, pCreateInfo->ppEnabledExtensionNames[i]);

    /* Find layer chain info */
    VkLayerInstanceCreateInfo* chain = find_instance_layer_info(pCreateInfo);
    if (!chain || !chain->u.pLayerInfo) {
        LOG("ERROR: No layer chain info found!\n");
        layer_marker("CI_NO_CHAIN");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    layer_marker("CI_CHAIN_FOUND");

    /* Save next layer's GetInstanceProcAddr */
    PFN_vkGetInstanceProcAddr next_gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    LOG("next_gipa = %p\n", (void*)next_gipa);

    /* Advance chain for next layer */
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

    /* Get next layer's vkCreateInstance */
    typedef VkResult (*PFN_CI)(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
    layer_marker("CI_GET_NEXT");
    PFN_CI next_create = (PFN_CI)next_gipa(NULL, "vkCreateInstance");
    if (!next_create) {
        LOG("ERROR: Could not get next vkCreateInstance!\n");
        layer_marker("CI_NEXT_NULL");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    char buf2[128];
    snprintf(buf2, sizeof(buf2), "CI_NEXT_CREATE=%p", (void*)next_create);
    layer_marker(buf2);

    /* Filter out extensions we provide (ICD doesn't support them) */
    const char** filtered = malloc(pCreateInfo->enabledExtensionCount * sizeof(char*));
    uint32_t fc = 0;

    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
        const char* ext = pCreateInfo->ppEnabledExtensionNames[i];
        if (strcmp(ext, "VK_KHR_surface") == 0 ||
            strcmp(ext, "VK_KHR_xcb_surface") == 0 ||
            strcmp(ext, "VK_KHR_xlib_surface") == 0 ||
            strcmp(ext, "VK_EXT_headless_surface") == 0) {
            LOG("Filtering extension: %s (we provide it)\n", ext);
        } else {
            filtered[fc++] = ext;
        }
    }

    VkInstanceCreateInfo modified = *pCreateInfo;
    modified.enabledExtensionCount = fc;
    modified.ppEnabledExtensionNames = filtered;

    snprintf(buf2, sizeof(buf2), "CI_CALLING_NEXT ext=%u", fc);
    layer_marker(buf2);
    LOG("Creating instance with %u extensions (filtered %u)\n",
        fc, pCreateInfo->enabledExtensionCount - fc);

    VkResult result = next_create(&modified, pAllocator, pInstance);
    free(filtered);

    snprintf(buf2, sizeof(buf2), "CI_RETURNED result=%d", result);
    layer_marker(buf2);

    if (result == VK_SUCCESS) {
        g_instance_count++;
        g_next_gipa = next_gipa;
        g_instance = *pInstance;

        /* Resolve real function pointers for feature/format spoofing.
         * We use next_gipa (the next layer's GIPA) so we get the ICD's
         * actual implementations, NOT our own interceptors. */
        g_real_get_features = (PFN_GetFeatures)next_gipa(*pInstance, "vkGetPhysicalDeviceFeatures");
        g_real_get_features2 = (PFN_GetFeatures2)next_gipa(*pInstance, "vkGetPhysicalDeviceFeatures2");
        if (!g_real_get_features2)
            g_real_get_features2 = (PFN_GetFeatures2)next_gipa(*pInstance, "vkGetPhysicalDeviceFeatures2KHR");
        g_real_get_format_props = (PFN_GetFormatProps)next_gipa(*pInstance, "vkGetPhysicalDeviceFormatProperties");
        g_real_get_format_props2 = (PFN_GetFormatProps2)next_gipa(*pInstance, "vkGetPhysicalDeviceFormatProperties2");
        if (!g_real_get_format_props2)
            g_real_get_format_props2 = (PFN_GetFormatProps2)next_gipa(*pInstance, "vkGetPhysicalDeviceFormatProperties2KHR");
        LOG("BC spoof: features=%p features2=%p fmtprops=%p fmtprops2=%p\n",
            (void*)g_real_get_features, (void*)g_real_get_features2,
            (void*)g_real_get_format_props, (void*)g_real_get_format_props2);

        LOG("Instance created: %p (instance #%d)\n", *pInstance, g_instance_count);
        char buf[256];
        snprintf(buf, sizeof(buf), "CreateInstance_OK #%d g_instance=%p next_gipa=%p",
                 g_instance_count, *pInstance, (void*)next_gipa);
        layer_marker(buf);
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "CreateInstance_FAIL result=%d", result);
        layer_marker(buf);
    }

    return result;
}

static void headless_DestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator) {
    char buf[256];
    snprintf(buf, sizeof(buf), "DestroyInstance_ENTER caller=%p g_instance=%p", instance, g_instance);
    layer_marker(buf);
    LOG("vkDestroyInstance: caller=%p, g_instance=%p\n", instance, g_instance);

    typedef void (*PFN)(VkInstance, const VkAllocationCallbacks*);
    PFN fn = (PFN)next_instance_proc("vkDestroyInstance");
    /* Use g_instance (ICD's handle) for the actual destroy call */
    if (fn) {
        fn(g_instance, pAllocator);
        layer_marker("DestroyInstance_DONE");
    } else {
        layer_marker("DestroyInstance_NO_FN");
    }
    g_instance = NULL;
    g_next_gipa = NULL;
    g_real_get_features = NULL;
    g_real_get_features2 = NULL;
    g_real_get_format_props = NULL;
    g_real_get_format_props2 = NULL;
    g_instance_count--;
}

/* ============================================================================
 * Section 12: vkCreateDevice — Layer Dispatch Chain
 * ============================================================================ */

static VkLayerDeviceCreateInfo* find_device_layer_info(const VkDeviceCreateInfo* pCreateInfo) {
    const VkBaseOutStructure* pNext = (const VkBaseOutStructure*)pCreateInfo->pNext;
    while (pNext) {
        const VkLayerDeviceCreateInfo* info = (const VkLayerDeviceCreateInfo*)pNext;
        if (info->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
            info->function == VK_LAYER_LINK_INFO) {
            return (VkLayerDeviceCreateInfo*)info;
        }
        pNext = pNext->pNext;
    }
    return NULL;
}

static VkResult headless_CreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice)
{
    TRACE_FN("vkCreateDevice");
    char buf[256];
    snprintf(buf, sizeof(buf), "CD_ENTER phys=%p g_instance=%p exts=%u",
             physicalDevice, g_instance, pCreateInfo->enabledExtensionCount);
    layer_marker(buf);
    LOG("vkCreateDevice intercepted (phys=%p, %u exts)\n",
        physicalDevice, pCreateInfo->enabledExtensionCount);

    if (!g_physical_device) g_physical_device = physicalDevice;

    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++)
        LOG("  dev ext[%u]: %s\n", i, pCreateInfo->ppEnabledExtensionNames[i]);

    VkLayerDeviceCreateInfo* chain = find_device_layer_info(pCreateInfo);
    if (!chain || !chain->u.pLayerInfo) {
        LOG("ERROR: No device layer chain info!\n");
        layer_marker("CD_NO_CHAIN");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    layer_marker("CD_CHAIN_FOUND");

    PFN_vkGetInstanceProcAddr next_gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr next_gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;

    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

    typedef VkResult (*PFN_CD)(VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice*);
    PFN_CD next_create = (PFN_CD)next_gipa(g_instance, "vkCreateDevice");
    if (!next_create) {
        LOG("ERROR: Could not get next vkCreateDevice!\n");
        layer_marker("CD_NEXT_NULL");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    snprintf(buf, sizeof(buf), "CD_NEXT_CREATE=%p", (void*)next_create);
    layer_marker(buf);

    /* Query the ICD's REAL device extensions so we only filter truly spoofed ones.
     * Extensions the ICD supports should pass through; only filter:
     * 1. VK_KHR_swapchain — layer provides headless swapchain implementation
     * 2. Extensions the ICD doesn't actually support (truly spoofed by us) */
    typedef VkResult (*PFN_EDEP)(VkPhysicalDevice, const char*, uint32_t*, VkExtensionProperties*);
    PFN_EDEP edep_fn = (PFN_EDEP)next_gipa(g_instance, "vkEnumerateDeviceExtensionProperties");
    uint32_t icd_ext_count = 0;
    VkExtensionProperties* icd_exts = NULL;
    if (edep_fn) {
        edep_fn(physicalDevice, NULL, &icd_ext_count, NULL);
        if (icd_ext_count > 0) {
            icd_exts = malloc(icd_ext_count * sizeof(VkExtensionProperties));
            if (icd_exts)
                edep_fn(physicalDevice, NULL, &icd_ext_count, icd_exts);
        }
    }

    const char** filtered = malloc(pCreateInfo->enabledExtensionCount * sizeof(char*));
    uint32_t fc = 0;
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
        const char* ext = pCreateInfo->ppEnabledExtensionNames[i];

        /* Always filter swapchain — layer provides headless implementation */
        if (strcmp(ext, "VK_KHR_swapchain") == 0 ||
            strcmp(ext, "VK_KHR_swapchain_mutable_format") == 0) {
            LOG("Filtering layer-provided extension: %s\n", ext);
            continue;
        }

        /* Filter extensions that crash through FEX thunks */
        if (strcmp(ext, "VK_KHR_map_memory2") == 0 ||
            strcmp(ext, "VK_EXT_map_memory_placed") == 0) {
            LOG("Filtering dangerous extension: %s\n", ext);
            continue;
        }

        /* Check if ICD actually supports this extension */
        int icd_has_it = 0;
        for (uint32_t j = 0; j < icd_ext_count && icd_exts; j++) {
            if (strcmp(ext, icd_exts[j].extensionName) == 0) {
                icd_has_it = 1;
                break;
            }
        }

        if (icd_has_it) {
            filtered[fc++] = ext;
            LOG("Passing through real ICD extension: %s\n", ext);
        } else {
            LOG("Filtering spoofed extension (ICD lacks): %s\n", ext);
        }
    }
    free(icd_exts);

    VkDeviceCreateInfo modified = *pCreateInfo;
    modified.enabledExtensionCount = fc;
    modified.ppEnabledExtensionNames = filtered;

    snprintf(buf, sizeof(buf), "CD_CALLING_NEXT dev_exts=%u", fc);
    layer_marker(buf);

    VkResult result = next_create(physicalDevice, &modified, pAllocator, pDevice);
    free(filtered);

    snprintf(buf, sizeof(buf), "CD_RETURNED result=%d", result);
    layer_marker(buf);

    if (result == VK_SUCCESS) {
        g_next_gdpa = next_gdpa;
        g_device = *pDevice;
        /* Store in per-device table */
        if (g_device_count < MAX_LAYER_DEVICES) {
            g_device_table[g_device_count].device = *pDevice;
            g_device_table[g_device_count].gdpa = next_gdpa;
            g_device_count++;
        }
        LOG("Device created: %p (tracked %d devices)\n", *pDevice, g_device_count);
        snprintf(buf, sizeof(buf), "CD_OK device=%p gdpa=%p", *pDevice, (void*)next_gdpa);
        layer_marker(buf);
    } else {
        LOG("vkCreateDevice FAILED: %d\n", result);
    }

    return result;
}

static void headless_DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) {
    TRACE_FN("vkDestroyDevice");
    /* Use THIS device's GDPA to resolve vkDestroyDevice */
    typedef void (*PFN)(VkDevice, const VkAllocationCallbacks*);
    PFN fn = (PFN)next_device_proc_for(device, "vkDestroyDevice");
    if (fn) fn(device, pAllocator);

    /* Remove from per-device table */
    for (int i = 0; i < g_device_count; i++) {
        if (g_device_table[i].device == device) {
            /* Shift remaining entries down */
            for (int j = i; j < g_device_count - 1; j++)
                g_device_table[j] = g_device_table[j + 1];
            g_device_count--;
            break;
        }
    }

    /* Only clear globals if THIS was the global device */
    if (g_device == device) {
        if (g_device_count > 0) {
            /* Point globals to another live device */
            g_device = g_device_table[g_device_count - 1].device;
            g_next_gdpa = g_device_table[g_device_count - 1].gdpa;
        } else {
            g_device = NULL;
            g_next_gdpa = NULL;
        }
    }
    LOG("Device destroyed: %p (remaining %d devices)\n", device, g_device_count);
}

/* ============================================================================
 * Section 13: vkGetInstanceProcAddr / vkGetDeviceProcAddr /
 *             vkGetPhysicalDeviceProcAddr (for physical device interception)
 * ============================================================================ */

/* Forward declarations */
static PFN_vkVoidFunction headless_GetInstanceProcAddr(VkInstance instance, const char* pName);
static PFN_vkVoidFunction headless_GetDeviceProcAddr(VkDevice device, const char* pName);

static int gipa_call_count = 0;

/* The Vulkan loader uses pfnGetPhysicalDeviceProcAddr (interface version 2)
 * as the AUTHORITATIVE source for which physical device functions a layer
 * intercepts. If this returns NULL for a function, the loader bypasses the
 * layer entirely for that function's dispatch — even if GIPA returns an
 * interceptor. Without this, our BC spoofing in GIPA is silently ignored. */
static PFN_vkVoidFunction headless_GetPhysicalDeviceProcAddr(VkInstance instance, const char* pName)
{
    if (!pName) return NULL;

    /* textureCompressionBC spoofing for DXVK */
    if (strcmp(pName, "vkGetPhysicalDeviceFeatures") == 0)
        return (PFN_vkVoidFunction)headless_GetPhysicalDeviceFeatures;
    if (strcmp(pName, "vkGetPhysicalDeviceFeatures2") == 0 ||
        strcmp(pName, "vkGetPhysicalDeviceFeatures2KHR") == 0)
        return (PFN_vkVoidFunction)headless_GetPhysicalDeviceFeatures2;
    if (strcmp(pName, "vkGetPhysicalDeviceFormatProperties") == 0)
        return (PFN_vkVoidFunction)headless_GetPhysicalDeviceFormatProperties;
    if (strcmp(pName, "vkGetPhysicalDeviceFormatProperties2") == 0 ||
        strcmp(pName, "vkGetPhysicalDeviceFormatProperties2KHR") == 0)
        return (PFN_vkVoidFunction)headless_GetPhysicalDeviceFormatProperties2;

    /* Surface queries (physical device level) */
    if (strcmp(pName, "vkGetPhysicalDeviceXcbPresentationSupportKHR") == 0)
        return (PFN_vkVoidFunction)headless_GetPhysicalDeviceXcbPresentationSupportKHR;
    if (strcmp(pName, "vkGetPhysicalDeviceXlibPresentationSupportKHR") == 0)
        return (PFN_vkVoidFunction)headless_GetPhysicalDeviceXlibPresentationSupportKHR;
    if (strcmp(pName, "vkGetPhysicalDeviceSurfaceSupportKHR") == 0)
        return (PFN_vkVoidFunction)headless_GetPhysicalDeviceSurfaceSupportKHR;
    if (strcmp(pName, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR") == 0)
        return (PFN_vkVoidFunction)headless_GetPhysicalDeviceSurfaceCapabilitiesKHR;
    if (strcmp(pName, "vkGetPhysicalDeviceSurfaceCapabilities2KHR") == 0)
        return (PFN_vkVoidFunction)headless_GetPhysicalDeviceSurfaceCapabilities2KHR;
    if (strcmp(pName, "vkGetPhysicalDeviceSurfaceFormatsKHR") == 0)
        return (PFN_vkVoidFunction)headless_GetPhysicalDeviceSurfaceFormatsKHR;
    if (strcmp(pName, "vkGetPhysicalDeviceSurfaceFormats2KHR") == 0)
        return (PFN_vkVoidFunction)headless_GetPhysicalDeviceSurfaceFormats2KHR;
    if (strcmp(pName, "vkGetPhysicalDeviceSurfacePresentModesKHR") == 0)
        return (PFN_vkVoidFunction)headless_GetPhysicalDeviceSurfacePresentModesKHR;

    /* Not intercepted — let the loader skip this layer for this function */
    return NULL;
}

static PFN_vkVoidFunction headless_GetInstanceProcAddr(VkInstance instance, const char* pName)
{
    /* Trace ALL GIPA calls (first 200) to see what the loader/Wine queries */
    gipa_call_count++;
    if (gipa_call_count <= 200) {
        char tbuf[256];
        snprintf(tbuf, sizeof(tbuf), "GIPA[%d] inst=%p %s", gipa_call_count, instance, pName ? pName : "(null)");
        layer_marker(tbuf);
    }

    /* Global functions (instance == NULL) */
    if (strcmp(pName, "vkCreateInstance") == 0)
        return (PFN_vkVoidFunction)headless_CreateInstance;
    if (strcmp(pName, "vkEnumerateInstanceExtensionProperties") == 0)
        return (PFN_vkVoidFunction)headless_EnumerateInstanceExtensionProperties;
    if (strcmp(pName, "vkGetInstanceProcAddr") == 0)
        return (PFN_vkVoidFunction)headless_GetInstanceProcAddr;

    /* Instance functions */
    /* DO NOT intercept vkDestroyInstance — causes infinite recursion via
     * next_instance_proc(), same as EnumPD and EDEP. Also, Wine creates
     * TWO instances (probe + real), and clearing g_instance/g_next_gipa
     * when the probe instance is destroyed would break the real instance.
     * Let the loader dispatch directly to the ICD's DestroyInstance. */
    if (strcmp(pName, "vkCreateDevice") == 0)
        return (PFN_vkVoidFunction)headless_CreateDevice;
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0)
        return (PFN_vkVoidFunction)headless_GetDeviceProcAddr;

    /* DO NOT intercept vkEnumeratePhysicalDevices — causes infinite recursion.
     * next_instance_proc() resolves through the loader's dispatch table which
     * includes our layer, so fn() calls back into us. Let the loader dispatch
     * directly to the ICD instead. g_physical_device is set in surface queries. */

    /* DO NOT intercept vkEnumerateDeviceExtensionProperties — causes infinite
     * recursion. next_instance_proc() resolves through the loader's dispatch
     * table which includes our layer, so fn() calls back into us endlessly.
     * VK_KHR_swapchain is declared in the layer JSON's "device_extensions",
     * so the Vulkan loader automatically merges it into the device extension
     * list. Same fix pattern as vkEnumeratePhysicalDevices above. */

    /* Surface functions (VK_KHR_surface + VK_KHR_xcb_surface + VK_KHR_xlib_surface) */
    if (strcmp(pName, "vkCreateXcbSurfaceKHR") == 0)
        return (PFN_vkVoidFunction)headless_CreateXcbSurfaceKHR;
    if (strcmp(pName, "vkCreateXlibSurfaceKHR") == 0)
        return (PFN_vkVoidFunction)headless_CreateXlibSurfaceKHR;
    if (strcmp(pName, "vkCreateHeadlessSurfaceEXT") == 0)
        return (PFN_vkVoidFunction)headless_CreateHeadlessSurfaceEXT;
    if (strcmp(pName, "vkGetPhysicalDeviceXcbPresentationSupportKHR") == 0)
        return (PFN_vkVoidFunction)headless_GetPhysicalDeviceXcbPresentationSupportKHR;
    if (strcmp(pName, "vkGetPhysicalDeviceXlibPresentationSupportKHR") == 0)
        return (PFN_vkVoidFunction)headless_GetPhysicalDeviceXlibPresentationSupportKHR;
    if (strcmp(pName, "vkDestroySurfaceKHR") == 0)
        return (PFN_vkVoidFunction)headless_DestroySurfaceKHR;
    if (strcmp(pName, "vkGetPhysicalDeviceSurfaceSupportKHR") == 0)
        return (PFN_vkVoidFunction)headless_GetPhysicalDeviceSurfaceSupportKHR;
    if (strcmp(pName, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR") == 0)
        return (PFN_vkVoidFunction)headless_GetPhysicalDeviceSurfaceCapabilitiesKHR;
    if (strcmp(pName, "vkGetPhysicalDeviceSurfaceCapabilities2KHR") == 0)
        return (PFN_vkVoidFunction)headless_GetPhysicalDeviceSurfaceCapabilities2KHR;
    if (strcmp(pName, "vkGetPhysicalDeviceSurfaceFormatsKHR") == 0)
        return (PFN_vkVoidFunction)headless_GetPhysicalDeviceSurfaceFormatsKHR;
    if (strcmp(pName, "vkGetPhysicalDeviceSurfaceFormats2KHR") == 0)
        return (PFN_vkVoidFunction)headless_GetPhysicalDeviceSurfaceFormats2KHR;
    if (strcmp(pName, "vkGetPhysicalDeviceSurfacePresentModesKHR") == 0)
        return (PFN_vkVoidFunction)headless_GetPhysicalDeviceSurfacePresentModesKHR;

    /* Swapchain functions (queried via instance) */
    if (strcmp(pName, "vkCreateSwapchainKHR") == 0)
        return (PFN_vkVoidFunction)headless_CreateSwapchainKHR;
    if (strcmp(pName, "vkDestroySwapchainKHR") == 0)
        return (PFN_vkVoidFunction)headless_DestroySwapchainKHR;
    if (strcmp(pName, "vkGetSwapchainImagesKHR") == 0)
        return (PFN_vkVoidFunction)headless_GetSwapchainImagesKHR;
    if (strcmp(pName, "vkAcquireNextImageKHR") == 0)
        return (PFN_vkVoidFunction)headless_AcquireNextImageKHR;
    if (strcmp(pName, "vkQueuePresentKHR") == 0)
        return (PFN_vkVoidFunction)headless_QueuePresentKHR;

    /* Physical device features & format spoofing (textureCompressionBC for DXVK) */
    if (strcmp(pName, "vkGetPhysicalDeviceFeatures") == 0) {
        LOG("GIPA INTERCEPT: %s -> headless_GetPhysicalDeviceFeatures (g_real=%p)\n",
            pName, (void*)g_real_get_features);
        return (PFN_vkVoidFunction)headless_GetPhysicalDeviceFeatures;
    }
    if (strcmp(pName, "vkGetPhysicalDeviceFeatures2") == 0 ||
        strcmp(pName, "vkGetPhysicalDeviceFeatures2KHR") == 0) {
        LOG("GIPA INTERCEPT: %s -> headless_GetPhysicalDeviceFeatures2 (g_real=%p)\n",
            pName, (void*)g_real_get_features2);
        return (PFN_vkVoidFunction)headless_GetPhysicalDeviceFeatures2;
    }
    if (strcmp(pName, "vkGetPhysicalDeviceFormatProperties") == 0) {
        LOG("GIPA INTERCEPT: %s -> headless_GetPhysicalDeviceFormatProperties (g_real=%p)\n",
            pName, (void*)g_real_get_format_props);
        return (PFN_vkVoidFunction)headless_GetPhysicalDeviceFormatProperties;
    }
    if (strcmp(pName, "vkGetPhysicalDeviceFormatProperties2") == 0 ||
        strcmp(pName, "vkGetPhysicalDeviceFormatProperties2KHR") == 0) {
        LOG("GIPA INTERCEPT: %s -> headless_GetPhysicalDeviceFormatProperties2 (g_real=%p)\n",
            pName, (void*)g_real_get_format_props2);
        return (PFN_vkVoidFunction)headless_GetPhysicalDeviceFormatProperties2;
    }

    /* Forward everything else */
    if (g_next_gipa) {
        PFN_vkVoidFunction fn = g_next_gipa(instance, pName);
        /* Log interesting/uncommon lookups */
        if (!fn || strncmp(pName, "vkGet", 5) == 0 || strncmp(pName, "vkCreate", 8) == 0 ||
            strncmp(pName, "vkEnum", 6) == 0 || strncmp(pName, "vkCmd", 4) == 0) {
            LOG("GIPA fwd: %s -> %p (inst=%p)\n", pName, (void*)fn, instance);
        }
        /* File-based markers for key probing functions so we can trace Wine's sequence */
        if (strcmp(pName, "vkGetPhysicalDeviceProperties") == 0 ||
            strcmp(pName, "vkGetPhysicalDeviceProperties2") == 0 ||
            strcmp(pName, "vkGetPhysicalDeviceProperties2KHR") == 0 ||
            strcmp(pName, "vkGetPhysicalDeviceFeatures") == 0 ||
            strcmp(pName, "vkGetPhysicalDeviceFeatures2") == 0 ||
            strcmp(pName, "vkGetPhysicalDeviceFeatures2KHR") == 0 ||
            strcmp(pName, "vkGetPhysicalDeviceMemoryProperties") == 0 ||
            strcmp(pName, "vkGetPhysicalDeviceMemoryProperties2") == 0 ||
            strcmp(pName, "vkGetPhysicalDeviceQueueFamilyProperties") == 0 ||
            strcmp(pName, "vkGetPhysicalDeviceQueueFamilyProperties2") == 0 ||
            strcmp(pName, "vkGetPhysicalDeviceFormatProperties") == 0 ||
            strcmp(pName, "vkGetPhysicalDeviceFormatProperties2") == 0 ||
            strcmp(pName, "vkEnumeratePhysicalDevices") == 0 ||
            strcmp(pName, "vkEnumerateDeviceExtensionProperties") == 0 ||
            strcmp(pName, "vkCreateDevice") == 0 ||
            strcmp(pName, "vkDestroyInstance") == 0) {
            char buf[256];
            snprintf(buf, sizeof(buf), "GIPA_FWD %s -> %p inst=%p", pName, (void*)fn, instance);
            layer_marker(buf);
        }
        return fn;
    }
    LOG("GIPA: %s -> NULL (no g_next_gipa!)\n", pName);
    layer_marker("GIPA_NO_NEXT_GIPA");
    return NULL;
}

/* ============================================================================
 * Diagnostic: vkBeginCommandBuffer / vkEndCommandBuffer / vkQueueSubmit wrappers
 * ============================================================================ */

static VkResult headless_BeginCommandBuffer(VkCommandBuffer cmdBuf, const void* pBeginInfo) {
    int n = __sync_add_and_fetch(&g_beginCmdBuf_count, 1);
    if (n <= 5 || (n % 100) == 0) {
        LOG("vkBeginCommandBuffer #%d (cmdBuf=%p) ENTER\n", n, cmdBuf);
    }
    VkResult r = g_real_BeginCmdBuf(cmdBuf, pBeginInfo);
    if (n <= 5 || (n % 100) == 0) {
        LOG("vkBeginCommandBuffer #%d result=%d DONE\n", n, r);
    }
    return r;
}

static VkResult headless_EndCommandBuffer(VkCommandBuffer cmdBuf) {
    int n = __sync_add_and_fetch(&g_endCmdBuf_count, 1);
    if (n <= 5 || (n % 100) == 0) {
        LOG("vkEndCommandBuffer #%d (cmdBuf=%p)\n", n, cmdBuf);
    }
    return g_real_EndCmdBuf(cmdBuf);
}

static VkResult headless_AllocateCommandBuffers(VkDevice dev, const void* pInfo, VkCommandBuffer* pBufs) {
    TRACE_FN("vkAllocateCommandBuffers");
    /* VkCommandBufferAllocateInfo layout on x86-64:
     * offset 0: sType(4) + pad(4), offset 8: pNext(8),
     * offset 16: commandPool(8), offset 24: level(4), offset 28: count(4) */
    uint64_t pool = 0;
    uint32_t level = 0, count = 0;
    if (pInfo) {
        pool = *(const uint64_t*)((const char*)pInfo + 16);
        level = *(const uint32_t*)((const char*)pInfo + 24);
        count = *(const uint32_t*)((const char*)pInfo + 28);
    }
    LOG("vkAllocateCommandBuffers: dev=%p pool=0x%llx level=%u count=%u pBufs=%p\n",
        dev, (unsigned long long)pool, level, count, (void*)pBufs);
    char tbuf[256];
    snprintf(tbuf, sizeof(tbuf), "ACB dev=%p pool=0x%llx count=%u pBufs=%p real=%p",
             dev, (unsigned long long)pool, count, (void*)pBufs, (void*)g_real_AllocCmdBufs);
    layer_marker(tbuf);

    if (!g_real_AllocCmdBufs) {
        LOG("vkAllocateCommandBuffers: g_real_AllocCmdBufs is NULL!\n");
        layer_marker("ACB_NULL_REAL_FN");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    layer_marker("ACB_CALLING");
    VkResult r = g_real_AllocCmdBufs(dev, pInfo, pBufs);
    snprintf(tbuf, sizeof(tbuf), "ACB_RESULT=%d cmdBuf0=%p", r,
             (pBufs && count > 0) ? (void*)*pBufs : NULL);
    layer_marker(tbuf);
    LOG("vkAllocateCommandBuffers result=%d cmdBuf=%p\n", r, pBufs ? *pBufs : NULL);
    return r;
}

static VkResult headless_QueueSubmit(VkQueue queue, uint32_t submitCount, const void* pSubmits, uint64_t fence) {
    TRACE_FN("vkQueueSubmit");
    LOG("vkQueueSubmit (queue=%p, submits=%u) ENTER\n", queue, submitCount);
    VkResult r = g_real_QueueSubmit(queue, submitCount, pSubmits, fence);
    LOG("vkQueueSubmit result=%d DONE\n", r);
    return r;
}

static VkResult headless_CreateCommandPool(VkDevice dev, const void* pInfo, const void* pAlloc, VkCommandPool* pPool) {
    TRACE_FN("vkCreateCommandPool");
    LOG("vkCreateCommandPool: dev=%p pInfo=%p pAlloc=%p pPool=%p real=%p\n",
        dev, pInfo, pAlloc, (void*)pPool, (void*)g_real_CreateCmdPool);
    if (!g_real_CreateCmdPool) {
        LOG("vkCreateCommandPool: g_real_CreateCmdPool is NULL!\n");
        layer_marker("CCP_NULL_REAL_FN");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    layer_marker("CCP_CALLING");
    VkResult r = g_real_CreateCmdPool(dev, pInfo, pAlloc, pPool);
    char tbuf[128];
    snprintf(tbuf, sizeof(tbuf), "CCP_RESULT=%d pool=0x%llx",
             r, (unsigned long long)(pPool ? *pPool : 0));
    layer_marker(tbuf);
    LOG("vkCreateCommandPool result=%d pool=0x%llx\n", r, (unsigned long long)(pPool ? *pPool : 0));
    return r;
}

/* Logged wrappers for common device functions — helps identify which call
 * triggers Wine's PE→Unix assertion before reaching our layer/ICD */
typedef VkResult (*PFN_vkCreateFence)(VkDevice, const void*, const void*, uint64_t*);
typedef VkResult (*PFN_vkCreateSemaphore)(VkDevice, const void*, const void*, uint64_t*);
typedef VkResult (*PFN_vkCreateEvent)(VkDevice, const void*, const void*, uint64_t*);
typedef void (*PFN_vkDestroyFence)(VkDevice, uint64_t, const void*);
typedef void (*PFN_vkDestroySemaphore)(VkDevice, uint64_t, const void*);
typedef VkResult (*PFN_vkWaitForFences)(VkDevice, uint32_t, const uint64_t*, uint32_t, uint64_t);
typedef VkResult (*PFN_vkResetFences)(VkDevice, uint32_t, const uint64_t*);

static PFN_vkCreateFence g_real_CreateFence = NULL;
static PFN_vkCreateSemaphore g_real_CreateSemaphore = NULL;

static VkResult headless_wrap_CreateFence(VkDevice dev, const void* ci, const void* alloc, uint64_t* out) {
    TRACE_FN("vkCreateFence");
    VkResult r = g_real_CreateFence(dev, ci, alloc, out);
    LOG("vkCreateFence: result=%d handle=0x%llx\n", r, out ? (unsigned long long)*out : 0);
    return r;
}

static VkResult headless_wrap_CreateSemaphore(VkDevice dev, const void* ci, const void* alloc, uint64_t* out) {
    TRACE_FN("vkCreateSemaphore");
    VkResult r = g_real_CreateSemaphore(dev, ci, alloc, out);
    LOG("vkCreateSemaphore: result=%d handle=0x%llx\n", r, out ? (unsigned long long)*out : 0);
    return r;
}

static int gdpa_log_count = 0;

static PFN_vkVoidFunction headless_GetDeviceProcAddr(VkDevice device, const char* pName)
{
    gdpa_log_count++;
    if (gdpa_log_count <= 50) {
        char tbuf[256];
        snprintf(tbuf, sizeof(tbuf), "GDPA[%d] dev=%p %s", gdpa_log_count, device, pName ? pName : "(null)");
        layer_marker(tbuf);
    }

    if (strcmp(pName, "vkGetDeviceProcAddr") == 0)
        return (PFN_vkVoidFunction)headless_GetDeviceProcAddr;
    if (strcmp(pName, "vkDestroyDevice") == 0)
        return (PFN_vkVoidFunction)headless_DestroyDevice;

    /* NOTE: Do NOT intercept vkBeginCommandBuffer/vkEndCommandBuffer here!
     * Dispatchable handle (VkCommandBuffer) dispatch through GDPA causes
     * recursive PE↔unix call that triggers Wine assertion crash.
     *
     * ALSO: Do NOT intercept vkAllocateCommandBuffers, vkCreateCommandPool,
     * vkQueueSubmit, vkCreateFence, vkCreateSemaphore here. Wrapping these
     * with global function pointers corrupts the dispatch chain for Wine
     * internal threads (thread 0090), causing UNIX_CALL to crash and
     * assert(!status) at loader.c:668. Let them pass through to the ICD's
     * dispatch-fixing trampolines instead. */

    /* Swapchain */
    if (strcmp(pName, "vkCreateSwapchainKHR") == 0)
        return (PFN_vkVoidFunction)headless_CreateSwapchainKHR;
    if (strcmp(pName, "vkDestroySwapchainKHR") == 0)
        return (PFN_vkVoidFunction)headless_DestroySwapchainKHR;
    if (strcmp(pName, "vkGetSwapchainImagesKHR") == 0)
        return (PFN_vkVoidFunction)headless_GetSwapchainImagesKHR;
    if (strcmp(pName, "vkAcquireNextImageKHR") == 0)
        return (PFN_vkVoidFunction)headless_AcquireNextImageKHR;
    if (strcmp(pName, "vkQueuePresentKHR") == 0)
        return (PFN_vkVoidFunction)headless_QueuePresentKHR;

    /* Use per-device GDPA — the ICD's GDPA uses dlsym() for safe dispatch.
     * NEVER use GIPA here: it creates dev_ext_trampolines that cause
     * infinite thunk recursion in FEX.
     * Use per-device lookup to ensure correct dispatch for each device. */
    PFN_vkGetDeviceProcAddr dev_gdpa = gdpa_for_device(device);
    PFN_vkVoidFunction fn = NULL;
    if (dev_gdpa && device)
        fn = dev_gdpa(device, pName);

    return fn;
}

/* ============================================================================
 * Section 14: Layer Negotiation Entry Point
 * ============================================================================ */

__attribute__((visibility("default")))
VkResult vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct)
{
    if (!pVersionStruct || pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (pVersionStruct->loaderLayerInterfaceVersion >= 2) {
        pVersionStruct->pfnGetInstanceProcAddr = headless_GetInstanceProcAddr;
        pVersionStruct->pfnGetDeviceProcAddr = headless_GetDeviceProcAddr;
        /* CRITICAL: Must provide pfnGetPhysicalDeviceProcAddr for the loader
         * to route physical device functions through our layer. Without this,
         * the loader bypasses us for vkGetPhysicalDeviceFeatures etc., and our
         * textureCompressionBC spoofing in GIPA is never used for dispatch. */
        pVersionStruct->pfnGetPhysicalDeviceProcAddr = (PFN_vkVoidFunction)headless_GetPhysicalDeviceProcAddr;
    }
    pVersionStruct->loaderLayerInterfaceVersion = 2;

    LOG("Layer negotiation complete (interface version 2, GPDPA=%p)\n",
        (void*)headless_GetPhysicalDeviceProcAddr);
    return VK_SUCCESS;
}

/* Constructor: log that the layer .so was loaded + install SIGABRT handler */
__attribute__((constructor))
static void layer_init(void) {
    LOG("Vulkan headless surface layer loaded (pid=%d)\n", getpid());
    signal(SIGABRT, sigabrt_handler);

    /* Dump mode: HEADLESS_DUMP_FRAMES=N writes first N frames as PPM to /tmp/ */
    const char *dump_env = getenv("HEADLESS_DUMP_FRAMES");
    if (dump_env) {
        g_dump_max_frames = atoi(dump_env);
        if (g_dump_max_frames > 0) {
            g_dump_mode = 1;
            g_dump_frame_count = 0;
            g_dump_summary = fopen("/tmp/frame_summary.txt", "w");
            if (g_dump_summary) {
                fprintf(g_dump_summary, "=== DUMP MODE: capturing %d frames ===\n", g_dump_max_frames);
                fflush(g_dump_summary);
            }
            LOG("DUMP MODE enabled: will capture %d frames to /tmp/frame_NNNN.ppm\n", g_dump_max_frames);
        }
    }
}
