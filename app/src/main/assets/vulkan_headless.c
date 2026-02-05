/*
 * Vulkan XCB-to-Xlib Surface Bridge via LD_PRELOAD
 *
 * This library provides VK_KHR_xcb_surface support by bridging to VK_KHR_xlib_surface.
 * Apps like vkcube that require XCB will work because we:
 * 1. Advertise VK_KHR_xcb_surface
 * 2. Intercept vkCreateXcbSurfaceKHR and create a real Xlib surface instead
 * 3. Forward all surface operations to the real Xlib surface
 *
 * Also provides VK_EXT_headless_surface for headless rendering tests.
 *
 * Usage: LD_PRELOAD=/lib/libvulkan_headless.so vkcube
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>

// Minimal Vulkan types
typedef uint32_t VkFlags;
typedef uint32_t VkBool32;
typedef uint64_t VkDeviceSize;

#define VK_TRUE 1
#define VK_FALSE 0
#define VK_SUCCESS 0
#define VK_INCOMPLETE 5
#define VK_ERROR_OUT_OF_HOST_MEMORY (-1)
#define VK_ERROR_EXTENSION_NOT_PRESENT (-7)
#define VK_ERROR_INITIALIZATION_FAILED (-3)
#define VK_MAX_EXTENSION_NAME_SIZE 256

#define VK_FORMAT_B8G8R8A8_UNORM 44
#define VK_FORMAT_R8G8B8A8_UNORM 37
#define VK_COLOR_SPACE_SRGB_NONLINEAR_KHR 0
#define VK_PRESENT_MODE_FIFO_KHR 2
#define VK_PRESENT_MODE_IMMEDIATE_KHR 0

#define VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR 0x00000001
#define VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR 0x00000001
#define VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT 0x00000010
#define VK_IMAGE_USAGE_TRANSFER_SRC_BIT 0x00000001
#define VK_IMAGE_USAGE_TRANSFER_DST_BIT 0x00000002

#define VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR 1000005000
#define VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR 1000004000
#define VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT 1000256000

typedef int VkResult;
typedef void* VkInstance;
typedef void* VkPhysicalDevice;
typedef uint64_t VkSurfaceKHR;
typedef void (*PFN_vkVoidFunction)(void);

typedef struct VkExtensionProperties {
    char extensionName[VK_MAX_EXTENSION_NAME_SIZE];
    uint32_t specVersion;
} VkExtensionProperties;

typedef struct VkExtent2D {
    uint32_t width;
    uint32_t height;
} VkExtent2D;

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

typedef struct VkSurfaceFormatKHR {
    int format;
    int colorSpace;
} VkSurfaceFormatKHR;

typedef int VkPresentModeKHR;
typedef struct VkAllocationCallbacks VkAllocationCallbacks;

// Swapchain types
typedef uint64_t VkSwapchainKHR;
typedef uint64_t VkImage;
typedef uint64_t VkSemaphore;
typedef uint64_t VkFence;
typedef void* VkDevice;
typedef void* VkQueue;

#define VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR 1000001000
#define VK_STRUCTURE_TYPE_PRESENT_INFO_KHR 1000001001
#define VK_SHARING_MODE_EXCLUSIVE 0
#define VK_SUBOPTIMAL_KHR 1000001003
#define VK_NOT_READY 1

typedef struct VkSwapchainCreateInfoKHR {
    int sType;
    const void* pNext;
    VkFlags flags;
    VkSurfaceKHR surface;
    uint32_t minImageCount;
    int imageFormat;
    int imageColorSpace;
    VkExtent2D imageExtent;
    uint32_t imageArrayLayers;
    VkFlags imageUsage;
    int imageSharingMode;
    uint32_t queueFamilyIndexCount;
    const uint32_t* pQueueFamilyIndices;
    VkFlags preTransform;
    VkFlags compositeAlpha;
    int presentMode;
    VkBool32 clipped;
    VkSwapchainKHR oldSwapchain;
} VkSwapchainCreateInfoKHR;

typedef struct VkPresentInfoKHR {
    int sType;
    const void* pNext;
    uint32_t waitSemaphoreCount;
    const VkSemaphore* pWaitSemaphores;
    uint32_t swapchainCount;
    const VkSwapchainKHR* pSwapchains;
    const uint32_t* pImageIndices;
    VkResult* pResults;
} VkPresentInfoKHR;

// Xlib surface create info
typedef struct VkXlibSurfaceCreateInfoKHR {
    int sType;
    const void* pNext;
    VkFlags flags;
    void* dpy;           // Display*
    unsigned long window; // Window
} VkXlibSurfaceCreateInfoKHR;

// XCB surface create info
typedef struct VkXcbSurfaceCreateInfoKHR {
    int sType;
    const void* pNext;
    VkFlags flags;
    void* connection;    // xcb_connection_t*
    uint32_t window;     // xcb_window_t
} VkXcbSurfaceCreateInfoKHR;

// Headless surface create info
typedef struct VkHeadlessSurfaceCreateInfoEXT {
    int sType;
    const void* pNext;
    VkFlags flags;
} VkHeadlessSurfaceCreateInfoEXT;

// Instance create info
typedef struct VkApplicationInfo {
    int sType;
    const void* pNext;
    const char* pApplicationName;
    uint32_t applicationVersion;
    const char* pEngineName;
    uint32_t engineVersion;
    uint32_t apiVersion;
} VkApplicationInfo;

typedef struct VkInstanceCreateInfo {
    int sType;
    const void* pNext;
    VkFlags flags;
    const VkApplicationInfo* pApplicationInfo;
    uint32_t enabledLayerCount;
    const char* const* ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char* const* ppEnabledExtensionNames;
} VkInstanceCreateInfo;

// Function pointer types
typedef VkResult (*PFN_vkEnumerateInstanceExtensionProperties)(const char*, uint32_t*, VkExtensionProperties*);
typedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddr)(VkInstance, const char*);
typedef PFN_vkVoidFunction (*PFN_vkGetDeviceProcAddr)(VkDevice, const char*);
typedef VkResult (*PFN_vkCreateInstance)(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
typedef VkResult (*PFN_vkCreateXlibSurfaceKHR)(VkInstance, const VkXlibSurfaceCreateInfoKHR*, const VkAllocationCallbacks*, VkSurfaceKHR*);
typedef void (*PFN_vkDestroySurfaceKHR)(VkInstance, VkSurfaceKHR, const VkAllocationCallbacks*);

// Forward declarations for exported functions
VkResult vkCreateXcbSurfaceKHR(VkInstance, const VkXcbSurfaceCreateInfoKHR*, const VkAllocationCallbacks*, VkSurfaceKHR*);
VkResult vkCreateSwapchainKHR(VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*);
void vkDestroySwapchainKHR(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*);
VkResult vkGetSwapchainImagesKHR(VkDevice, VkSwapchainKHR, uint32_t*, VkImage*);
VkResult vkAcquireNextImageKHR(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t*);
VkResult vkQueuePresentKHR(VkQueue, const VkPresentInfoKHR*);
PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice, const char*);

// Xlib function types (loaded dynamically)
typedef void* (*PFN_XOpenDisplay)(const char*);
typedef int (*PFN_XCloseDisplay)(void*);
typedef unsigned long (*PFN_XCreateSimpleWindow)(void*, unsigned long, int, int, unsigned int, unsigned int, unsigned int, unsigned long, unsigned long);
typedef unsigned long (*PFN_XRootWindow)(void*, int);
typedef int (*PFN_XMapWindow)(void*, unsigned long);
typedef unsigned long (*PFN_XBlackPixel)(void*, int);
typedef unsigned long (*PFN_XWhitePixel)(void*, int);
typedef int (*PFN_XFlush)(void*);
typedef int (*PFN_XDefaultScreen)(void*);

// Real function pointers
static PFN_vkEnumerateInstanceExtensionProperties real_vkEnumerateInstanceExtensionProperties = NULL;
static PFN_vkGetInstanceProcAddr real_vkGetInstanceProcAddr = NULL;
static PFN_vkCreateInstance real_vkCreateInstance = NULL;
static PFN_vkGetDeviceProcAddr real_vkGetDeviceProcAddr = NULL;

// Xlib function pointers (dynamically loaded)
static void* libX11_handle = NULL;
static PFN_XOpenDisplay real_XOpenDisplay = NULL;
static PFN_XCloseDisplay real_XCloseDisplay = NULL;
static PFN_XCreateSimpleWindow real_XCreateSimpleWindow = NULL;
static PFN_XRootWindow real_XRootWindow = NULL;
static PFN_XMapWindow real_XMapWindow = NULL;
static PFN_XBlackPixel real_XBlackPixel = NULL;
static PFN_XWhitePixel real_XWhitePixel = NULL;
static PFN_XFlush real_XFlush = NULL;
static PFN_XDefaultScreen real_XDefaultScreen = NULL;

// Surface tracking
typedef struct SurfaceEntry {
    VkSurfaceKHR our_handle;      // Our fake handle (for headless) or real handle (for xcb->xlib)
    VkSurfaceKHR real_handle;     // Real Xlib surface handle (0 if headless)
    void* display;                // X11 Display* (for cleanup)
    unsigned long window;         // X11 Window (for cleanup)
    int is_headless;              // 1 if headless, 0 if xcb->xlib bridge
    uint32_t width;
    uint32_t height;
    struct SurfaceEntry* next;
} SurfaceEntry;

static SurfaceEntry* g_surfaces = NULL;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_next_handle = 0xBEEF000000000001ULL;
static VkInstance g_current_instance = NULL;
static VkPhysicalDevice g_physical_device = NULL;

// Frame output socket - use TCP localhost for proot compatibility
#define FRAME_SOCKET_PORT 19850
static int g_frame_socket = -1;
static int g_frame_socket_connected = 0;

// Connect to frame output socket (TCP on localhost)
static int connect_frame_socket(void) {
    if (g_frame_socket_connected) return 1;

    g_frame_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (g_frame_socket < 0) {
        fprintf(stderr, "[XCB-Bridge] Failed to create frame socket: %s\n", strerror(errno));
        return 0;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(FRAME_SOCKET_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(g_frame_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        static int error_count = 0;
        if (error_count < 3) {
            fprintf(stderr, "[XCB-Bridge] Failed to connect to frame socket port %d: %s\n",
                    FRAME_SOCKET_PORT, strerror(errno));
            error_count++;
        }
        close(g_frame_socket);
        g_frame_socket = -1;
        return 0;
    }

    g_frame_socket_connected = 1;
    fprintf(stderr, "[XCB-Bridge] Connected to frame socket on port %d\n", FRAME_SOCKET_PORT);

    // Set socket to non-blocking for frame dropping when buffer is full
    int flags = fcntl(g_frame_socket, F_GETFL, 0);
    fcntl(g_frame_socket, F_SETFL, flags | O_NONBLOCK);

    // Use small send buffer to drop frames quickly when display can't keep up
    int bufsize = 500 * 500 * 4 * 2;  // ~2 frames worth
    setsockopt(g_frame_socket, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));

    return 1;
}

// Send a frame to the socket
static void send_frame_pitched(uint32_t width, uint32_t height, const void* pixels, size_t row_pitch) {
    if (!g_frame_socket_connected && !connect_frame_socket()) {
        return;  // Can't connect, skip frame
    }

    // Frame header: width (4 bytes) + height (4 bytes)
    uint32_t header[2] = { width, height };

    ssize_t sent = write(g_frame_socket, header, sizeof(header));
    if (sent != sizeof(header)) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // Buffer full, drop this frame
            static int drop_count = 0;
            drop_count++;
            if (drop_count < 5 || drop_count % 100 == 0) {
                fprintf(stderr, "[XCB-Bridge] Dropping frame (buffer full, dropped %d)\n", drop_count);
            }
            return;
        }
        fprintf(stderr, "[XCB-Bridge] Failed to send frame header: %s\n", strerror(errno));
        close(g_frame_socket);
        g_frame_socket = -1;
        g_frame_socket_connected = 0;
        return;
    }

    // Frame data: RGBA pixels - handle row pitch
    // For non-blocking socket, drop frame if buffer is full (don't retry mid-frame)
    size_t expected_pitch = width * 4;
    if (row_pitch == expected_pitch) {
        // Tightly packed, send directly
        size_t data_size = width * height * 4;
        sent = write(g_frame_socket, pixels, data_size);
        if (sent != (ssize_t)data_size) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Buffer full, frame already partially sent - this is bad, reconnect
                close(g_frame_socket);
                g_frame_socket = -1;
                g_frame_socket_connected = 0;
            }
            return;
        }
    } else {
        // Row pitch differs - send row by row
        const uint8_t* src = (const uint8_t*)pixels;
        for (uint32_t y = 0; y < height; y++) {
            sent = write(g_frame_socket, src, expected_pitch);
            if (sent != (ssize_t)expected_pitch) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    close(g_frame_socket);
                    g_frame_socket = -1;
                    g_frame_socket_connected = 0;
                }
                return;
            }
            src += row_pitch;
        }
    }

    static int frame_count = 0;
    if (frame_count < 5 || frame_count % 60 == 0) {
        fprintf(stderr, "[XCB-Bridge] Sent frame %d: %ux%u (pitch=%zu)\n",
                frame_count, width, height, row_pitch);
    }
    frame_count++;
}

// Legacy wrapper for compatibility
static void send_frame(uint32_t width, uint32_t height, const void* pixels) {
    send_frame_pitched(width, height, pixels, width * 4);
}

// Load Xlib dynamically
static int load_xlib(void) {
    if (libX11_handle) return 1;

    libX11_handle = dlopen("libX11.so.6", RTLD_NOW | RTLD_GLOBAL);
    if (!libX11_handle) {
        libX11_handle = dlopen("libX11.so", RTLD_NOW | RTLD_GLOBAL);
    }
    if (!libX11_handle) {
        fprintf(stderr, "[XCB-Bridge] Failed to load libX11: %s\n", dlerror());
        return 0;
    }

    real_XOpenDisplay = (PFN_XOpenDisplay)dlsym(libX11_handle, "XOpenDisplay");
    real_XCloseDisplay = (PFN_XCloseDisplay)dlsym(libX11_handle, "XCloseDisplay");
    real_XCreateSimpleWindow = (PFN_XCreateSimpleWindow)dlsym(libX11_handle, "XCreateSimpleWindow");
    real_XRootWindow = (PFN_XRootWindow)dlsym(libX11_handle, "XRootWindow");
    real_XMapWindow = (PFN_XMapWindow)dlsym(libX11_handle, "XMapWindow");
    real_XBlackPixel = (PFN_XBlackPixel)dlsym(libX11_handle, "XBlackPixel");
    real_XWhitePixel = (PFN_XWhitePixel)dlsym(libX11_handle, "XWhitePixel");
    real_XFlush = (PFN_XFlush)dlsym(libX11_handle, "XFlush");
    real_XDefaultScreen = (PFN_XDefaultScreen)dlsym(libX11_handle, "XDefaultScreen");

    if (!real_XOpenDisplay || !real_XCreateSimpleWindow) {
        fprintf(stderr, "[XCB-Bridge] Failed to load Xlib functions\n");
        return 0;
    }

    fprintf(stderr, "[XCB-Bridge] Loaded libX11.so successfully\n");
    return 1;
}

// Check if surface is one of ours (headless)
static int is_headless_surface(VkSurfaceKHR surface) {
    return (surface & 0xFFFF000000000000ULL) == 0xBEEF000000000000ULL;
}

// Find surface entry
static SurfaceEntry* find_surface(VkSurfaceKHR handle) {
    pthread_mutex_lock(&g_mutex);
    SurfaceEntry* s = g_surfaces;
    while (s) {
        if (s->our_handle == handle) {
            pthread_mutex_unlock(&g_mutex);
            return s;
        }
        s = s->next;
    }
    pthread_mutex_unlock(&g_mutex);
    return NULL;
}

// Add surface entry
static SurfaceEntry* add_surface(VkSurfaceKHR our_handle, VkSurfaceKHR real_handle,
                                  void* display, unsigned long window, int is_headless,
                                  uint32_t width, uint32_t height) {
    SurfaceEntry* entry = malloc(sizeof(SurfaceEntry));
    if (!entry) return NULL;

    entry->our_handle = our_handle;
    entry->real_handle = real_handle;
    entry->display = display;
    entry->window = window;
    entry->is_headless = is_headless;
    entry->width = width;
    entry->height = height;

    pthread_mutex_lock(&g_mutex);
    entry->next = g_surfaces;
    g_surfaces = entry;
    pthread_mutex_unlock(&g_mutex);

    return entry;
}

// Create headless surface
static VkResult my_vkCreateHeadlessSurfaceEXT(
    VkInstance instance,
    const VkHeadlessSurfaceCreateInfoEXT* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSurfaceKHR* pSurface)
{
    (void)instance; (void)pCreateInfo; (void)pAllocator;

    VkSurfaceKHR handle = g_next_handle++;
    SurfaceEntry* entry = add_surface(handle, 0, NULL, 0, 1, 1920, 1080);
    if (!entry) return VK_ERROR_OUT_OF_HOST_MEMORY;

    *pSurface = handle;
    fprintf(stderr, "[XCB-Bridge] Created headless surface: 0x%lx\n", (unsigned long)handle);
    return VK_SUCCESS;
}

// Create XCB surface - create a headless-like surface that works with the real swapchain
// EXPORTED as global symbol so LD_PRELOAD can intercept direct calls
VkResult vkCreateXcbSurfaceKHR(
    VkInstance instance,
    const VkXcbSurfaceCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSurfaceKHR* pSurface)
{
    (void)pCreateInfo; (void)pAllocator;

    fprintf(stderr, "[XCB-Bridge] vkCreateXcbSurfaceKHR called\n");

    // Instead of trying to bridge to Xlib (which fails because fake XCB breaks libX11),
    // we create a surface that uses the ICD's internal surfaceless/headless rendering.
    // This allows vkcube to render, though output won't be visible.

    // Try to use the ICD's surfaceless rendering capability
    // Vortek ICD advertises VK_GOOGLE_surfaceless_query which means it can render without a surface

    // For now, create a "fake" surface that we track, but forward surface queries to the ICD
    // The ICD will use its internal headless rendering path

    VkSurfaceKHR handle = g_next_handle++;

    // Mark as NOT headless (is_headless=0) so surface queries go to real ICD
    // The ICD should handle surfaceless rendering
    SurfaceEntry* entry = add_surface(handle, 0, NULL, 0, 0, 500, 500);
    if (!entry) return VK_ERROR_OUT_OF_HOST_MEMORY;

    // Store instance for later use
    g_current_instance = instance;

    *pSurface = handle;
    fprintf(stderr, "[XCB-Bridge] Created XCB surface: 0x%lx (using ICD's surfaceless path)\n",
            (unsigned long)handle);

    return VK_SUCCESS;
}

// XCB presentation support - always return true
static VkBool32 my_vkGetPhysicalDeviceXcbPresentationSupportKHR(
    VkPhysicalDevice physicalDevice,
    uint32_t queueFamilyIndex,
    void* connection,
    uint32_t visual_id)
{
    (void)physicalDevice; (void)queueFamilyIndex; (void)connection; (void)visual_id;
    fprintf(stderr, "[XCB-Bridge] vkGetPhysicalDeviceXcbPresentationSupportKHR -> VK_TRUE\n");
    return VK_TRUE;
}

// Destroy surface
static void my_vkDestroySurfaceKHR(
    VkInstance instance,
    VkSurfaceKHR surface,
    const VkAllocationCallbacks* pAllocator)
{
    SurfaceEntry* entry = find_surface(surface);

    if (entry && entry->is_headless) {
        // Headless surface - just free our tracking
        fprintf(stderr, "[XCB-Bridge] Destroying headless surface: 0x%lx\n", (unsigned long)surface);
    } else if (entry) {
        // XCB->Xlib bridge surface - destroy real surface and X resources
        fprintf(stderr, "[XCB-Bridge] Destroying bridged surface: 0x%lx\n", (unsigned long)surface);

        // Destroy real Vulkan surface
        if (entry->real_handle && real_vkGetInstanceProcAddr) {
            PFN_vkDestroySurfaceKHR fn = (PFN_vkDestroySurfaceKHR)
                real_vkGetInstanceProcAddr(instance, "vkDestroySurfaceKHR");
            if (fn) fn(instance, entry->real_handle, pAllocator);
        }

        // Close X display (this also destroys the window)
        if (entry->display && real_XCloseDisplay) {
            real_XCloseDisplay(entry->display);
        }
    } else {
        // Unknown surface - forward to real implementation
        if (real_vkGetInstanceProcAddr) {
            PFN_vkDestroySurfaceKHR fn = (PFN_vkDestroySurfaceKHR)
                real_vkGetInstanceProcAddr(instance, "vkDestroySurfaceKHR");
            if (fn) fn(instance, surface, pAllocator);
        }
        return;
    }

    // Remove from tracking list
    pthread_mutex_lock(&g_mutex);
    SurfaceEntry** pp = &g_surfaces;
    while (*pp) {
        if ((*pp)->our_handle == surface) {
            SurfaceEntry* to_free = *pp;
            *pp = (*pp)->next;
            free(to_free);
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_mutex);
}

// Surface support - for our surfaces (headless and fake XCB)
static VkResult my_vkGetPhysicalDeviceSurfaceSupportKHR(
    VkPhysicalDevice physicalDevice,
    uint32_t queueFamilyIndex,
    VkSurfaceKHR surface,
    VkBool32* pSupported)
{
    (void)queueFamilyIndex;

    // Store physical device for later memory property queries
    if (physicalDevice && !g_physical_device) {
        g_physical_device = physicalDevice;
        fprintf(stderr, "[XCB-Bridge] Captured physical device: %p\n", physicalDevice);
    }

    SurfaceEntry* entry = find_surface(surface);
    if (entry) {
        // All our surfaces (headless and fake XCB) support presentation
        *pSupported = VK_TRUE;
        return VK_SUCCESS;
    }

    // Forward unknown surfaces to real implementation
    if (real_vkGetInstanceProcAddr && g_current_instance) {
        typedef VkResult (*PFN)(VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32*);
        PFN fn = (PFN)real_vkGetInstanceProcAddr(g_current_instance, "vkGetPhysicalDeviceSurfaceSupportKHR");
        if (fn) return fn(physicalDevice, queueFamilyIndex, surface, pSupported);
    }
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

// Surface capabilities - for our surfaces (headless and fake XCB)
static VkResult my_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface,
    VkSurfaceCapabilitiesKHR* pCapabilities)
{
    (void)physicalDevice;

    SurfaceEntry* entry = find_surface(surface);
    if (entry) {
        // Provide capabilities for all our surfaces
        pCapabilities->minImageCount = 2;
        pCapabilities->maxImageCount = 8;
        pCapabilities->currentExtent.width = entry->width;
        pCapabilities->currentExtent.height = entry->height;
        pCapabilities->minImageExtent.width = 1;
        pCapabilities->minImageExtent.height = 1;
        pCapabilities->maxImageExtent.width = 16384;
        pCapabilities->maxImageExtent.height = 16384;
        pCapabilities->maxImageArrayLayers = 1;
        pCapabilities->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        pCapabilities->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        pCapabilities->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        pCapabilities->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                              VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        fprintf(stderr, "[XCB-Bridge] Surface capabilities: %ux%u\n", entry->width, entry->height);
        return VK_SUCCESS;
    }

    // Forward unknown surfaces to real implementation
    if (real_vkGetInstanceProcAddr && g_current_instance) {
        typedef VkResult (*PFN)(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR*);
        PFN fn = (PFN)real_vkGetInstanceProcAddr(g_current_instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        if (fn) return fn(physicalDevice, surface, pCapabilities);
    }
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

// Surface formats - for our surfaces
static VkResult my_vkGetPhysicalDeviceSurfaceFormatsKHR(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface,
    uint32_t* pCount,
    VkSurfaceFormatKHR* pFormats)
{
    (void)physicalDevice;

    SurfaceEntry* entry = find_surface(surface);
    if (entry) {
        static const VkSurfaceFormatKHR formats[] = {
            { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
            { VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
        };
        if (!pFormats) { *pCount = 2; return VK_SUCCESS; }
        uint32_t copy = *pCount < 2 ? *pCount : 2;
        memcpy(pFormats, formats, copy * sizeof(VkSurfaceFormatKHR));
        *pCount = copy;
        fprintf(stderr, "[XCB-Bridge] Surface formats: returning %u formats\n", copy);
        return copy < 2 ? VK_INCOMPLETE : VK_SUCCESS;
    }

    // Forward unknown surfaces
    if (real_vkGetInstanceProcAddr && g_current_instance) {
        typedef VkResult (*PFN)(VkPhysicalDevice, VkSurfaceKHR, uint32_t*, VkSurfaceFormatKHR*);
        PFN fn = (PFN)real_vkGetInstanceProcAddr(g_current_instance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
        if (fn) return fn(physicalDevice, surface, pCount, pFormats);
    }
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

// Present modes - for our surfaces
static VkResult my_vkGetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface,
    uint32_t* pCount,
    VkPresentModeKHR* pModes)
{
    (void)physicalDevice;

    SurfaceEntry* entry = find_surface(surface);
    if (entry) {
        static const VkPresentModeKHR modes[] = { VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR };
        if (!pModes) { *pCount = 2; return VK_SUCCESS; }
        uint32_t copy = *pCount < 2 ? *pCount : 2;
        memcpy(pModes, modes, copy * sizeof(VkPresentModeKHR));
        *pCount = copy;
        fprintf(stderr, "[XCB-Bridge] Present modes: returning %u modes\n", copy);
        return copy < 2 ? VK_INCOMPLETE : VK_SUCCESS;
    }

    // Forward unknown surfaces
    if (real_vkGetInstanceProcAddr && g_current_instance) {
        typedef VkResult (*PFN)(VkPhysicalDevice, VkSurfaceKHR, uint32_t*, VkPresentModeKHR*);
        PFN fn = (PFN)real_vkGetInstanceProcAddr(g_current_instance, "vkGetPhysicalDeviceSurfacePresentModesKHR");
        if (fn) return fn(physicalDevice, surface, pCount, pModes);
    }
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

// ============================================================================
// Swapchain emulation for fake surfaces
// ============================================================================

#define MAX_SWAPCHAIN_IMAGES 8
typedef uint64_t VkDeviceMemory;

typedef struct SwapchainEntry {
    VkSwapchainKHR handle;
    VkSurfaceKHR surface;
    VkDevice device;
    uint32_t image_count;
    VkImage images[MAX_SWAPCHAIN_IMAGES];  // Real image handles
    VkDeviceMemory memory[MAX_SWAPCHAIN_IMAGES];  // Memory for each image
    VkDeviceSize row_pitch[MAX_SWAPCHAIN_IMAGES];  // Row pitch for LINEAR images
    uint32_t width;
    uint32_t height;
    int format;
    uint32_t current_image;
    struct SwapchainEntry* next;
} SwapchainEntry;

static SwapchainEntry* g_swapchains = NULL;
static uint64_t g_next_swapchain = 0xDEAD000000000001ULL;
static uint64_t g_next_image = 0xFACE000000000001ULL;
static VkDevice g_current_device = NULL;

// Find swapchain entry
static SwapchainEntry* find_swapchain(VkSwapchainKHR handle) {
    pthread_mutex_lock(&g_mutex);
    SwapchainEntry* s = g_swapchains;
    while (s) {
        if (s->handle == handle) {
            pthread_mutex_unlock(&g_mutex);
            return s;
        }
        s = s->next;
    }
    pthread_mutex_unlock(&g_mutex);
    return NULL;
}

// Check if swapchain is one of ours
static int is_our_swapchain(VkSwapchainKHR swapchain) {
    return (swapchain & 0xFFFF000000000000ULL) == 0xDEAD000000000000ULL;
}

// Image create info for creating real swapchain images
#define VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO 14
#define VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO 5
#define VK_IMAGE_TYPE_2D 1
#define VK_SAMPLE_COUNT_1_BIT 1
#define VK_IMAGE_TILING_OPTIMAL 0
#define VK_IMAGE_TILING_LINEAR 1
#define VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT 0x01
#define VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT 0x02
#define VK_MEMORY_PROPERTY_HOST_COHERENT_BIT 0x04

typedef struct VkImageCreateInfo {
    int sType;
    const void* pNext;
    VkFlags flags;
    int imageType;
    int format;
    struct { uint32_t width; uint32_t height; uint32_t depth; } extent;
    uint32_t mipLevels;
    uint32_t arrayLayers;
    int samples;
    int tiling;
    VkFlags usage;
    int sharingMode;
    uint32_t queueFamilyIndexCount;
    const uint32_t* pQueueFamilyIndices;
    int initialLayout;
} VkImageCreateInfo;

typedef struct VkMemoryRequirements {
    VkDeviceSize size;
    VkDeviceSize alignment;
    uint32_t memoryTypeBits;
} VkMemoryRequirements;

typedef struct VkMemoryAllocateInfo {
    int sType;
    const void* pNext;
    VkDeviceSize allocationSize;
    uint32_t memoryTypeIndex;
} VkMemoryAllocateInfo;

typedef uint64_t VkDeviceMemory;

// For querying memory properties
typedef struct VkMemoryType {
    uint32_t propertyFlags;
    uint32_t heapIndex;
} VkMemoryType;

typedef struct VkMemoryHeap {
    VkDeviceSize size;
    uint32_t flags;
} VkMemoryHeap;

typedef struct VkPhysicalDeviceMemoryProperties {
    uint32_t memoryTypeCount;
    VkMemoryType memoryTypes[32];
    uint32_t memoryHeapCount;
    VkMemoryHeap memoryHeaps[16];
} VkPhysicalDeviceMemoryProperties;

static VkPhysicalDeviceMemoryProperties g_mem_properties = {0};
static int g_mem_properties_queried = 0;

// Create swapchain for fake surface
VkResult vkCreateSwapchainKHR(
    VkDevice device,
    const VkSwapchainCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSwapchainKHR* pSwapchain)
{
    (void)pAllocator;

    fprintf(stderr, "[XCB-Bridge] *** vkCreateSwapchainKHR ENTERED ***\n");

    // Check if this is for one of our fake surfaces
    SurfaceEntry* surf = find_surface(pCreateInfo->surface);
    if (!surf) {
        // Forward to real implementation
        if (real_vkGetInstanceProcAddr && g_current_instance) {
            typedef VkResult (*PFN)(VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*);
            PFN fn = (PFN)real_vkGetInstanceProcAddr(g_current_instance, "vkCreateSwapchainKHR");
            if (fn) return fn(device, pCreateInfo, pAllocator, pSwapchain);
        }
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    fprintf(stderr, "[XCB-Bridge] vkCreateSwapchainKHR: %ux%u, %u images, format=%d\n",
            pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height,
            pCreateInfo->minImageCount, pCreateInfo->imageFormat);

    // Create swapchain entry
    SwapchainEntry* entry = malloc(sizeof(SwapchainEntry));
    if (!entry) return VK_ERROR_OUT_OF_HOST_MEMORY;

    entry->handle = g_next_swapchain++;
    entry->surface = pCreateInfo->surface;
    entry->device = device;
    entry->width = pCreateInfo->imageExtent.width;
    entry->height = pCreateInfo->imageExtent.height;
    entry->format = pCreateInfo->imageFormat;
    entry->current_image = 0;

    entry->image_count = pCreateInfo->minImageCount;
    if (entry->image_count > MAX_SWAPCHAIN_IMAGES) {
        entry->image_count = MAX_SWAPCHAIN_IMAGES;
    }

    // Get Vulkan function pointers
    typedef VkResult (*PFN_vkCreateImage)(VkDevice, const VkImageCreateInfo*, const VkAllocationCallbacks*, VkImage*);
    typedef void (*PFN_vkGetImageMemoryRequirements)(VkDevice, VkImage, VkMemoryRequirements*);
    typedef VkResult (*PFN_vkAllocateMemory)(VkDevice, const VkMemoryAllocateInfo*, const VkAllocationCallbacks*, VkDeviceMemory*);
    typedef VkResult (*PFN_vkBindImageMemory)(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize);

    PFN_vkCreateImage fn_createImage = NULL;
    PFN_vkGetImageMemoryRequirements fn_getMemReq = NULL;
    PFN_vkAllocateMemory fn_allocMem = NULL;
    PFN_vkBindImageMemory fn_bindMem = NULL;

    if (real_vkGetInstanceProcAddr && g_current_instance) {
        fn_createImage = (PFN_vkCreateImage)real_vkGetInstanceProcAddr(g_current_instance, "vkCreateImage");
        fn_getMemReq = (PFN_vkGetImageMemoryRequirements)real_vkGetInstanceProcAddr(g_current_instance, "vkGetImageMemoryRequirements");
        fn_allocMem = (PFN_vkAllocateMemory)real_vkGetInstanceProcAddr(g_current_instance, "vkAllocateMemory");
        fn_bindMem = (PFN_vkBindImageMemory)real_vkGetInstanceProcAddr(g_current_instance, "vkBindImageMemory");
    }

    // Create REAL images with memory backing
    for (uint32_t i = 0; i < entry->image_count; i++) {
        entry->memory[i] = 0;

        if (fn_createImage && fn_getMemReq && fn_allocMem && fn_bindMem) {
            VkImageCreateInfo imageInfo = {0};
            imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = pCreateInfo->imageFormat;
            imageInfo.extent.width = pCreateInfo->imageExtent.width;
            imageInfo.extent.height = pCreateInfo->imageExtent.height;
            imageInfo.extent.depth = 1;
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = pCreateInfo->imageArrayLayers;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.tiling = VK_IMAGE_TILING_LINEAR;  // LINEAR for CPU readback
            imageInfo.usage = pCreateInfo->imageUsage;
            imageInfo.sharingMode = pCreateInfo->imageSharingMode;
            imageInfo.initialLayout = 0; // VK_IMAGE_LAYOUT_UNDEFINED

            VkResult res = fn_createImage(device, &imageInfo, NULL, &entry->images[i]);
            if (res != VK_SUCCESS) {
                fprintf(stderr, "[XCB-Bridge] vkCreateImage[%u] failed: %d\n", i, res);
                entry->images[i] = g_next_image++;
                continue;
            }

            // Get memory requirements
            VkMemoryRequirements memReq = {0};
            fn_getMemReq(device, entry->images[i], &memReq);

            // Query memory properties if not done yet
            if (!g_mem_properties_queried && g_physical_device && real_vkGetInstanceProcAddr && g_current_instance) {
                typedef void (*PFN_vkGetPhysicalDeviceMemoryProperties)(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties*);
                PFN_vkGetPhysicalDeviceMemoryProperties fn_getMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)
                    real_vkGetInstanceProcAddr(g_current_instance, "vkGetPhysicalDeviceMemoryProperties");
                if (fn_getMemProps) {
                    fn_getMemProps(g_physical_device, &g_mem_properties);
                    g_mem_properties_queried = 1;
                    fprintf(stderr, "[XCB-Bridge] Queried memory properties: %u types\n", g_mem_properties.memoryTypeCount);
                    for (uint32_t k = 0; k < g_mem_properties.memoryTypeCount; k++) {
                        fprintf(stderr, "[XCB-Bridge]   Type %u: flags=0x%x\n", k, g_mem_properties.memoryTypes[k].propertyFlags);
                    }
                }
            }

            // Allocate memory - find HOST_VISIBLE | HOST_COHERENT type
            VkMemoryAllocateInfo allocInfo = {0};
            allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocInfo.allocationSize = memReq.size;

            // Find a HOST_VISIBLE memory type
            uint32_t hostVisibleType = UINT32_MAX;
            uint32_t requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

            if (g_mem_properties_queried) {
                for (uint32_t j = 0; j < g_mem_properties.memoryTypeCount; j++) {
                    if ((memReq.memoryTypeBits & (1 << j)) &&
                        (g_mem_properties.memoryTypes[j].propertyFlags & requiredFlags) == requiredFlags) {
                        hostVisibleType = j;
                        fprintf(stderr, "[XCB-Bridge] Found HOST_VISIBLE memory type: %u\n", j);
                        break;
                    }
                }
            }

            if (hostVisibleType != UINT32_MAX) {
                allocInfo.memoryTypeIndex = hostVisibleType;
            } else {
                // Fallback: try the first bit set in memoryTypeBits
                fprintf(stderr, "[XCB-Bridge] WARNING: No HOST_VISIBLE memory type found, using fallback\n");
                for (uint32_t j = 0; j < 32; j++) {
                    if (memReq.memoryTypeBits & (1 << j)) {
                        allocInfo.memoryTypeIndex = j;
                        break;
                    }
                }
            }

            res = fn_allocMem(device, &allocInfo, NULL, &entry->memory[i]);
            if (res != VK_SUCCESS) {
                fprintf(stderr, "[XCB-Bridge] vkAllocateMemory[%u] failed: %d\n", i, res);
                continue;
            }

            // Bind memory to image
            res = fn_bindMem(device, entry->images[i], entry->memory[i], 0);
            if (res != VK_SUCCESS) {
                fprintf(stderr, "[XCB-Bridge] vkBindImageMemory[%u] failed: %d\n", i, res);
                continue;
            }

            // Query image layout for row pitch (LINEAR tiling)
            typedef struct VkImageSubresource {
                uint32_t aspectMask;
                uint32_t mipLevel;
                uint32_t arrayLayer;
            } VkImageSubresource;
            typedef struct VkSubresourceLayout {
                VkDeviceSize offset;
                VkDeviceSize size;
                VkDeviceSize rowPitch;
                VkDeviceSize arrayPitch;
                VkDeviceSize depthPitch;
            } VkSubresourceLayout;
            typedef void (*PFN_vkGetImageSubresourceLayout)(VkDevice, VkImage, const VkImageSubresource*, VkSubresourceLayout*);

            PFN_vkGetImageSubresourceLayout fn_getLayout = NULL;
            if (real_vkGetInstanceProcAddr && g_current_instance) {
                fn_getLayout = (PFN_vkGetImageSubresourceLayout)real_vkGetInstanceProcAddr(g_current_instance, "vkGetImageSubresourceLayout");
            }
            if (fn_getLayout) {
                VkImageSubresource subres = {0};
                subres.aspectMask = 1;  // VK_IMAGE_ASPECT_COLOR_BIT
                subres.mipLevel = 0;
                subres.arrayLayer = 0;
                VkSubresourceLayout layout = {0};
                fn_getLayout(device, entry->images[i], &subres, &layout);
                entry->row_pitch[i] = layout.rowPitch;
                fprintf(stderr, "[XCB-Bridge] Image[%u] rowPitch: %lu (expected: %u)\n",
                        i, (unsigned long)layout.rowPitch, entry->width * 4);
            } else {
                entry->row_pitch[i] = entry->width * 4;  // Fallback
            }

            fprintf(stderr, "[XCB-Bridge] Created real image[%u]: 0x%lx (mem: 0x%lx, size: %lu)\n",
                    i, (unsigned long)entry->images[i], (unsigned long)entry->memory[i],
                    (unsigned long)memReq.size);
        } else {
            // Fallback to fake handles
            entry->images[i] = g_next_image++;
            fprintf(stderr, "[XCB-Bridge] Using fake image[%u]: 0x%lx\n", i, (unsigned long)entry->images[i]);
        }
    }

    // Add to list
    pthread_mutex_lock(&g_mutex);
    entry->next = g_swapchains;
    g_swapchains = entry;
    pthread_mutex_unlock(&g_mutex);

    g_current_device = device;
    *pSwapchain = entry->handle;

    fprintf(stderr, "[XCB-Bridge] Created swapchain: 0x%lx with %u images\n",
            (unsigned long)entry->handle, entry->image_count);

    return VK_SUCCESS;
}

// Destroy swapchain
void vkDestroySwapchainKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const VkAllocationCallbacks* pAllocator)
{
    (void)pAllocator;

    if (!is_our_swapchain(swapchain)) {
        // Forward to real implementation
        if (real_vkGetInstanceProcAddr && g_current_instance) {
            typedef void (*PFN)(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*);
            PFN fn = (PFN)real_vkGetInstanceProcAddr(g_current_instance, "vkDestroySwapchainKHR");
            if (fn) fn(device, swapchain, pAllocator);
        }
        return;
    }

    fprintf(stderr, "[XCB-Bridge] vkDestroySwapchainKHR: 0x%lx (device=0x%lx)\n",
            (unsigned long)swapchain, (unsigned long)device);
    fflush(stderr);

    // Find the swapchain first - we need it for device handle and cleanup
    fprintf(stderr, "[XCB-Bridge] Looking up swapchain in list...\n");
    fflush(stderr);

    pthread_mutex_lock(&g_mutex);
    SwapchainEntry** pp = &g_swapchains;
    SwapchainEntry* to_free = NULL;
    while (*pp) {
        if ((*pp)->handle == swapchain) {
            to_free = *pp;
            *pp = (*pp)->next;
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_mutex);

    if (!to_free) {
        fprintf(stderr, "[XCB-Bridge] vkDestroySwapchainKHR: swapchain not found in list\n");
        fflush(stderr);
        return;
    }

    fprintf(stderr, "[XCB-Bridge] Found swapchain entry, image_count=%u\n", to_free->image_count);
    fflush(stderr);

    // Get device handle - prefer the one passed in, fall back to stored one
    VkDevice dev = device ? device : to_free->device;
    fprintf(stderr, "[XCB-Bridge] Using device: 0x%lx\n", (unsigned long)dev);
    fflush(stderr);

    // Get device-level function pointers for cleanup using vkGetDeviceProcAddr
    typedef void (*PFN_vkDestroyImage)(VkDevice, VkImage, const VkAllocationCallbacks*);
    typedef void (*PFN_vkFreeMemory)(VkDevice, VkDeviceMemory, const VkAllocationCallbacks*);
    typedef VkResult (*PFN_vkDeviceWaitIdle)(VkDevice);

    PFN_vkDestroyImage fn_destroyImage = NULL;
    PFN_vkFreeMemory fn_freeMemory = NULL;
    PFN_vkDeviceWaitIdle fn_deviceWaitIdle = NULL;

    // Use vkGetDeviceProcAddr for device-level functions (more reliable)
    if (real_vkGetDeviceProcAddr && dev) {
        fprintf(stderr, "[XCB-Bridge] Getting device function pointers via vkGetDeviceProcAddr\n");
        fflush(stderr);
        fn_destroyImage = (PFN_vkDestroyImage)real_vkGetDeviceProcAddr(dev, "vkDestroyImage");
        fn_freeMemory = (PFN_vkFreeMemory)real_vkGetDeviceProcAddr(dev, "vkFreeMemory");
        fn_deviceWaitIdle = (PFN_vkDeviceWaitIdle)real_vkGetDeviceProcAddr(dev, "vkDeviceWaitIdle");
        fprintf(stderr, "[XCB-Bridge] fn_destroyImage=%p, fn_freeMemory=%p, fn_deviceWaitIdle=%p\n",
                (void*)fn_destroyImage, (void*)fn_freeMemory, (void*)fn_deviceWaitIdle);
        fflush(stderr);
    }

    // Wait for device idle before destroying
    if (fn_deviceWaitIdle) {
        fprintf(stderr, "[XCB-Bridge] Calling vkDeviceWaitIdle...\n");
        fflush(stderr);
        VkResult res = fn_deviceWaitIdle(dev);
        fprintf(stderr, "[XCB-Bridge] vkDeviceWaitIdle returned %d\n", res);
        fflush(stderr);
    }

    // Destroy images
    for (uint32_t i = 0; i < to_free->image_count; i++) {
        if (to_free->images[i] && fn_destroyImage) {
            fprintf(stderr, "[XCB-Bridge] Destroying image[%u]: 0x%lx\n", i, (unsigned long)to_free->images[i]);
            fflush(stderr);
            fn_destroyImage(dev, to_free->images[i], NULL);
        }
    }

    // Free memory
    for (uint32_t i = 0; i < to_free->image_count; i++) {
        if (to_free->memory[i] && fn_freeMemory) {
            fprintf(stderr, "[XCB-Bridge] Freeing memory[%u]: 0x%lx\n", i, (unsigned long)to_free->memory[i]);
            fflush(stderr);
            fn_freeMemory(dev, to_free->memory[i], NULL);
        }
    }

    free(to_free);
    fprintf(stderr, "[XCB-Bridge] vkDestroySwapchainKHR completed successfully\n");
    fflush(stderr);
    return;

#if 0  // Old code using vkGetInstanceProcAddr
    typedef void (*PFN_vkDestroyImage_old)(VkDevice, VkImage, const VkAllocationCallbacks*);
    if (real_vkGetInstanceProcAddr && g_current_instance) {

            // Wait for device to be idle before destroying resources
            if (dev && fn_deviceWaitIdle) {
                fprintf(stderr, "[XCB-Bridge] Waiting for device idle...\n");
                fflush(stderr);
                fn_deviceWaitIdle(dev);
                fprintf(stderr, "[XCB-Bridge] Device idle, destroying resources\n");
                fflush(stderr);
            }

            // First pass: destroy images (must be done before freeing memory)
            for (uint32_t i = 0; i < to_free->image_count; i++) {
                // Only destroy real handles (not fake ones starting with 0xFACE)
                VkImage img = to_free->images[i];
                if (img && (img & 0xFFFF000000000000ULL) != 0xFACE000000000000ULL) {
                    if (fn_destroyImage && dev) {
                        fprintf(stderr, "[XCB-Bridge] Destroying image[%u]: 0x%lx\n", i, (unsigned long)img);
                        fflush(stderr);
                        fn_destroyImage(dev, img, NULL);
                    }
                }
                to_free->images[i] = 0;  // Mark as destroyed
            }

            // Second pass: free memory (after images are destroyed)
            for (uint32_t i = 0; i < to_free->image_count; i++) {
                VkDeviceMemory mem = to_free->memory[i];
                if (mem && fn_freeMemory && dev) {
                    fprintf(stderr, "[XCB-Bridge] Freeing memory[%u]: 0x%lx\n", i, (unsigned long)mem);
                    fflush(stderr);
                    fn_freeMemory(dev, mem, NULL);
                }
                to_free->memory[i] = 0;  // Mark as freed
            }

            free(to_free);
            fprintf(stderr, "[XCB-Bridge] vkDestroySwapchainKHR completed successfully\n");
            fflush(stderr);
            return;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_mutex);
    fprintf(stderr, "[XCB-Bridge] vkDestroySwapchainKHR: swapchain not found in list\n");
    fflush(stderr);
#endif  // Disabled cleanup code
}

// Get swapchain images
VkResult vkGetSwapchainImagesKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint32_t* pSwapchainImageCount,
    VkImage* pSwapchainImages)
{
    (void)device;

    SwapchainEntry* entry = find_swapchain(swapchain);
    if (!entry) {
        // Forward to real implementation
        if (real_vkGetInstanceProcAddr && g_current_instance) {
            typedef VkResult (*PFN)(VkDevice, VkSwapchainKHR, uint32_t*, VkImage*);
            PFN fn = (PFN)real_vkGetInstanceProcAddr(g_current_instance, "vkGetSwapchainImagesKHR");
            if (fn) return fn(device, swapchain, pSwapchainImageCount, pSwapchainImages);
        }
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (!pSwapchainImages) {
        *pSwapchainImageCount = entry->image_count;
        fprintf(stderr, "[XCB-Bridge] vkGetSwapchainImagesKHR: count = %u\n", entry->image_count);
        return VK_SUCCESS;
    }

    uint32_t count = *pSwapchainImageCount < entry->image_count ? *pSwapchainImageCount : entry->image_count;
    for (uint32_t i = 0; i < count; i++) {
        pSwapchainImages[i] = entry->images[i];
    }
    *pSwapchainImageCount = count;

    fprintf(stderr, "[XCB-Bridge] vkGetSwapchainImagesKHR: returning %u images\n", count);
    return count < entry->image_count ? VK_INCOMPLETE : VK_SUCCESS;
}

// Helper to check if an image belongs to one of our swapchains
static int is_swapchain_image(VkImage image, SwapchainEntry** out_entry, uint32_t* out_index) {
    pthread_mutex_lock(&g_mutex);
    SwapchainEntry* s = g_swapchains;
    while (s) {
        for (uint32_t i = 0; i < s->image_count; i++) {
            if (s->images[i] == image) {
                if (out_entry) *out_entry = s;
                if (out_index) *out_index = i;
                pthread_mutex_unlock(&g_mutex);
                return 1;
            }
        }
        s = s->next;
    }
    pthread_mutex_unlock(&g_mutex);
    return 0;
}

// Hook vkDestroyImage - prevent double-free of swapchain images
void vkDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks* pAllocator) {
    SwapchainEntry* entry = NULL;
    uint32_t index = 0;

    if (image && is_swapchain_image(image, &entry, &index)) {
        // This is one of our swapchain images - mark as destroyed but don't forward
        fprintf(stderr, "[XCB-Bridge] vkDestroyImage: swapchain image 0x%lx (entry %p, index %u) - marking as destroyed\n",
                (unsigned long)image, (void*)entry, index);
        fflush(stderr);

        // Mark the image as destroyed so we don't try to destroy it again
        pthread_mutex_lock(&g_mutex);
        if (entry && index < entry->image_count) {
            // Destroy the real image via driver
            if (real_vkGetInstanceProcAddr && g_current_instance) {
                typedef void (*PFN)(VkDevice, VkImage, const VkAllocationCallbacks*);
                PFN fn = (PFN)real_vkGetInstanceProcAddr(g_current_instance, "vkDestroyImage");
                if (fn && entry->images[index]) {
                    fn(device, entry->images[index], pAllocator);
                }
            }
            entry->images[index] = 0;  // Mark as destroyed
        }
        pthread_mutex_unlock(&g_mutex);
        return;
    }

    // Forward non-swapchain images to real implementation
    if (real_vkGetInstanceProcAddr && g_current_instance) {
        typedef void (*PFN)(VkDevice, VkImage, const VkAllocationCallbacks*);
        PFN fn = (PFN)real_vkGetInstanceProcAddr(g_current_instance, "vkDestroyImage");
        if (fn) fn(device, image, pAllocator);
    }
}

// Hook vkWaitForFences - vkcube may block here before AcquireNextImage
VkResult vkWaitForFences(VkDevice device, uint32_t fenceCount, const VkFence* pFences,
                          VkBool32 waitAll, uint64_t timeout) {
    static int wait_count = 0;
    if (wait_count < 20 || (wait_count % 60 == 0)) {
        fprintf(stderr, "[XCB-Bridge] vkWaitForFences (count=%u, waitAll=%u, timeout=%lu, call #%d)\n",
                fenceCount, waitAll, (unsigned long)timeout, wait_count);
        fflush(stderr);
    }
    wait_count++;

    // For our fake swapchain, immediately return success
    // The fences are never signaled but we pretend they are
    if (wait_count < 20) {
        fprintf(stderr, "[XCB-Bridge] vkWaitForFences -> returning VK_SUCCESS immediately\n");
        fflush(stderr);
    }
    return VK_SUCCESS;  // Pretend all fences are signaled
}

// Hook vkResetFences
VkResult vkResetFences(VkDevice device, uint32_t fenceCount, const VkFence* pFences) {
    static int reset_count = 0;
    if (reset_count < 10) {
        fprintf(stderr, "[XCB-Bridge] vkResetFences (count=%u)\n", fenceCount);
        fflush(stderr);
    }
    reset_count++;

    // Forward to real implementation
    typedef VkResult (*PFN)(VkDevice, uint32_t, const VkFence*);
    PFN fn = NULL;
    if (real_vkGetInstanceProcAddr && g_current_instance) {
        fn = (PFN)real_vkGetInstanceProcAddr(g_current_instance, "vkResetFences");
    }
    if (fn) return fn(device, fenceCount, pFences);
    return VK_SUCCESS;
}

// Acquire next image
VkResult vkAcquireNextImageKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint64_t timeout,
    VkSemaphore semaphore,
    VkFence fence,
    uint32_t* pImageIndex)
{
    // Log at VERY start to catch any call
    static int entry_count = 0;
    if (entry_count < 5) {
        fprintf(stderr, "[XCB-Bridge] *** vkAcquireNextImageKHR ENTERED *** (swapchain=0x%lx)\n",
                (unsigned long)swapchain);
        fflush(stderr);
    }
    entry_count++;

    (void)timeout;

    SwapchainEntry* entry = find_swapchain(swapchain);
    if (!entry) {
        // Forward to real implementation
        if (real_vkGetInstanceProcAddr && g_current_instance) {
            typedef VkResult (*PFN)(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t*);
            PFN fn = (PFN)real_vkGetInstanceProcAddr(g_current_instance, "vkAcquireNextImageKHR");
            if (fn) return fn(device, swapchain, timeout, semaphore, fence, pImageIndex);
        }
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    *pImageIndex = entry->current_image;
    entry->current_image = (entry->current_image + 1) % entry->image_count;

    static int acquire_count = 0;
    if (acquire_count < 10 || acquire_count % 60 == 0) {
        fprintf(stderr, "[XCB-Bridge] vkAcquireNextImageKHR: index=%u (call #%d)\n", *pImageIndex, acquire_count);
        fflush(stderr);
    }
    acquire_count++;

    // Signal semaphore if provided - use vkQueueSubmit with no work to signal it
    // For now, apps should work without explicit signaling since we're not doing real presentation
    // The GPU work doesn't actually depend on swapchain image acquisition timing

    // Signal fence if provided
    if (fence && real_vkGetInstanceProcAddr && g_current_instance) {
        typedef VkResult (*PFN_vkResetFences)(VkDevice, uint32_t, const VkFence*);
        typedef VkResult (*PFN_vkQueueSubmit)(VkQueue, uint32_t, const void*, VkFence);
        typedef VkResult (*PFN_vkGetDeviceQueue)(VkDevice, uint32_t, uint32_t, VkQueue*);

        // We can't easily signal a fence without a queue submit, and we need the queue
        // For simplicity, we'll rely on the app not strictly depending on fence signaling
    }

    return VK_SUCCESS;
}

// Queue present - send frame to socket for display
VkResult vkQueuePresentKHR(
    VkQueue queue,
    const VkPresentInfoKHR* pPresentInfo)
{
    (void)queue;

    static int present_count = 0;
    if (present_count < 10 || present_count % 60 == 0) {
        fprintf(stderr, "[XCB-Bridge] vkQueuePresentKHR (call #%d)\n", present_count);
        fflush(stderr);
    }
    present_count++;

    // Check if any of the swapchains are ours
    for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
        SwapchainEntry* entry = find_swapchain(pPresentInfo->pSwapchains[i]);
        if (entry) {
            // Our fake swapchain - map image and send to display
            uint32_t imageIndex = pPresentInfo->pImageIndices[i];
            if (imageIndex < entry->image_count && entry->memory[imageIndex]) {
                // Wait for GPU to finish rendering before reading the image
                typedef VkResult (*PFN_vkQueueWaitIdle)(VkQueue);
                PFN_vkQueueWaitIdle fn_wait = NULL;
                if (real_vkGetDeviceProcAddr && entry->device) {
                    fn_wait = (PFN_vkQueueWaitIdle)real_vkGetDeviceProcAddr(entry->device, "vkQueueWaitIdle");
                }
                if (fn_wait && queue) {
                    fn_wait(queue);
                }

                // Get vkMapMemory and vkUnmapMemory
                typedef VkResult (*PFN_vkMapMemory)(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkFlags, void**);
                typedef void (*PFN_vkUnmapMemory)(VkDevice, VkDeviceMemory);

                PFN_vkMapMemory fn_map = NULL;
                PFN_vkUnmapMemory fn_unmap = NULL;

                if (real_vkGetDeviceProcAddr && entry->device) {
                    fn_map = (PFN_vkMapMemory)real_vkGetDeviceProcAddr(entry->device, "vkMapMemory");
                    fn_unmap = (PFN_vkUnmapMemory)real_vkGetDeviceProcAddr(entry->device, "vkUnmapMemory");
                }

                if (fn_map && fn_unmap) {
                    void* mapped = NULL;
                    // Map entire image memory (use row_pitch * height for size)
                    size_t map_size = entry->row_pitch[imageIndex] * entry->height;
                    if (map_size == 0) map_size = entry->width * entry->height * 4;
                    VkResult res = fn_map(entry->device, entry->memory[imageIndex], 0,
                                          map_size, 0, &mapped);
                    if (res == VK_SUCCESS && mapped) {
                        // Send frame to display socket with row pitch
                        size_t pitch = entry->row_pitch[imageIndex];
                        if (pitch == 0) pitch = entry->width * 4;
                        send_frame_pitched(entry->width, entry->height, mapped, pitch);
                        fn_unmap(entry->device, entry->memory[imageIndex]);
                    } else if (present_count < 5) {
                        fprintf(stderr, "[XCB-Bridge] vkMapMemory failed: %d\n", res);
                    }
                }
            }

            if (pPresentInfo->pResults) {
                pPresentInfo->pResults[i] = VK_SUCCESS;
            }
        } else {
            // Forward to real implementation
            if (real_vkGetInstanceProcAddr && g_current_instance) {
                typedef VkResult (*PFN)(VkQueue, const VkPresentInfoKHR*);
                PFN fn = (PFN)real_vkGetInstanceProcAddr(g_current_instance, "vkQueuePresentKHR");
                if (fn) return fn(queue, pPresentInfo);
            }
        }
    }

    return VK_SUCCESS;
}

// ============================================================================
// vkCreateImageView logging - to see if vkcube proceeds after swapchain
// ============================================================================

typedef uint64_t VkImageView;
typedef struct VkImageViewCreateInfo {
    int sType;
    const void* pNext;
    VkFlags flags;
    VkImage image;
    int viewType;
    int format;
    // ... more fields we don't need
} VkImageViewCreateInfo;

static VkResult (*real_vkCreateImageView)(VkDevice, const VkImageViewCreateInfo*, const VkAllocationCallbacks*, VkImageView*) = NULL;

// Global symbol to intercept direct vkCreateImageView calls
VkResult vkCreateImageView(VkDevice device, const VkImageViewCreateInfo* pCreateInfo,
                            const VkAllocationCallbacks* pAllocator, VkImageView* pView) {
    static int create_view_count = 0;
    if (create_view_count < 10) {
        fprintf(stderr, "[XCB-Bridge] vkCreateImageView (call #%d, image=0x%lx, format=%d)\n",
                create_view_count, (unsigned long)pCreateInfo->image, pCreateInfo->format);
        fflush(stderr);
    }
    create_view_count++;

    // Forward to real implementation
    if (!real_vkCreateImageView) {
        real_vkCreateImageView = dlsym(RTLD_NEXT, "vkCreateImageView");
    }
    if (!real_vkCreateImageView && real_vkGetInstanceProcAddr && g_current_instance) {
        real_vkCreateImageView = (typeof(real_vkCreateImageView))
            real_vkGetInstanceProcAddr(g_current_instance, "vkCreateImageView");
    }
    if (real_vkCreateImageView) {
        VkResult result = real_vkCreateImageView(device, pCreateInfo, pAllocator, pView);
        if (create_view_count <= 10) {
            fprintf(stderr, "[XCB-Bridge] vkCreateImageView -> result=%d, view=0x%lx\n",
                    result, (unsigned long)(pView ? *pView : 0));
            fflush(stderr);
        }
        return result;
    }
    fprintf(stderr, "[XCB-Bridge] vkCreateImageView -> FAILED (no real function)\n");
    fflush(stderr);
    return VK_ERROR_INITIALIZATION_FAILED;
}

// Alias for vkGetDeviceProcAddr
VkResult hooked_vkCreateImageView(VkDevice device, const VkImageViewCreateInfo* pCreateInfo,
                                   const VkAllocationCallbacks* pAllocator, VkImageView* pView) {
    return vkCreateImageView(device, pCreateInfo, pAllocator, pView);
}

// ============================================================================
// Instance extension enumeration
// ============================================================================

// Intercept vkEnumerateInstanceExtensionProperties
VkResult vkEnumerateInstanceExtensionProperties(
    const char* pLayerName,
    uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties)
{
    if (!real_vkEnumerateInstanceExtensionProperties) {
        real_vkEnumerateInstanceExtensionProperties = dlsym(RTLD_NEXT, "vkEnumerateInstanceExtensionProperties");
    }
    if (!real_vkEnumerateInstanceExtensionProperties) {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }

    uint32_t real_count = 0;
    VkResult result = real_vkEnumerateInstanceExtensionProperties(pLayerName, &real_count, NULL);
    if (result != VK_SUCCESS) return result;

    // Add VK_EXT_headless_surface and VK_KHR_xcb_surface
    uint32_t total = real_count + 2;

    if (!pProperties) {
        *pPropertyCount = total;
        fprintf(stderr, "[XCB-Bridge] Extensions: %u (real=%u + headless + xcb_surface)\n", total, real_count);
        return VK_SUCCESS;
    }

    uint32_t get_count = *pPropertyCount < real_count ? *pPropertyCount : real_count;
    result = real_vkEnumerateInstanceExtensionProperties(pLayerName, &get_count, pProperties);

    uint32_t idx = real_count;
    uint32_t added = 0;

    if (*pPropertyCount > idx) {
        strncpy(pProperties[idx].extensionName, "VK_EXT_headless_surface", VK_MAX_EXTENSION_NAME_SIZE);
        pProperties[idx].specVersion = 1;
        idx++; added++;
    }
    if (*pPropertyCount > idx) {
        strncpy(pProperties[idx].extensionName, "VK_KHR_xcb_surface", VK_MAX_EXTENSION_NAME_SIZE);
        pProperties[idx].specVersion = 6;
        idx++; added++;
    }

    *pPropertyCount = real_count + added;
    fprintf(stderr, "[XCB-Bridge] Added %u extensions (headless + xcb_surface)\n", added);
    return (added == 2) ? VK_SUCCESS : VK_INCOMPLETE;
}

// Intercept vkCreateInstance
VkResult vkCreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance)
{
    if (!real_vkCreateInstance) {
        real_vkCreateInstance = dlsym(RTLD_NEXT, "vkCreateInstance");
    }
    if (!real_vkCreateInstance) {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }

    // Filter out extensions we provide
    int has_headless = 0, has_xcb = 0;
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
        const char* ext = pCreateInfo->ppEnabledExtensionNames[i];
        if (strcmp(ext, "VK_EXT_headless_surface") == 0) has_headless = 1;
        if (strcmp(ext, "VK_KHR_xcb_surface") == 0) has_xcb = 1;
    }

    if (!has_headless && !has_xcb) {
        VkResult r = real_vkCreateInstance(pCreateInfo, pAllocator, pInstance);
        if (r == VK_SUCCESS) g_current_instance = *pInstance;
        return r;
    }

    // Filter out our extensions
    const char** filtered = malloc(pCreateInfo->enabledExtensionCount * sizeof(char*));
    uint32_t filtered_count = 0;

    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
        const char* ext = pCreateInfo->ppEnabledExtensionNames[i];
        if (strcmp(ext, "VK_EXT_headless_surface") != 0 &&
            strcmp(ext, "VK_KHR_xcb_surface") != 0) {
            filtered[filtered_count++] = ext;
        } else {
            fprintf(stderr, "[XCB-Bridge] Filtering: %s (we provide it)\n", ext);
        }
    }

    VkInstanceCreateInfo modified = *pCreateInfo;
    modified.enabledExtensionCount = filtered_count;
    modified.ppEnabledExtensionNames = filtered;

    fprintf(stderr, "[XCB-Bridge] Creating instance with %u extensions\n", filtered_count);
    VkResult result = real_vkCreateInstance(&modified, pAllocator, pInstance);
    free(filtered);

    if (result == VK_SUCCESS) {
        g_current_instance = *pInstance;
        fprintf(stderr, "[XCB-Bridge] Instance created: %p\n", *pInstance);
    }
    return result;
}

// Intercept vkGetInstanceProcAddr
PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char* pName)
{
    if (!real_vkGetInstanceProcAddr) {
        real_vkGetInstanceProcAddr = dlsym(RTLD_NEXT, "vkGetInstanceProcAddr");
    }

    // Debug: log all function requests
    if (strncmp(pName, "vkCreate", 8) == 0 || strncmp(pName, "vkGetPhysicalDevice", 19) == 0 ||
        strncmp(pName, "vkGet", 5) == 0 || strncmp(pName, "vkAcquire", 9) == 0 ||
        strncmp(pName, "vkQueue", 7) == 0 || strncmp(pName, "vkDestroy", 9) == 0) {
        fprintf(stderr, "[XCB-Bridge] vkGetInstanceProcAddr('%s')\n", pName);
    }

    // Our implementations
    if (strcmp(pName, "vkEnumerateInstanceExtensionProperties") == 0)
        return (PFN_vkVoidFunction)vkEnumerateInstanceExtensionProperties;
    if (strcmp(pName, "vkCreateInstance") == 0)
        return (PFN_vkVoidFunction)vkCreateInstance;
    if (strcmp(pName, "vkCreateHeadlessSurfaceEXT") == 0)
        return (PFN_vkVoidFunction)my_vkCreateHeadlessSurfaceEXT;
    if (strcmp(pName, "vkCreateXcbSurfaceKHR") == 0) {
        fprintf(stderr, "[XCB-Bridge] -> returning vkCreateXcbSurfaceKHR\n");
        return (PFN_vkVoidFunction)vkCreateXcbSurfaceKHR;
    }
    if (strcmp(pName, "vkGetPhysicalDeviceXcbPresentationSupportKHR") == 0)
        return (PFN_vkVoidFunction)my_vkGetPhysicalDeviceXcbPresentationSupportKHR;
    if (strcmp(pName, "vkDestroySurfaceKHR") == 0)
        return (PFN_vkVoidFunction)my_vkDestroySurfaceKHR;

    // For headless surfaces only (bridged surfaces use real implementation)
    if (strcmp(pName, "vkGetPhysicalDeviceSurfaceSupportKHR") == 0)
        return (PFN_vkVoidFunction)my_vkGetPhysicalDeviceSurfaceSupportKHR;
    if (strcmp(pName, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR") == 0)
        return (PFN_vkVoidFunction)my_vkGetPhysicalDeviceSurfaceCapabilitiesKHR;
    if (strcmp(pName, "vkGetPhysicalDeviceSurfaceFormatsKHR") == 0)
        return (PFN_vkVoidFunction)my_vkGetPhysicalDeviceSurfaceFormatsKHR;
    if (strcmp(pName, "vkGetPhysicalDeviceSurfacePresentModesKHR") == 0)
        return (PFN_vkVoidFunction)my_vkGetPhysicalDeviceSurfacePresentModesKHR;

    // Return our vkGetDeviceProcAddr so we can intercept device-level functions
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0) {
        fprintf(stderr, "[XCB-Bridge] -> returning our vkGetDeviceProcAddr\n");
        return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    }

    // Swapchain functions (device-level but can be queried via vkGetInstanceProcAddr)
    if (strcmp(pName, "vkCreateSwapchainKHR") == 0) {
        fprintf(stderr, "[XCB-Bridge] -> returning vkCreateSwapchainKHR\n");
        return (PFN_vkVoidFunction)vkCreateSwapchainKHR;
    }
    if (strcmp(pName, "vkDestroySwapchainKHR") == 0)
        return (PFN_vkVoidFunction)vkDestroySwapchainKHR;
    if (strcmp(pName, "vkGetSwapchainImagesKHR") == 0)
        return (PFN_vkVoidFunction)vkGetSwapchainImagesKHR;
    if (strcmp(pName, "vkAcquireNextImageKHR") == 0)
        return (PFN_vkVoidFunction)vkAcquireNextImageKHR;
    if (strcmp(pName, "vkQueuePresentKHR") == 0)
        return (PFN_vkVoidFunction)vkQueuePresentKHR;

    if (real_vkGetInstanceProcAddr)
        return real_vkGetInstanceProcAddr(instance, pName);
    return NULL;

}

// Also intercept vkGetDeviceProcAddr for device-level functions
PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice device, const char* pName)
{
    if (!real_vkGetDeviceProcAddr) {
        real_vkGetDeviceProcAddr = dlsym(RTLD_NEXT, "vkGetDeviceProcAddr");
    }

    // Log all function requests
    fprintf(stderr, "[XCB-Bridge] vkGetDeviceProcAddr('%s')\n", pName);
    fflush(stderr);

    // Swapchain functions
    if (strcmp(pName, "vkCreateSwapchainKHR") == 0) {
        fprintf(stderr, "[XCB-Bridge] -> returning our vkCreateSwapchainKHR\n");
        return (PFN_vkVoidFunction)vkCreateSwapchainKHR;
    }
    if (strcmp(pName, "vkDestroySwapchainKHR") == 0) {
        fprintf(stderr, "[XCB-Bridge] -> returning our vkDestroySwapchainKHR\n");
        return (PFN_vkVoidFunction)vkDestroySwapchainKHR;
    }
    if (strcmp(pName, "vkGetSwapchainImagesKHR") == 0) {
        fprintf(stderr, "[XCB-Bridge] -> returning our vkGetSwapchainImagesKHR\n");
        return (PFN_vkVoidFunction)vkGetSwapchainImagesKHR;
    }
    if (strcmp(pName, "vkAcquireNextImageKHR") == 0) {
        fprintf(stderr, "[XCB-Bridge] -> returning our vkAcquireNextImageKHR\n");
        return (PFN_vkVoidFunction)vkAcquireNextImageKHR;
    }
    if (strcmp(pName, "vkQueuePresentKHR") == 0) {
        fprintf(stderr, "[XCB-Bridge] -> returning our vkQueuePresentKHR\n");
        return (PFN_vkVoidFunction)vkQueuePresentKHR;
    }
    if (strcmp(pName, "vkCreateImageView") == 0) {
        fprintf(stderr, "[XCB-Bridge] -> returning our hooked_vkCreateImageView\n");
        return (PFN_vkVoidFunction)hooked_vkCreateImageView;
    }

    // Log other device proc lookups to see what vkcube is doing
    static int other_lookup_count = 0;
    if (other_lookup_count < 50) {
        fprintf(stderr, "[XCB-Bridge] vkGetDeviceProcAddr('%s') -> forwarding\n", pName);
        fflush(stderr);
    }
    other_lookup_count++;

    if (real_vkGetDeviceProcAddr)
        return real_vkGetDeviceProcAddr(device, pName);
    return NULL;
}

// ============================================================================
// XCB Stubs - Fake XCB for window creation (vkcube uses XCB for window mgmt)
// ============================================================================

#include <sys/eventfd.h>
#include <fcntl.h>

typedef struct xcb_connection_t { int fd; int has_error; } xcb_connection_t;
typedef struct { uint32_t root; uint16_t width_in_pixels; uint16_t height_in_pixels; uint8_t root_depth; } xcb_screen_t;
typedef struct { xcb_screen_t *data; int rem; } xcb_screen_iterator_t;
typedef struct { uint8_t status; } xcb_setup_t;
typedef struct { unsigned int sequence; } xcb_void_cookie_t;
typedef struct { uint32_t atom; } xcb_intern_atom_reply_t;
typedef struct { unsigned int sequence; } xcb_intern_atom_cookie_t;

// Event fd that we can signal to wake up select/poll
static int g_event_fd = -1;
static int g_window_mapped = 0;
static uint32_t g_window_id = 0;
static uint16_t g_window_width = 500;
static uint16_t g_window_height = 500;

static xcb_connection_t fake_conn = { .fd = -1, .has_error = 0 };
static xcb_screen_t fake_screen = { .root = 0x123, .width_in_pixels = 1920, .height_in_pixels = 1080, .root_depth = 24 };
static xcb_setup_t fake_setup = { .status = 1 };

// Signal the event fd to wake up any waiters
static void signal_event_fd(void) {
    if (g_event_fd >= 0) {
        uint64_t val = 1;
        write(g_event_fd, &val, sizeof(val));
    }
}

xcb_connection_t* xcb_connect(const char *name, int *screenp) {
    (void)name;
    fprintf(stderr, "[XCB-Bridge] xcb_connect('%s') -> fake\n", name ? name : ":0");
    if (screenp) *screenp = 0;

    // Create eventfd for waking up select/poll
    if (g_event_fd < 0) {
        g_event_fd = eventfd(0, EFD_NONBLOCK);
        if (g_event_fd < 0) {
            fprintf(stderr, "[XCB-Bridge] WARNING: eventfd() failed, using dummy fd\n");
            g_event_fd = 3;
        } else {
            fprintf(stderr, "[XCB-Bridge] Created eventfd: %d\n", g_event_fd);
        }
    }
    fake_conn.fd = g_event_fd;

    return &fake_conn;
}
void xcb_disconnect(xcb_connection_t *c) {
    (void)c;
    fprintf(stderr, "[XCB-Bridge] xcb_disconnect()\n");
    fflush(stderr);
}
int xcb_connection_has_error(xcb_connection_t *c) {
    (void)c;
    static int check_count = 0;
    if (check_count < 5) {
        fprintf(stderr, "[XCB-Bridge] xcb_connection_has_error() -> 0 (no error)\n");
        fflush(stderr);
    }
    check_count++;
    return 0;  // No error
}
const xcb_setup_t* xcb_get_setup(xcb_connection_t *c) { (void)c; return &fake_setup; }
xcb_screen_iterator_t xcb_setup_roots_iterator(const xcb_setup_t *R) {
    (void)R;
    return (xcb_screen_iterator_t){ .data = &fake_screen, .rem = 1 };
}
void xcb_screen_next(xcb_screen_iterator_t *i) { if (i) i->rem--; }
uint32_t xcb_generate_id(xcb_connection_t *c) { static uint32_t id = 0x1000; (void)c; return id++; }
xcb_void_cookie_t xcb_create_window(xcb_connection_t *c, uint8_t d, uint32_t w, uint32_t p,
    int16_t x, int16_t y, uint16_t wi, uint16_t h, uint16_t b, uint16_t cl, uint32_t v,
    uint32_t m, const void *l) {
    (void)c;(void)d;(void)p;(void)x;(void)y;(void)b;(void)cl;(void)v;(void)m;(void)l;
    g_window_id = w;
    g_window_width = wi;
    g_window_height = h;
    fprintf(stderr, "[XCB-Bridge] xcb_create_window: id=0x%x, size=%ux%u\n", w, wi, h);
    return (xcb_void_cookie_t){1};
}
xcb_void_cookie_t xcb_map_window(xcb_connection_t *c, uint32_t w) {
    (void)c;
    fprintf(stderr, "[XCB-Bridge] xcb_map_window(0x%x) - will send MapNotify\n", w);
    g_window_mapped = 1;
    signal_event_fd();  // Wake up any waiting select/poll
    return (xcb_void_cookie_t){2};
}
xcb_void_cookie_t xcb_destroy_window(xcb_connection_t *c, uint32_t w) { (void)c;(void)w; return (xcb_void_cookie_t){3}; }
int xcb_flush(xcb_connection_t *c) {
    (void)c;
    static int flush_count = 0;
    if (flush_count < 5) {
        fprintf(stderr, "[XCB-Bridge] xcb_flush (call #%d)\n", flush_count);
        fflush(stderr);
    }
    flush_count++;
    signal_event_fd();  // Wake up any waiting select/poll
    return 1;
}

// XCB event types
#define XCB_EXPOSE 12
#define XCB_CONFIGURE_NOTIFY 22
#define XCB_MAP_NOTIFY 19
#define XCB_KEY_PRESS 2

typedef struct xcb_generic_event_t {
    uint8_t response_type;
    uint8_t pad0;
    uint16_t sequence;
    uint32_t pad[7];
    uint32_t full_sequence;
} xcb_generic_event_t;

typedef struct xcb_expose_event_t {
    uint8_t response_type;
    uint8_t pad0;
    uint16_t sequence;
    uint32_t window;
    uint16_t x, y, width, height;
    uint16_t count;
    uint8_t pad1[2];
} xcb_expose_event_t;

typedef struct xcb_map_notify_event_t {
    uint8_t response_type;
    uint8_t pad0;
    uint16_t sequence;
    uint32_t event;
    uint32_t window;
    uint8_t override_redirect;
    uint8_t pad1[3];
} xcb_map_notify_event_t;

typedef struct xcb_configure_notify_event_t {
    uint8_t response_type;
    uint8_t pad0;
    uint16_t sequence;
    uint32_t event;
    uint32_t window;
    uint32_t above_sibling;
    int16_t x, y;
    uint16_t width, height;
    uint16_t border_width;
    uint8_t override_redirect;
    uint8_t pad1;
} xcb_configure_notify_event_t;

// Event queue state
static int g_event_state = 0;  // 0=MapNotify, 1=ConfigureNotify, 2=Expose, 3+=done

void* xcb_poll_for_event(xcb_connection_t *c) {
    (void)c;
    static int poll_count = 0;

    // Log more aggressively to debug the render loop issue
    if (poll_count < 50 || (poll_count % 60 == 0)) {
        fprintf(stderr, "[XCB-Bridge] xcb_poll_for_event (call #%d, state=%d, mapped=%d)\n",
                poll_count, g_event_state, g_window_mapped);
        fflush(stderr);
    }
    poll_count++;

    // Drain eventfd to prevent spurious wakeups
    if (g_event_fd >= 0) {
        uint64_t val;
        read(g_event_fd, &val, sizeof(val));
    }

    // Only send events after window is mapped
    if (!g_window_mapped) {
        return NULL;
    }

    // Send events in sequence: MapNotify -> ConfigureNotify -> Expose
    if (g_event_state == 0) {
        g_event_state = 1;
        xcb_map_notify_event_t* event = malloc(sizeof(xcb_map_notify_event_t));
        if (event) {
            memset(event, 0, sizeof(*event));
            event->response_type = XCB_MAP_NOTIFY;
            event->event = g_window_id ? g_window_id : 0x1000;
            event->window = g_window_id ? g_window_id : 0x1000;
            event->override_redirect = 0;
            fprintf(stderr, "[XCB-Bridge] Sending MAP_NOTIFY event for window 0x%x\n", event->window);
            fflush(stderr);
            signal_event_fd();  // Signal more events coming
            return event;
        }
    }

    if (g_event_state == 1) {
        g_event_state = 2;
        xcb_configure_notify_event_t* event = malloc(sizeof(xcb_configure_notify_event_t));
        if (event) {
            memset(event, 0, sizeof(*event));
            event->response_type = XCB_CONFIGURE_NOTIFY;
            event->event = g_window_id ? g_window_id : 0x1000;
            event->window = g_window_id ? g_window_id : 0x1000;
            event->x = 0;
            event->y = 0;
            event->width = g_window_width;
            event->height = g_window_height;
            event->border_width = 0;
            event->override_redirect = 0;
            fprintf(stderr, "[XCB-Bridge] Sending CONFIGURE_NOTIFY event: %ux%u\n",
                    event->width, event->height);
            fflush(stderr);
            signal_event_fd();  // Signal more events coming
            return event;
        }
    }

    if (g_event_state == 2) {
        g_event_state = 3;
        xcb_expose_event_t* event = malloc(sizeof(xcb_expose_event_t));
        if (event) {
            memset(event, 0, sizeof(*event));
            event->response_type = XCB_EXPOSE;
            event->window = g_window_id ? g_window_id : 0x1000;
            event->x = 0;
            event->y = 0;
            event->width = g_window_width;
            event->height = g_window_height;
            event->count = 0;
            fprintf(stderr, "[XCB-Bridge] Sending EXPOSE event: %ux%u\n",
                    event->width, event->height);
            fflush(stderr);
            signal_event_fd();  // Keep eventfd ready for render loop
            return event;
        }
    }

    // After initial events are sent (state >= 3), alternate between
    // returning NULL (to let vkcube exit event loop and draw) and
    // returning EXPOSE (to trigger next redraw)
    if (g_event_state >= 3) {
        static int cycle_count = 0;
        static int phase = 0;  // 0 = return NULL, 1 = return EXPOSE

        if (phase == 0) {
            // Return NULL to let vkcube exit event loop and call draw()
            phase = 1;
            if (cycle_count < 10 || (cycle_count % 60 == 0)) {
                fprintf(stderr, "[XCB-Bridge] Returning NULL (cycle #%d) - vkcube should draw now\n", cycle_count);
                fflush(stderr);
            }
            cycle_count++;
            return NULL;
        } else {
            // Return EXPOSE to trigger next redraw
            phase = 0;
            xcb_expose_event_t* event = malloc(sizeof(xcb_expose_event_t));
            if (event) {
                memset(event, 0, sizeof(*event));
                event->response_type = XCB_EXPOSE;
                event->window = g_window_id ? g_window_id : 0x1000;
                event->x = 0;
                event->y = 0;
                event->width = g_window_width;
                event->height = g_window_height;
                event->count = 0;
                return event;
            }
        }
    }

    return NULL;
}

void* xcb_wait_for_event(xcb_connection_t *c) {
    (void)c;
    static int wait_count = 0;

    // ALWAYS log this to catch any call
    fprintf(stderr, "[XCB-Bridge] *** xcb_wait_for_event CALLED *** (call #%d, state=%d)\n", wait_count, g_event_state);
    fflush(stderr);
    wait_count++;

    // First try poll_for_event in case we have queued events
    void* event = xcb_poll_for_event(c);
    if (event) return event;

    // After initial events (state >= 3), return an EXPOSE event to trigger drawing
    if (g_event_state >= 3) {
        xcb_expose_event_t* expose = malloc(sizeof(xcb_expose_event_t));
        if (expose) {
            memset(expose, 0, sizeof(*expose));
            expose->response_type = XCB_EXPOSE;
            expose->window = g_window_id ? g_window_id : 0x1000;
            expose->width = g_window_width;
            expose->height = g_window_height;
            expose->count = 0;
            fprintf(stderr, "[XCB-Bridge] xcb_wait_for_event returning EXPOSE\n");
            fflush(stderr);
            return expose;
        }
    }

    // Don't block forever - return NULL after a short wait
    usleep(16000); // ~16ms = 60fps
    return NULL;
}

int xcb_get_file_descriptor(xcb_connection_t *c) {
    (void)c;
    return g_event_fd >= 0 ? g_event_fd : 3;
}
xcb_intern_atom_cookie_t xcb_intern_atom(xcb_connection_t *c, uint8_t o, uint16_t l, const char *n) {
    (void)c;(void)o;(void)l;(void)n; return (xcb_intern_atom_cookie_t){10};
}
xcb_intern_atom_reply_t* xcb_intern_atom_reply(xcb_connection_t *c, xcb_intern_atom_cookie_t ck, void **e) {
    (void)c;(void)ck;
    if(e)*e=NULL;
    // IMPORTANT: Return malloc'd memory - caller will free() this!
    xcb_intern_atom_reply_t* r = malloc(sizeof(xcb_intern_atom_reply_t));
    if (r) r->atom = 1;
    return r;
}
xcb_void_cookie_t xcb_change_property(xcb_connection_t *c, uint8_t m, uint32_t w, uint32_t p,
    uint32_t t, uint8_t f, uint32_t l, const void *d) {
    (void)c;(void)m;(void)w;(void)p;(void)t;(void)f;(void)l;(void)d; return (xcb_void_cookie_t){20};
}

// xcb_get_geometry - needed by vkcube to get window dimensions
typedef struct { unsigned int sequence; } xcb_get_geometry_cookie_t;
typedef struct {
    uint8_t response_type;
    uint8_t depth;
    uint16_t sequence;
    uint32_t length;
    uint32_t root;
    int16_t x, y;
    uint16_t width, height;
    uint16_t border_width;
    uint8_t pad[2];
} xcb_get_geometry_reply_t;

xcb_get_geometry_cookie_t xcb_get_geometry(xcb_connection_t *c, uint32_t drawable) {
    (void)c; (void)drawable;
    fprintf(stderr, "[XCB-Bridge] xcb_get_geometry(0x%x)\n", drawable);
    fflush(stderr);
    return (xcb_get_geometry_cookie_t){ .sequence = 30 };
}

xcb_get_geometry_cookie_t xcb_get_geometry_unchecked(xcb_connection_t *c, uint32_t drawable) {
    return xcb_get_geometry(c, drawable);
}

xcb_get_geometry_reply_t* xcb_get_geometry_reply(xcb_connection_t *c,
    xcb_get_geometry_cookie_t cookie, void **e) {
    (void)c; (void)cookie;
    if (e) *e = NULL;
    // IMPORTANT: Return malloc'd memory - caller will free() this!
    xcb_get_geometry_reply_t* reply = malloc(sizeof(xcb_get_geometry_reply_t));
    if (reply) {
        memset(reply, 0, sizeof(*reply));
        reply->response_type = 1;
        reply->depth = 24;
        reply->sequence = 30;
        reply->root = 0x123;
        reply->x = 0;
        reply->y = 0;
        reply->width = g_window_width;
        reply->height = g_window_height;
        reply->border_width = 0;
    }
    fprintf(stderr, "[XCB-Bridge] xcb_get_geometry_reply -> %ux%u\n",
            reply ? reply->width : 0, reply ? reply->height : 0);
    fflush(stderr);
    return reply;
}

// xcb_request_check - needed by vkcube to check for errors
void* xcb_request_check(xcb_connection_t *c, xcb_void_cookie_t cookie) {
    (void)c; (void)cookie;
    // Return NULL = no error
    return NULL;
}

// Simple atoi replacement to avoid GLIBC 2.38 dependency
static int simple_atoi(const char *s) {
    int n = 0;
    while (*s >= '0' && *s <= '9') {
        n = n * 10 + (*s - '0');
        s++;
    }
    return n;
}

// Additional XCB functions needed by libX11
int xcb_parse_display(const char *name, char **host, int *display, int *screen) {
    // Parse display string like ":0" or "localhost:0.0"
    if (host) *host = NULL;
    if (display) *display = 0;
    if (screen) *screen = 0;

    if (!name || name[0] == '\0') name = ":0";

    // Simple parser for ":N" or ":N.S" format
    const char *p = strchr(name, ':');
    if (p) {
        if (display) *display = simple_atoi(p + 1);
        const char *dot = strchr(p, '.');
        if (dot && screen) *screen = simple_atoi(dot + 1);
    }

    fprintf(stderr, "[XCB-Bridge] xcb_parse_display('%s') -> display=%d, screen=%d\n",
            name, display ? *display : 0, screen ? *screen : 0);
    return 1; // Success
}

xcb_connection_t* xcb_connect_to_display_with_auth_info(const char *display, void *auth, int *screen) {
    (void)auth;
    return xcb_connect(display, screen);
}

int xcb_get_maximum_request_length(xcb_connection_t *c) {
    (void)c;
    return 65535;
}

uint32_t xcb_get_maximum_request_length_fd(xcb_connection_t *c) {
    (void)c;
    return 65535;
}

void xcb_prefetch_maximum_request_length(xcb_connection_t *c) {
    (void)c;
}

void* xcb_wait_for_reply(xcb_connection_t *c, unsigned int request, void **e) {
    (void)c; (void)request;
    if (e) *e = NULL;
    return NULL;
}

void* xcb_wait_for_reply64(xcb_connection_t *c, uint64_t request, void **e) {
    (void)c; (void)request;
    if (e) *e = NULL;
    return NULL;
}

int xcb_poll_for_reply(xcb_connection_t *c, unsigned int request, void **reply, void **e) {
    (void)c; (void)request;
    if (reply) *reply = NULL;
    if (e) *e = NULL;
    return 1; // No reply available
}

void xcb_discard_reply(xcb_connection_t *c, unsigned int sequence) {
    (void)c; (void)sequence;
}

void xcb_discard_reply64(xcb_connection_t *c, uint64_t sequence) {
    (void)c; (void)sequence;
}

// XCB-Xlib interop functions
xcb_connection_t* XGetXCBConnection(void* dpy) {
    (void)dpy;
    fprintf(stderr, "[XCB-Bridge] XGetXCBConnection() -> fake conn\n");
    return &fake_conn;
}

void XSetEventQueueOwner(void* dpy, int owner) {
    (void)dpy; (void)owner;
    fprintf(stderr, "[XCB-Bridge] XSetEventQueueOwner(%d)\n", owner);
}

// Hook select() to see if vkcube is blocking on it
#include <sys/select.h>
static int (*real_select)(int, fd_set*, fd_set*, fd_set*, struct timeval*) = NULL;
int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout) {
    static int select_count = 0;
    if (select_count < 20 || (select_count % 60 == 0)) {
        fprintf(stderr, "[XCB-Bridge] select() called (nfds=%d, call #%d)\n", nfds, select_count);
        fflush(stderr);
    }
    select_count++;

    if (!real_select) {
        real_select = dlsym(RTLD_NEXT, "select");
    }

    // If waiting on our eventfd, signal it so select returns immediately
    if (readfds && g_event_fd >= 0 && FD_ISSET(g_event_fd, readfds)) {
        signal_event_fd();
    }

    if (real_select) {
        return real_select(nfds, readfds, writefds, exceptfds, timeout);
    }
    return 0;
}

// Hook poll() as well
#include <poll.h>
static int (*real_poll)(struct pollfd*, nfds_t, int) = NULL;
int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    static int poll_count = 0;
    if (poll_count < 20 || (poll_count % 60 == 0)) {
        fprintf(stderr, "[XCB-Bridge] poll() called (nfds=%lu, timeout=%d, call #%d)\n",
                (unsigned long)nfds, timeout, poll_count);
        fflush(stderr);
    }
    poll_count++;

    if (!real_poll) {
        real_poll = dlsym(RTLD_NEXT, "poll");
    }

    // Signal eventfd for any fd that might be ours
    if (g_event_fd >= 0) {
        for (nfds_t i = 0; i < nfds; i++) {
            if (fds[i].fd == g_event_fd) {
                signal_event_fd();
                break;
            }
        }
    }

    if (real_poll) {
        return real_poll(fds, nfds, timeout);
    }
    return 0;
}

// Hook ppoll as well
static int (*real_ppoll)(struct pollfd*, nfds_t, const struct timespec*, const sigset_t*) = NULL;
int ppoll(struct pollfd *fds, nfds_t nfds, const struct timespec *timeout, const sigset_t *sigmask) {
    static int ppoll_count = 0;
    if (ppoll_count < 20 || (ppoll_count % 60 == 0)) {
        fprintf(stderr, "[XCB-Bridge] ppoll() called (nfds=%lu, call #%d)\n",
                (unsigned long)nfds, ppoll_count);
        fflush(stderr);
    }
    ppoll_count++;

    if (!real_ppoll) {
        real_ppoll = dlsym(RTLD_NEXT, "ppoll");
    }

    if (g_event_fd >= 0) {
        for (nfds_t i = 0; i < nfds; i++) {
            if (fds[i].fd == g_event_fd) {
                signal_event_fd();
                break;
            }
        }
    }

    if (real_ppoll) {
        return real_ppoll(fds, nfds, timeout, sigmask);
    }
    return 0;
}

// Hook epoll_wait
#include <sys/epoll.h>
static int (*real_epoll_wait)(int, struct epoll_event*, int, int) = NULL;
int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout) {
    static int epoll_count = 0;
    if (epoll_count < 20 || (epoll_count % 60 == 0)) {
        fprintf(stderr, "[XCB-Bridge] epoll_wait() called (epfd=%d, call #%d)\n", epfd, epoll_count);
        fflush(stderr);
    }
    epoll_count++;

    if (!real_epoll_wait) {
        real_epoll_wait = dlsym(RTLD_NEXT, "epoll_wait");
    }
    if (real_epoll_wait) {
        return real_epoll_wait(epfd, events, maxevents, timeout);
    }
    return 0;
}

// Exit handler to see when process exits
static void on_exit_handler(void) {
    fprintf(stderr, "[XCB-Bridge] *** PROCESS EXITING (atexit handler) ***\n");
    fflush(stderr);
}

// Destructor
__attribute__((destructor))
static void fini(void) {
    fprintf(stderr, "[XCB-Bridge] *** LIBRARY UNLOADING (destructor) ***\n");
    fflush(stderr);
}

// Constructor
__attribute__((constructor))
static void init(void) {
    fprintf(stderr, "[XCB-Bridge] Vulkan XCB-to-Xlib bridge loaded\n");
    fprintf(stderr, "[XCB-Bridge]   VK_KHR_xcb_surface -> bridges to VK_KHR_xlib_surface\n");
    fprintf(stderr, "[XCB-Bridge]   VK_EXT_headless_surface -> headless rendering\n");
    real_vkGetInstanceProcAddr = dlsym(RTLD_NEXT, "vkGetInstanceProcAddr");
    real_vkEnumerateInstanceExtensionProperties = dlsym(RTLD_NEXT, "vkEnumerateInstanceExtensionProperties");
    atexit(on_exit_handler);
}
