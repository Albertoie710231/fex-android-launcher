/*
 * Minimal 32-bit Vulkan ICD stub for Steam's CVulkanTopology check.
 *
 * Steam's 32-bit process calls vkCreateInstance + vkEnumeratePhysicalDevices
 * to build a GPU topology. Without a 32-bit ICD, it fails with -9
 * (VK_ERROR_INCOMPATIBLE_DRIVER) and may refuse to -applaunch games.
 *
 * This stub returns a fake physical device with Mali-G720 properties
 * so Steam's topology check passes. No actual Vulkan rendering happens
 * through this — games use the 64-bit fex_thunk_icd.so.
 *
 * Build:
 *   x86_64-linux-gnu-gcc -m32 -shared -fPIC -O2 \
 *       -o libvulkan_dummy_i386.so vulkan_dummy_icd_i386.c
 *
 * Deploy to rootfs:
 *   /usr/lib/i386-linux-gnu/libvulkan_dummy_i386.so
 *   /usr/share/vulkan/icd.d/vulkan_dummy_i386.json
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* Vulkan constants */
#define VK_SUCCESS                     0
#define VK_ERROR_INITIALIZATION_FAILED (-3)
#define VK_API_VERSION_1_3             ((uint32_t)(1U << 22) | (3U << 12))
#define VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU 1
#define VK_MAX_PHYSICAL_DEVICE_NAME_SIZE 256
#define VK_UUID_SIZE                   16
#define VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO    1
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 1001000001

typedef void (*PFN_vkVoidFunction)(void);
typedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddr)(void*, const char*);

/* Fake handles — just non-NULL pointers */
static uint32_t fake_instance_storage = 0xDEAD0001;
static uint32_t fake_physdev_storage  = 0xDEAD0002;

#define FAKE_INSTANCE ((void*)&fake_instance_storage)
#define FAKE_PHYSDEV  ((void*)&fake_physdev_storage)

/* --- Vulkan structures (minimal, matching Vulkan spec layout) --- */

typedef struct {
    uint32_t apiVersion;
    uint32_t driverVersion;
    uint32_t vendorID;
    uint32_t deviceID;
    uint32_t deviceType;
    char     deviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    uint8_t  pipelineCacheUUID[VK_UUID_SIZE];
    /* VkPhysicalDeviceLimits — large struct, we zero it */
    uint8_t  limits[504];  /* sizeof(VkPhysicalDeviceLimits) on 32-bit */
    /* VkPhysicalDeviceSparseProperties */
    uint8_t  sparseProperties[20];
} VkPhysicalDeviceProperties;

typedef struct {
    uint32_t sType;
    void*    pNext;
    VkPhysicalDeviceProperties properties;
} VkPhysicalDeviceProperties2;

typedef struct {
    uint32_t queueFlags;
    uint32_t queueCount;
    uint32_t timestampValidBits;
    uint32_t minImageTransferGranularity[3];
} VkQueueFamilyProperties;

typedef struct {
    uint32_t memoryTypeCount;
    struct {
        uint32_t propertyFlags;
        uint32_t heapIndex;
    } memoryTypes[32];
    uint32_t memoryHeapCount;
    struct {
        uint64_t size;
        uint32_t flags;
    } memoryHeaps[16];
} VkPhysicalDeviceMemoryProperties;

/* --- ICD entry points --- */

static int32_t stub_vkCreateInstance(const void* pCreateInfo, const void* pAllocator,
                                      void** pInstance) {
    (void)pCreateInfo; (void)pAllocator;
    *pInstance = FAKE_INSTANCE;
    return VK_SUCCESS;
}

static void stub_vkDestroyInstance(void* instance, const void* pAllocator) {
    (void)instance; (void)pAllocator;
}

static int32_t stub_vkEnumeratePhysicalDevices(void* instance, uint32_t* pCount,
                                                 void** pDevices) {
    (void)instance;
    if (!pDevices) {
        *pCount = 1;
        return VK_SUCCESS;
    }
    if (*pCount >= 1) {
        pDevices[0] = FAKE_PHYSDEV;
        *pCount = 1;
    }
    return VK_SUCCESS;
}

static void stub_vkGetPhysicalDeviceProperties(void* physdev,
                                                 VkPhysicalDeviceProperties* props) {
    (void)physdev;
    memset(props, 0, sizeof(*props));
    props->apiVersion    = VK_API_VERSION_1_3;
    props->driverVersion = 1;
    props->vendorID      = 0x13B5;  /* ARM */
    props->deviceID      = 0x0001;
    props->deviceType    = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    strncpy(props->deviceName, "Mali-G720-Immortalis MC12 (dummy-i386)",
            VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1);
}

static void stub_vkGetPhysicalDeviceProperties2(void* physdev,
                                                  VkPhysicalDeviceProperties2* props2) {
    stub_vkGetPhysicalDeviceProperties(physdev, &props2->properties);
}

static int32_t stub_vkEnumerateInstanceExtensionProperties(const char* pLayerName,
                                                            uint32_t* pCount,
                                                            void* pProperties) {
    (void)pLayerName; (void)pProperties;
    *pCount = 0;
    return VK_SUCCESS;
}

static int32_t stub_vkEnumerateDeviceExtensionProperties(void* physdev,
                                                          const char* pLayerName,
                                                          uint32_t* pCount,
                                                          void* pProperties) {
    (void)physdev; (void)pLayerName; (void)pProperties;
    *pCount = 0;
    return VK_SUCCESS;
}

