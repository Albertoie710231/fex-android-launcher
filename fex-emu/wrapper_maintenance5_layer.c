/*
 * VK_LAYER_WRAPPER_MAINTENANCE5
 *
 * Productized maintenance5 shim for the leegao Mali wrapper. The layer
 * advertises VK_KHR_maintenance5 through its implicit-layer manifest and
 * implements the small subset DXVK 2.7+ needs on top of Vulkan 1.3-era entry
 * points exposed by the wrapper.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct VkLayerInstanceLink_ {
    struct VkLayerInstanceLink_ *pNext;
    PFN_vkVoidFunction (*pfnNextGetInstanceProcAddr)(VkInstance, const char *);
    PFN_vkVoidFunction (*pfnNextGetPhysicalDeviceProcAddr)(VkInstance, const char *);
} VkLayerInstanceLink;

typedef struct {
    VkStructureType sType;
    const void *pNext;
    int32_t function;
    union { VkLayerInstanceLink *pLayerInfo; void *_pad; } u;
} VkLayerInstanceCreateInfo;

typedef struct VkLayerDeviceLink_ {
    struct VkLayerDeviceLink_ *pNext;
    PFN_vkVoidFunction (*pfnNextGetInstanceProcAddr)(VkInstance, const char *);
    PFN_vkVoidFunction (*pfnNextGetDeviceProcAddr)(VkDevice, const char *);
} VkLayerDeviceLink;

typedef struct {
    VkStructureType sType;
    const void *pNext;
    int32_t function;
    union { VkLayerDeviceLink *pLayerInfo; void *_pad; } u;
} VkLayerDeviceCreateInfo;

static PFN_vkGetInstanceProcAddr next_gipa;
static PFN_vkGetDeviceProcAddr next_gdpa;
static PFN_vkGetPhysicalDeviceFeatures2 next_get_features2;
static PFN_vkGetPhysicalDeviceFeatures2 next_get_features2_khr;
static PFN_vkGetPhysicalDeviceProperties2 next_get_properties2;
static PFN_vkGetPhysicalDeviceProperties2 next_get_properties2_khr;

static PFN_vkCreateBuffer next_create_buffer;
static PFN_vkCreateGraphicsPipelines next_create_graphics_pipelines;
static PFN_vkCreateComputePipelines next_create_compute_pipelines;
static PFN_vkCmdBindIndexBuffer next_cmd_bind_index_buffer;
static PFN_vkGetImageSubresourceLayout next_get_image_subresource_layout;

static int layer_enabled(void) {
    const char *e = getenv("WRAPPER_MAINTENANCE5");
    return e && e[0] == '1';
}

static void fill_features2(VkPhysicalDeviceFeatures2 *features) {
    if (!layer_enabled() || !features) return;
    for (VkBaseOutStructure *p = (VkBaseOutStructure *)features->pNext; p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR) {
            ((VkPhysicalDeviceMaintenance5FeaturesKHR *)p)->maintenance5 = VK_TRUE;
        }
    }
}

static void fill_properties2(VkPhysicalDeviceProperties2 *properties) {
    if (!layer_enabled() || !properties) return;
    for (VkBaseOutStructure *p = (VkBaseOutStructure *)properties->pNext; p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_PROPERTIES_KHR) {
            VkPhysicalDeviceMaintenance5PropertiesKHR *m =
                (VkPhysicalDeviceMaintenance5PropertiesKHR *)p;
            m->earlyFragmentMultisampleCoverageAfterSampleCounting = VK_FALSE;
            m->earlyFragmentSampleMaskTestBeforeSampleCounting = VK_FALSE;
            m->depthStencilSwizzleOneSupport = VK_FALSE;
            m->polygonModePointSize = VK_FALSE;
            m->nonStrictSinglePixelWideLinesUseParallelogram = VK_FALSE;
            m->nonStrictWideLinesUseParallelogram = VK_FALSE;
        }
    }
}

static uint32_t low32(uint64_t v) {
    return (uint32_t)(v & 0xffffffffu);
}

static uint32_t folded_buffer_usage(const VkBufferCreateInfo *info) {
    uint32_t usage = info ? info->usage : 0;
    if (!info) return usage;
    for (const VkBaseInStructure *p = (const VkBaseInStructure *)info->pNext; p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR) {
            const VkBufferUsageFlags2CreateInfoKHR *f = (const VkBufferUsageFlags2CreateInfoKHR *)p;
            usage |= low32(f->usage);
        }
    }
    return usage;
}

static VkPipelineCreateFlags folded_pipeline_flags(VkPipelineCreateFlags flags, const void *pNext) {
    uint32_t out = flags;
    for (const VkBaseInStructure *p = (const VkBaseInStructure *)pNext; p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR) {
            const VkPipelineCreateFlags2CreateInfoKHR *f = (const VkPipelineCreateFlags2CreateInfoKHR *)p;
            out |= low32(f->flags);
        }
    }
    return (VkPipelineCreateFlags)out;
}

static void resolve_device_funcs(VkDevice device) {
    if (!next_gdpa) return;
    if (!next_create_buffer)
        next_create_buffer = (PFN_vkCreateBuffer)next_gdpa(device, "vkCreateBuffer");
    if (!next_create_graphics_pipelines)
        next_create_graphics_pipelines = (PFN_vkCreateGraphicsPipelines)next_gdpa(device, "vkCreateGraphicsPipelines");
    if (!next_create_compute_pipelines)
        next_create_compute_pipelines = (PFN_vkCreateComputePipelines)next_gdpa(device, "vkCreateComputePipelines");
    if (!next_cmd_bind_index_buffer)
        next_cmd_bind_index_buffer = (PFN_vkCmdBindIndexBuffer)next_gdpa(device, "vkCmdBindIndexBuffer");
    if (!next_get_image_subresource_layout)
        next_get_image_subresource_layout = (PFN_vkGetImageSubresourceLayout)next_gdpa(device, "vkGetImageSubresourceLayout");
}

__attribute__((visibility("default")))
void Maintenance5_GetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2 *features) {
    if (next_get_features2) next_get_features2(physicalDevice, features);
    fill_features2(features);
}

__attribute__((visibility("default")))
void Maintenance5_GetPhysicalDeviceFeatures2KHR(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2 *features) {
    if (next_get_features2_khr) next_get_features2_khr(physicalDevice, features);
    else if (next_get_features2) next_get_features2(physicalDevice, features);
    fill_features2(features);
}

__attribute__((visibility("default")))
void Maintenance5_GetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2 *properties) {
    if (next_get_properties2) next_get_properties2(physicalDevice, properties);
    fill_properties2(properties);
}

__attribute__((visibility("default")))
void Maintenance5_GetPhysicalDeviceProperties2KHR(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2 *properties) {
    if (next_get_properties2_khr) next_get_properties2_khr(physicalDevice, properties);
    else if (next_get_properties2) next_get_properties2(physicalDevice, properties);
    fill_properties2(properties);
}

__attribute__((visibility("default")))
VkResult Maintenance5_CreateBuffer(VkDevice device, const VkBufferCreateInfo *info,
                                   const VkAllocationCallbacks *allocator, VkBuffer *buffer) {
    resolve_device_funcs(device);
    if (!next_create_buffer) return VK_ERROR_INITIALIZATION_FAILED;
    if (!layer_enabled() || !info) return next_create_buffer(device, info, allocator, buffer);
    VkBufferCreateInfo local = *info;
    local.usage = folded_buffer_usage(info);
    return next_create_buffer(device, &local, allocator, buffer);
}

__attribute__((visibility("default")))
VkResult Maintenance5_CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                              const VkGraphicsPipelineCreateInfo *infos,
                                              const VkAllocationCallbacks *allocator, VkPipeline *pipelines) {
    resolve_device_funcs(device);
    if (!next_create_graphics_pipelines) return VK_ERROR_INITIALIZATION_FAILED;
    if (!layer_enabled() || !infos || count == 0)
        return next_create_graphics_pipelines(device, cache, count, infos, allocator, pipelines);

    VkGraphicsPipelineCreateInfo *local = malloc(sizeof(*local) * count);
    if (!local) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy(local, infos, sizeof(*local) * count);
    for (uint32_t i = 0; i < count; i++)
        local[i].flags = folded_pipeline_flags(infos[i].flags, infos[i].pNext);
    VkResult r = next_create_graphics_pipelines(device, cache, count, local, allocator, pipelines);
    free(local);
    return r;
}

__attribute__((visibility("default")))
VkResult Maintenance5_CreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                             const VkComputePipelineCreateInfo *infos,
                                             const VkAllocationCallbacks *allocator, VkPipeline *pipelines) {
    resolve_device_funcs(device);
    if (!next_create_compute_pipelines) return VK_ERROR_INITIALIZATION_FAILED;
    if (!layer_enabled() || !infos || count == 0)
        return next_create_compute_pipelines(device, cache, count, infos, allocator, pipelines);

    VkComputePipelineCreateInfo *local = malloc(sizeof(*local) * count);
    if (!local) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy(local, infos, sizeof(*local) * count);
    for (uint32_t i = 0; i < count; i++)
        local[i].flags = folded_pipeline_flags(infos[i].flags, infos[i].pNext);
    VkResult r = next_create_compute_pipelines(device, cache, count, local, allocator, pipelines);
    free(local);
    return r;
}

__attribute__((visibility("default")))
void Maintenance5_CmdBindIndexBuffer2KHR(VkCommandBuffer commandBuffer, VkBuffer buffer,
                                         VkDeviceSize offset, VkDeviceSize size, VkIndexType indexType) {
    (void)size;
    if (next_cmd_bind_index_buffer)
        next_cmd_bind_index_buffer(commandBuffer, buffer, offset, indexType);
}

__attribute__((visibility("default")))
void Maintenance5_GetRenderingAreaGranularityKHR(VkDevice device, const VkRenderingAreaInfoKHR *info,
                                                 VkExtent2D *granularity) {
    (void)device; (void)info;
    if (granularity) { granularity->width = 1; granularity->height = 1; }
}

__attribute__((visibility("default")))
void Maintenance5_GetImageSubresourceLayout2KHR(VkDevice device, VkImage image,
                                                const VkImageSubresource2KHR *subresource,
                                                VkSubresourceLayout2KHR *layout) {
    resolve_device_funcs(device);
    if (!next_get_image_subresource_layout || !subresource || !layout) return;
    next_get_image_subresource_layout(device, image, &subresource->imageSubresource, &layout->subresourceLayout);
}

__attribute__((visibility("default")))
void Maintenance5_GetDeviceImageSubresourceLayoutKHR(VkDevice device,
                                                     const VkDeviceImageSubresourceInfoKHR *info,
                                                     VkSubresourceLayout2KHR *layout) {
    (void)device; (void)info;
    if (layout) memset(&layout->subresourceLayout, 0, sizeof(layout->subresourceLayout));
}

__attribute__((visibility("default")))
VkResult Maintenance5_CreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *info,
                                   const VkAllocationCallbacks *allocator, VkDevice *device) {
    const VkLayerDeviceCreateInfo *ci = (const VkLayerDeviceCreateInfo *)info->pNext;
    while (ci && !(ci->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && ci->function == 0))
        ci = (const VkLayerDeviceCreateInfo *)ci->pNext;
    if (!ci || !ci->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = ci->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    next_gdpa = ci->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    ((VkLayerDeviceCreateInfo *)ci)->u.pLayerInfo = ci->u.pLayerInfo->pNext;

    PFN_vkCreateDevice create_next = (PFN_vkCreateDevice)gipa(NULL, "vkCreateDevice");
    if (!create_next) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = create_next(physicalDevice, info, allocator, device);
    if (r == VK_SUCCESS && device && *device) {
        resolve_device_funcs(*device);
        if (layer_enabled()) fprintf(stderr, "[wrapper_maintenance5] device created, hooks armed\n");
    }
    return r;
}

__attribute__((visibility("default")))
VkResult Maintenance5_CreateInstance(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *allocator,
                                     VkInstance *instance) {
    const VkLayerInstanceCreateInfo *ci = (const VkLayerInstanceCreateInfo *)info->pNext;
    while (ci && !(ci->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && ci->function == 0))
        ci = (const VkLayerInstanceCreateInfo *)ci->pNext;
    if (!ci || !ci->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;

    next_gipa = ci->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    ((VkLayerInstanceCreateInfo *)ci)->u.pLayerInfo = ci->u.pLayerInfo->pNext;

    PFN_vkCreateInstance create_next = (PFN_vkCreateInstance)next_gipa(NULL, "vkCreateInstance");
    if (!create_next) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = create_next(info, allocator, instance);
    if (r != VK_SUCCESS) return r;

    next_get_features2 = (PFN_vkGetPhysicalDeviceFeatures2)next_gipa(*instance, "vkGetPhysicalDeviceFeatures2");
    next_get_features2_khr = (PFN_vkGetPhysicalDeviceFeatures2)next_gipa(*instance, "vkGetPhysicalDeviceFeatures2KHR");
    next_get_properties2 = (PFN_vkGetPhysicalDeviceProperties2)next_gipa(*instance, "vkGetPhysicalDeviceProperties2");
    next_get_properties2_khr = (PFN_vkGetPhysicalDeviceProperties2)next_gipa(*instance, "vkGetPhysicalDeviceProperties2KHR");

    if (layer_enabled()) fprintf(stderr, "[wrapper_maintenance5] instance created\n");
    return r;
}

__attribute__((visibility("default")))
PFN_vkVoidFunction Maintenance5_GetInstanceProcAddr(VkInstance instance, const char *name) {
    if (!name) return NULL;
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)Maintenance5_CreateInstance;
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)Maintenance5_GetInstanceProcAddr;
    if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)Maintenance5_CreateDevice;
    if (!strcmp(name, "vkGetPhysicalDeviceFeatures2")) return (PFN_vkVoidFunction)Maintenance5_GetPhysicalDeviceFeatures2;
    if (!strcmp(name, "vkGetPhysicalDeviceFeatures2KHR")) return (PFN_vkVoidFunction)Maintenance5_GetPhysicalDeviceFeatures2KHR;
    if (!strcmp(name, "vkGetPhysicalDeviceProperties2")) return (PFN_vkVoidFunction)Maintenance5_GetPhysicalDeviceProperties2;
    if (!strcmp(name, "vkGetPhysicalDeviceProperties2KHR")) return (PFN_vkVoidFunction)Maintenance5_GetPhysicalDeviceProperties2KHR;
    return next_gipa ? next_gipa(instance, name) : NULL;
}

__attribute__((visibility("default")))
PFN_vkVoidFunction Maintenance5_GetDeviceProcAddr(VkDevice device, const char *name) {
    if (!name) return NULL;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)Maintenance5_GetDeviceProcAddr;
    if (!strcmp(name, "vkCreateBuffer")) return (PFN_vkVoidFunction)Maintenance5_CreateBuffer;
    if (!strcmp(name, "vkCreateGraphicsPipelines")) return (PFN_vkVoidFunction)Maintenance5_CreateGraphicsPipelines;
    if (!strcmp(name, "vkCreateComputePipelines")) return (PFN_vkVoidFunction)Maintenance5_CreateComputePipelines;
    if (!strcmp(name, "vkCmdBindIndexBuffer2KHR")) return (PFN_vkVoidFunction)Maintenance5_CmdBindIndexBuffer2KHR;
    if (!strcmp(name, "vkGetRenderingAreaGranularityKHR")) return (PFN_vkVoidFunction)Maintenance5_GetRenderingAreaGranularityKHR;
    if (!strcmp(name, "vkGetImageSubresourceLayout2KHR")) return (PFN_vkVoidFunction)Maintenance5_GetImageSubresourceLayout2KHR;
    if (!strcmp(name, "vkGetDeviceImageSubresourceLayoutKHR")) return (PFN_vkVoidFunction)Maintenance5_GetDeviceImageSubresourceLayoutKHR;
    return next_gdpa ? next_gdpa(device, name) : NULL;
}
