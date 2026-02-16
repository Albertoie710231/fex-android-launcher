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
#define VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO 47
#define VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO 48

#define VK_IMAGE_TYPE_2D 1
#define VK_SAMPLE_COUNT_1_BIT 1
#define VK_IMAGE_TILING_LINEAR 1
#define VK_SHARING_MODE_EXCLUSIVE 0
#define VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT 0x02
#define VK_MEMORY_PROPERTY_HOST_COHERENT_BIT 0x04

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

typedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddr)(VkInstance, const char*);
typedef PFN_vkVoidFunction (*PFN_vkGetDeviceProcAddr)(VkDevice, const char*);

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

static PFN_vkVoidFunction next_device_proc(const char* name) {
    if (g_next_gdpa && g_device)
        return g_next_gdpa(g_device, name);
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
            if (!rb2->robustBufferAccess2) {
                rb2->robustBufferAccess2 = VK_TRUE;
                LOG("Spoofed robustBufferAccess2 = VK_TRUE\n");
            }
            if (!rb2->robustImageAccess2) {
                rb2->robustImageAccess2 = VK_TRUE;
                LOG("Spoofed robustImageAccess2 = VK_TRUE\n");
            }
            if (!rb2->nullDescriptor) {
                rb2->nullDescriptor = VK_TRUE;
                LOG("Spoofed nullDescriptor = VK_TRUE\n");
            }
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
    layer_marker("CreateXcbSurface_ENTER");
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
    layer_marker("CreateXlibSurface_ENTER");
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

static VkResult headless_CreateSwapchainKHR(
    VkDevice device,
    const VkSwapchainCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSwapchainKHR* pSwapchain)
{
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
        PFN fn = (PFN)next_device_proc("vkCreateSwapchainKHR");
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

    /* Get Vulkan functions for image creation */
    typedef VkResult (*PFN_CI)(VkDevice, const VkImageCreateInfo*, const VkAllocationCallbacks*, VkImage*);
    typedef void (*PFN_GMR)(VkDevice, VkImage, VkMemoryRequirements*);
    typedef VkResult (*PFN_AM)(VkDevice, const VkMemoryAllocateInfo*, const VkAllocationCallbacks*, VkDeviceMemory*);
    typedef VkResult (*PFN_BIM)(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize);
    typedef void (*PFN_GSL)(VkDevice, VkImage, const VkImageSubresource*, VkSubresourceLayout*);

    PFN_CI fn_ci = (PFN_CI)next_device_proc("vkCreateImage");
    PFN_GMR fn_gmr = (PFN_GMR)next_device_proc("vkGetImageMemoryRequirements");
    PFN_AM fn_am = (PFN_AM)next_device_proc("vkAllocateMemory");
    PFN_BIM fn_bim = (PFN_BIM)next_device_proc("vkBindImageMemory");
    PFN_GSL fn_gsl = (PFN_GSL)next_device_proc("vkGetImageSubresourceLayout");

    for (uint32_t i = 0; i < sc->image_count; i++) {
        if (!fn_ci || !fn_gmr || !fn_am || !fn_bim) {
            LOG("Missing core Vulkan functions for image creation!\n");
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
        ici.tiling = VK_IMAGE_TILING_LINEAR;
        ici.usage = pCreateInfo->imageUsage;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = 0; /* UNDEFINED */

        VkResult res = fn_ci(device, &ici, NULL, &sc->images[i]);
        if (res != VK_SUCCESS) {
            LOG("vkCreateImage[%u] failed: %d\n", i, res);
            continue;
        }

        VkMemoryRequirements memReq = {0};
        fn_gmr(device, sc->images[i], &memReq);

        VkMemoryAllocateInfo ai = {0};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = memReq.size;
        ai.memoryTypeIndex = find_host_visible_mem(memReq.memoryTypeBits);

        res = fn_am(device, &ai, NULL, &sc->memory[i]);
        if (res != VK_SUCCESS) {
            LOG("vkAllocateMemory[%u] failed: %d\n", i, res);
            continue;
        }

        res = fn_bim(device, sc->images[i], sc->memory[i], 0);
        if (res != VK_SUCCESS) {
            LOG("vkBindImageMemory[%u] failed: %d\n", i, res);
            continue;
        }

        if (fn_gsl) {
            VkImageSubresource sub = { 1, 0, 0 }; /* COLOR_BIT */
            VkSubresourceLayout layout = {0};
            fn_gsl(device, sc->images[i], &sub, &layout);
            sc->row_pitch[i] = layout.rowPitch;
        } else {
            sc->row_pitch[i] = sc->width * 4;
        }

        LOG("Image[%u]: 0x%lx, mem=0x%lx, pitch=%lu\n",
            i, (unsigned long)sc->images[i], (unsigned long)sc->memory[i],
            (unsigned long)sc->row_pitch[i]);
    }

    pthread_mutex_lock(&g_mutex);
    sc->next = g_swapchains;
    g_swapchains = sc;
    pthread_mutex_unlock(&g_mutex);

    *pSwapchain = sc->handle;
    snprintf(scbuf, sizeof(scbuf), "SC_OK handle=0x%lx images=%u",
             (unsigned long)sc->handle, sc->image_count);
    layer_marker(scbuf);
    LOG("Created swapchain 0x%lx with %u images\n", (unsigned long)sc->handle, sc->image_count);
    return VK_SUCCESS;
}

static void headless_DestroySwapchainKHR(
    VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator)
{
    if (!is_our_swapchain(swapchain)) {
        typedef void (*PFN)(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*);
        PFN fn = (PFN)next_device_proc("vkDestroySwapchainKHR");
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

    PFN_WI fn_wait = (PFN_WI)next_device_proc("vkDeviceWaitIdle");
    PFN_DI fn_di = (PFN_DI)next_device_proc("vkDestroyImage");
    PFN_FM fn_fm = (PFN_FM)next_device_proc("vkFreeMemory");

    if (fn_wait) fn_wait(dev);
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
    SwapchainEntry* sc = find_swapchain(swapchain);
    if (!sc) {
        typedef VkResult (*PFN)(VkDevice, VkSwapchainKHR, uint32_t*, VkImage*);
        PFN fn = (PFN)next_device_proc("vkGetSwapchainImagesKHR");
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
    SwapchainEntry* sc = find_swapchain(swapchain);
    if (!sc) {
        typedef VkResult (*PFN)(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t*);
        PFN fn = (PFN)next_device_proc("vkAcquireNextImageKHR");
        if (fn) return fn(device, swapchain, timeout, sem, fence, pImageIndex);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *pImageIndex = sc->current_image;
    sc->current_image = (sc->current_image + 1) % sc->image_count;
    return VK_SUCCESS;
}

static int g_present_count = 0;

static VkResult headless_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
{
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
        if (idx < sc->image_count && sc->memory[idx]) {
            /* Wait for GPU */
            typedef VkResult (*PFN_QWI)(VkQueue);
            PFN_QWI fn_qwi = (PFN_QWI)next_device_proc("vkQueueWaitIdle");
            if (fn_qwi && queue) fn_qwi(queue);

            /* Map and send */
            typedef VkResult (*PFN_MM)(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkFlags, void**);
            typedef void (*PFN_UM)(VkDevice, VkDeviceMemory);

            PFN_MM fn_map = (PFN_MM)next_device_proc("vkMapMemory");
            PFN_UM fn_unmap = (PFN_UM)next_device_proc("vkUnmapMemory");

            if (fn_map && fn_unmap) {
                void* mapped = NULL;
                size_t pitch = sc->row_pitch[idx];
                if (!pitch) pitch = sc->width * 4;
                size_t map_size = pitch * sc->height;

                VkResult res = fn_map(sc->device, sc->memory[idx], 0, map_size, 0, &mapped);
                if (res == VK_SUCCESS && mapped) {
                    send_frame(sc->width, sc->height, mapped, pitch);
                    fn_unmap(sc->device, sc->memory[idx]);
                }
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

    /* For other layer names or NULL (global), forward to next layer */
    typedef VkResult (*PFN)(const char*, uint32_t*, VkExtensionProperties*);
    PFN fn = NULL;
    if (g_next_gipa) fn = (PFN)g_next_gipa(NULL, "vkEnumerateInstanceExtensionProperties");
    if (fn) return fn(pLayerName, pCount, pProps);
    return VK_ERROR_INITIALIZATION_FAILED;
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

    /* Check which extensions are already present */
    int has_ext[sizeof(inject_exts) / sizeof(inject_exts[0])];
    memset(has_ext, 0, sizeof(has_ext));
    int need_inject = 0;

    if (real_count > 0) {
        VkExtensionProperties* tmp = malloc(real_count * sizeof(VkExtensionProperties));
        if (tmp) {
            uint32_t tc = real_count;
            fn(pd, pLayerName, &tc, tmp);
            for (uint32_t i = 0; i < tc; i++) {
                for (int j = 0; j < num_inject; j++) {
                    if (strcmp(tmp[i].extensionName, inject_exts[j].name) == 0)
                        has_ext[j] = 1;
                }
            }
            free(tmp);
        }
    }

    for (int j = 0; j < num_inject; j++)
        if (!has_ext[j]) need_inject++;

    uint32_t total = real_count + need_inject;
    if (!pProps) { *pCount = total; return VK_SUCCESS; }

    uint32_t get = *pCount < real_count ? *pCount : real_count;
    res = fn(pd, pLayerName, &get, pProps);

    /* Append missing extensions */
    uint32_t idx = get;
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
    layer_marker("CI_ENTER");
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

    /* Filter extensions that we spoof — the ICD doesn't actually support them.
     * We advertise them in enumeration and spoof their features, but the ICD
     * would reject vkCreateDevice if we pass them through. */
    static const char* spoofed_exts[] = {
        "VK_KHR_swapchain",
        "VK_EXT_depth_clip_enable",
        "VK_EXT_custom_border_color",
        "VK_EXT_transform_feedback",
        "VK_EXT_robustness2",
        "VK_KHR_maintenance5",
        "VK_KHR_maintenance6",
        "VK_KHR_pipeline_library",
        "VK_EXT_non_seamless_cube_map",
        "VK_EXT_graphics_pipeline_library",
    };
    const char** filtered = malloc(pCreateInfo->enabledExtensionCount * sizeof(char*));
    uint32_t fc = 0;
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
        const char* ext = pCreateInfo->ppEnabledExtensionNames[i];
        int is_spoofed = 0;
        for (int j = 0; j < (int)(sizeof(spoofed_exts)/sizeof(spoofed_exts[0])); j++) {
            if (strcmp(ext, spoofed_exts[j]) == 0) { is_spoofed = 1; break; }
        }
        if (is_spoofed) {
            LOG("Filtering spoofed device extension: %s\n", ext);
        } else {
            filtered[fc++] = ext;
        }
    }

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
        LOG("Device created: %p\n", *pDevice);
        snprintf(buf, sizeof(buf), "CD_OK device=%p gdpa=%p", *pDevice, (void*)next_gdpa);
        layer_marker(buf);
    } else {
        LOG("vkCreateDevice FAILED: %d\n", result);
    }

    return result;
}

static void headless_DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) {
    typedef void (*PFN)(VkDevice, const VkAllocationCallbacks*);
    PFN fn = (PFN)next_device_proc("vkDestroyDevice");
    if (fn) fn(device, pAllocator);
    g_device = NULL;
    g_next_gdpa = NULL;
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
    if (strcmp(pName, "vkGetPhysicalDeviceSurfaceFormatsKHR") == 0)
        return (PFN_vkVoidFunction)headless_GetPhysicalDeviceSurfaceFormatsKHR;
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
    if (strcmp(pName, "vkGetPhysicalDeviceSurfaceFormatsKHR") == 0)
        return (PFN_vkVoidFunction)headless_GetPhysicalDeviceSurfaceFormatsKHR;
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

static PFN_vkVoidFunction headless_GetDeviceProcAddr(VkDevice device, const char* pName)
{
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0)
        return (PFN_vkVoidFunction)headless_GetDeviceProcAddr;
    if (strcmp(pName, "vkDestroyDevice") == 0)
        return (PFN_vkVoidFunction)headless_DestroyDevice;

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

    /* FEX thunks' GDPA crashes (segfault) for most device functions.
     * Use GIPA exclusively — safe and returns all device functions. */
    PFN_vkVoidFunction fn = NULL;
    if (g_next_gipa && g_instance)
        fn = g_next_gipa(g_instance, pName);
    /* Last resort: try GDPA if GIPA failed (shouldn't happen) */
    if (!fn && g_next_gdpa)
        fn = g_next_gdpa(device, pName);

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

/* Constructor: log that the layer .so was loaded */
__attribute__((constructor))
static void layer_init(void) {
    LOG("Vulkan headless surface layer loaded (pid=%d)\n", getpid());
}