static int32_t stub_vkEnumerateInstanceLayerProperties(uint32_t* pCount, void* pProperties) {
    (void)pProperties;
    *pCount = 0;
    return VK_SUCCESS;
}

static void stub_vkGetPhysicalDeviceQueueFamilyProperties(void* physdev,
                                                            uint32_t* pCount,
                                                            VkQueueFamilyProperties* pProps) {
    (void)physdev;
    if (!pProps) {
        *pCount = 1;
        return;
    }
    if (*pCount >= 1) {
        memset(&pProps[0], 0, sizeof(pProps[0]));
        pProps[0].queueFlags = 0xF; /* GRAPHICS | COMPUTE | TRANSFER | SPARSE */
        pProps[0].queueCount = 1;
        pProps[0].timestampValidBits = 64;
        *pCount = 1;
    }
}

static void stub_vkGetPhysicalDeviceMemoryProperties(void* physdev,
                                                       VkPhysicalDeviceMemoryProperties* props) {
    (void)physdev;
    memset(props, 0, sizeof(*props));
    props->memoryTypeCount = 1;
    props->memoryTypes[0].propertyFlags = 0xF; /* DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT | HOST_CACHED */
    props->memoryTypes[0].heapIndex = 0;
    props->memoryHeapCount = 1;
    props->memoryHeaps[0].size = (uint64_t)2ULL * 1024 * 1024 * 1024; /* 2 GB */
    props->memoryHeaps[0].flags = 1; /* DEVICE_LOCAL */
}

static void stub_vkGetPhysicalDeviceFeatures(void* physdev, void* features) {
    (void)physdev;
    /* Zero all features — Steam topology doesn't need them enabled */
    memset(features, 0, 256);
}

static int32_t stub_vkCreateDevice(void* physdev, const void* pCreateInfo,
                                    const void* pAllocator, void** pDevice) {
    (void)physdev; (void)pCreateInfo; (void)pAllocator;
    /* Should never be called — this is topology-only. Return failure. */
    *pDevice = NULL;
    return VK_ERROR_INITIALIZATION_FAILED;
}

/* --- Dispatch table --- */

struct {
    const char* name;
    PFN_vkVoidFunction fn;
} g_dispatch[] = {
    { "vkCreateInstance",                          (PFN_vkVoidFunction)stub_vkCreateInstance },
    { "vkDestroyInstance",                         (PFN_vkVoidFunction)stub_vkDestroyInstance },
    { "vkEnumeratePhysicalDevices",                (PFN_vkVoidFunction)stub_vkEnumeratePhysicalDevices },
    { "vkGetPhysicalDeviceProperties",             (PFN_vkVoidFunction)stub_vkGetPhysicalDeviceProperties },
    { "vkGetPhysicalDeviceProperties2",            (PFN_vkVoidFunction)stub_vkGetPhysicalDeviceProperties2 },
    { "vkGetPhysicalDeviceProperties2KHR",         (PFN_vkVoidFunction)stub_vkGetPhysicalDeviceProperties2 },
    { "vkEnumerateInstanceExtensionProperties",    (PFN_vkVoidFunction)stub_vkEnumerateInstanceExtensionProperties },
    { "vkEnumerateDeviceExtensionProperties",      (PFN_vkVoidFunction)stub_vkEnumerateDeviceExtensionProperties },
    { "vkEnumerateInstanceLayerProperties",        (PFN_vkVoidFunction)stub_vkEnumerateInstanceLayerProperties },
    { "vkGetPhysicalDeviceQueueFamilyProperties",  (PFN_vkVoidFunction)stub_vkGetPhysicalDeviceQueueFamilyProperties },
    { "vkGetPhysicalDeviceMemoryProperties",       (PFN_vkVoidFunction)stub_vkGetPhysicalDeviceMemoryProperties },
    { "vkGetPhysicalDeviceFeatures",               (PFN_vkVoidFunction)stub_vkGetPhysicalDeviceFeatures },
    { "vkCreateDevice",                            (PFN_vkVoidFunction)stub_vkCreateDevice },
    { NULL, NULL }
};

static PFN_vkVoidFunction stub_gipa(void* instance, const char* pName) {
    (void)instance;
    for (int i = 0; g_dispatch[i].name; i++) {
        if (strcmp(pName, g_dispatch[i].name) == 0)
            return g_dispatch[i].fn;
    }
    return NULL;
}

/* --- Loader ICD interface --- */

__attribute__((visibility("default")))
int32_t vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* pSupportedVersion) {
    if (*pSupportedVersion > 5)
        *pSupportedVersion = 5;
    return VK_SUCCESS;
}

__attribute__((visibility("default")))
PFN_vkVoidFunction vk_icdGetInstanceProcAddr(void* instance, const char* pName) {
    return stub_gipa(instance, pName);
}

__attribute__((visibility("default")))
PFN_vkVoidFunction vk_icdGetPhysicalDeviceProcAddr(void* instance, const char* pName) {
    (void)instance;
    /* Only return physical-device-level functions */
    if (strncmp(pName, "vkGetPhysicalDevice", 19) == 0 ||
        strncmp(pName, "vkEnumeratePhysicalDevice", 25) == 0 ||
        strncmp(pName, "vkEnumerateDeviceExtension", 26) == 0)
        return stub_gipa(NULL, pName);
    return NULL;
}
