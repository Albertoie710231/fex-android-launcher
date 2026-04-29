/*
 * VK_LAYER_WRAPPER_PIPELINE_LIBRARY
 *
 * Guarded VK_KHR_pipeline_library / VK_EXT_graphics_pipeline_library shim.
 * The leegao wrapper does not expose these extensions, and forwarding partial
 * graphics-pipeline-library creates to it is unsafe. This layer advertises the
 * feature, answers feature/property queries, turns library pipeline creates
 * into fake handles, and strips pipeline-library pNext structs from final
 * pipeline creates before forwarding them to the wrapper.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct VkLayerInstanceLink_ {
    struct VkLayerInstanceLink_ *pNext;
    PFN_vkVoidFunction (*pfnNextGetInstanceProcAddr)(VkInstance, const char *);
    PFN_vkVoidFunction (*pfnNextGetPhysicalDeviceProcAddr)(VkInstance, const char *);
} VkLayerInstanceLink;

typedef struct { VkStructureType sType; const void *pNext; int32_t function; union { VkLayerInstanceLink *pLayerInfo; void *_pad; } u; } VkLayerInstanceCreateInfo;

typedef struct VkLayerDeviceLink_ {
    struct VkLayerDeviceLink_ *pNext;
    PFN_vkVoidFunction (*pfnNextGetInstanceProcAddr)(VkInstance, const char *);
    PFN_vkVoidFunction (*pfnNextGetDeviceProcAddr)(VkDevice, const char *);
} VkLayerDeviceLink;

typedef struct { VkStructureType sType; const void *pNext; int32_t function; union { VkLayerDeviceLink *pLayerInfo; void *_pad; } u; } VkLayerDeviceCreateInfo;

typedef struct FakePipelineNode {
    VkDevice device;
    VkPipeline handle;
    VkGraphicsPipelineLibraryFlagsEXT subsets;
    struct FakePipelineNode *next;
} FakePipelineNode;

typedef struct PNextCopy {
    VkBaseOutStructure header;
    unsigned char bytes[128];
} PNextCopy;

static PFN_vkGetInstanceProcAddr next_gipa;
static PFN_vkGetDeviceProcAddr next_gdpa;
static PFN_vkGetPhysicalDeviceFeatures2 next_get_features2;
static PFN_vkGetPhysicalDeviceFeatures2 next_get_features2_khr;
static PFN_vkGetPhysicalDeviceProperties2 next_get_properties2;
static PFN_vkGetPhysicalDeviceProperties2 next_get_properties2_khr;
static PFN_vkCreateGraphicsPipelines next_create_graphics_pipelines;
static PFN_vkDestroyPipeline next_destroy_pipeline;
static PFN_vkCmdBindPipeline next_cmd_bind_pipeline;

static pthread_mutex_t fake_lock = PTHREAD_MUTEX_INITIALIZER;
static FakePipelineNode *fake_pipelines;
static uintptr_t next_fake_pipeline = (uintptr_t)0x7f455850504c0001ull;

static int layer_enabled(void) {
    const char *e = getenv("WRAPPER_PIPELINE_LIBRARY");
    return e && e[0] == '1';
}

static VkPipeline make_fake_pipeline(VkDevice device, VkGraphicsPipelineLibraryFlagsEXT subsets) {
    FakePipelineNode *node = malloc(sizeof(*node));
    if (!node) return VK_NULL_HANDLE;
    node->device = device;
    node->subsets = subsets;
    pthread_mutex_lock(&fake_lock);
    node->handle = (VkPipeline)next_fake_pipeline++;
    node->next = fake_pipelines;
    fake_pipelines = node;
    pthread_mutex_unlock(&fake_lock);
    return node->handle;
}

static int is_fake_pipeline(VkDevice device, VkPipeline pipeline) {
    int found = 0;
    pthread_mutex_lock(&fake_lock);
    for (FakePipelineNode *n = fake_pipelines; n; n = n->next) {
        if (n->device == device && n->handle == pipeline) { found = 1; break; }
    }
    pthread_mutex_unlock(&fake_lock);
    return found;
}

static void destroy_fake_pipeline(VkDevice device, VkPipeline pipeline) {
    pthread_mutex_lock(&fake_lock);
    FakePipelineNode **pp = &fake_pipelines;
    while (*pp) {
        FakePipelineNode *n = *pp;
        if (n->device == device && n->handle == pipeline) {
            *pp = n->next;
            free(n);
            break;
        }
        pp = &n->next;
    }
    pthread_mutex_unlock(&fake_lock);
}

static VkGraphicsPipelineLibraryFlagsEXT gpl_subsets(const void *pNext) {
    for (const VkBaseInStructure *p = (const VkBaseInStructure *)pNext; p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT)
            return ((const VkGraphicsPipelineLibraryCreateInfoEXT *)p)->flags;
    }
    return 0;
}

static int has_library_info(const void *pNext) {
    for (const VkBaseInStructure *p = (const VkBaseInStructure *)pNext; p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR)
            return ((const VkPipelineLibraryCreateInfoKHR *)p)->libraryCount > 0;
    }
    return 0;
}

static int strip_pipeline_library_pnext(const void *src, PNextCopy *copies, int max_copies, const void **out) {
    int n = 0;
    VkBaseOutStructure *prev = NULL;
    *out = NULL;

    for (const VkBaseInStructure *p = (const VkBaseInStructure *)src; p && n < max_copies; p = p->pNext) {
        size_t size = 0;
        if (p->sType == VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT ||
            p->sType == VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR) {
            continue;
        }
        switch (p->sType) {
        case VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO:
            size = sizeof(VkPipelineRenderingCreateInfo);
            break;
        case VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR:
            size = sizeof(VkPipelineCreateFlags2CreateInfoKHR);
            break;
        case VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO:
            size = sizeof(VkPipelineCreationFeedbackCreateInfo);
            break;
        default:
            continue;
        }
        if (size > sizeof(copies[n].bytes)) continue;
        memcpy(copies[n].bytes, p, size);
        VkBaseOutStructure *cur = (VkBaseOutStructure *)copies[n].bytes;
        cur->pNext = NULL;
        if (prev) prev->pNext = cur;
        else *out = cur;
        prev = cur;
        n++;
    }
    return n;
}

static void fill_features2(VkPhysicalDeviceFeatures2 *features) {
    if (!layer_enabled() || !features) return;
    for (VkBaseOutStructure *p = (VkBaseOutStructure *)features->pNext; p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT)
            ((VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT *)p)->graphicsPipelineLibrary = VK_TRUE;
    }
}

static void fill_properties2(VkPhysicalDeviceProperties2 *properties) {
    if (!layer_enabled() || !properties) return;
    for (VkBaseOutStructure *p = (VkBaseOutStructure *)properties->pNext; p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT) {
            VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT *props =
                (VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT *)p;
            props->graphicsPipelineLibraryFastLinking = VK_FALSE;
            props->graphicsPipelineLibraryIndependentInterpolationDecoration = VK_FALSE;
        }
    }
}

static void resolve_device_funcs(VkDevice device) {
    if (!next_gdpa) return;
    if (!next_create_graphics_pipelines)
        next_create_graphics_pipelines = (PFN_vkCreateGraphicsPipelines)next_gdpa(device, "vkCreateGraphicsPipelines");
    if (!next_destroy_pipeline)
        next_destroy_pipeline = (PFN_vkDestroyPipeline)next_gdpa(device, "vkDestroyPipeline");
    if (!next_cmd_bind_pipeline)
        next_cmd_bind_pipeline = (PFN_vkCmdBindPipeline)next_gdpa(device, "vkCmdBindPipeline");
}

__attribute__((visibility("default"))) void PipelineLibrary_GetPhysicalDeviceFeatures2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f) { if (next_get_features2) next_get_features2(pd, f); fill_features2(f); }
__attribute__((visibility("default"))) void PipelineLibrary_GetPhysicalDeviceFeatures2KHR(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f) { if (next_get_features2_khr) next_get_features2_khr(pd, f); else if (next_get_features2) next_get_features2(pd, f); fill_features2(f); }
__attribute__((visibility("default"))) void PipelineLibrary_GetPhysicalDeviceProperties2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p) { if (next_get_properties2) next_get_properties2(pd, p); fill_properties2(p); }
__attribute__((visibility("default"))) void PipelineLibrary_GetPhysicalDeviceProperties2KHR(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p) { if (next_get_properties2_khr) next_get_properties2_khr(pd, p); else if (next_get_properties2) next_get_properties2(pd, p); fill_properties2(p); }

__attribute__((visibility("default")))
VkResult PipelineLibrary_CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                 const VkGraphicsPipelineCreateInfo *infos,
                                                 const VkAllocationCallbacks *allocator, VkPipeline *pipelines) {
    resolve_device_funcs(device);
    if (!next_create_graphics_pipelines) return VK_ERROR_INITIALIZATION_FAILED;
    if (!layer_enabled() || !infos)
        return next_create_graphics_pipelines(device, cache, count, infos, allocator, pipelines);

    VkResult overall = VK_SUCCESS;
    for (uint32_t i = 0; i < count; i++) {
        VkGraphicsPipelineLibraryFlagsEXT subsets = gpl_subsets(infos[i].pNext);
        int is_library = (infos[i].flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) != 0;
        if (is_library) {
            VkPipeline fake = make_fake_pipeline(device, subsets);
            if (!fake) {
                if (pipelines) pipelines[i] = VK_NULL_HANDLE;
                overall = VK_ERROR_OUT_OF_HOST_MEMORY;
            } else {
                if (pipelines) pipelines[i] = fake;
                fprintf(stderr, "[wrapper_pipeline_library] fake library pipeline subsets=0x%x\n", subsets);
            }
            continue;
        }

        VkGraphicsPipelineCreateInfo local = infos[i];
        PNextCopy copies[4];
        const void *filtered = NULL;
        strip_pipeline_library_pnext(infos[i].pNext, copies, 4, &filtered);
        local.pNext = filtered;
        local.flags &= ~(VkPipelineCreateFlags)(VK_PIPELINE_CREATE_LIBRARY_BIT_KHR |
            VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT |
            VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT);

        VkResult r = next_create_graphics_pipelines(device, cache, 1, &local, allocator,
                                                    pipelines ? &pipelines[i] : NULL);
        if (r != VK_SUCCESS && overall == VK_SUCCESS) overall = r;
        if (has_library_info(infos[i].pNext))
            fprintf(stderr, "[wrapper_pipeline_library] linked pipeline forwarded without library handles result=%d\n", (int)r);
    }
    return overall;
}

__attribute__((visibility("default")))
void PipelineLibrary_DestroyPipeline(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks *allocator) {
    resolve_device_funcs(device);
    if (pipeline && is_fake_pipeline(device, pipeline)) { destroy_fake_pipeline(device, pipeline); return; }
    if (next_destroy_pipeline) next_destroy_pipeline(device, pipeline, allocator);
}

__attribute__((visibility("default")))
void PipelineLibrary_CmdBindPipeline(VkCommandBuffer commandBuffer, VkPipelineBindPoint bindPoint, VkPipeline pipeline) {
    if (next_cmd_bind_pipeline) next_cmd_bind_pipeline(commandBuffer, bindPoint, pipeline);
}

__attribute__((visibility("default")))
VkResult PipelineLibrary_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *info, const VkAllocationCallbacks *allocator, VkDevice *device) {
    const VkLayerDeviceCreateInfo *ci = (const VkLayerDeviceCreateInfo *)info->pNext;
    while (ci && !(ci->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && ci->function == 0)) ci = (const VkLayerDeviceCreateInfo *)ci->pNext;
    if (!ci || !ci->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = ci->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    next_gdpa = ci->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    ((VkLayerDeviceCreateInfo *)ci)->u.pLayerInfo = ci->u.pLayerInfo->pNext;
    PFN_vkCreateDevice create_next = (PFN_vkCreateDevice)gipa(NULL, "vkCreateDevice");
    if (!create_next) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = create_next(pd, info, allocator, device);
    if (r == VK_SUCCESS && device && *device) { resolve_device_funcs(*device); if (layer_enabled()) fprintf(stderr, "[wrapper_pipeline_library] device created, hooks armed\n"); }
    return r;
}

__attribute__((visibility("default")))
VkResult PipelineLibrary_CreateInstance(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *allocator, VkInstance *instance) {
    const VkLayerInstanceCreateInfo *ci = (const VkLayerInstanceCreateInfo *)info->pNext;
    while (ci && !(ci->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && ci->function == 0)) ci = (const VkLayerInstanceCreateInfo *)ci->pNext;
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
    if (layer_enabled()) fprintf(stderr, "[wrapper_pipeline_library] instance created\n");
    return r;
}

__attribute__((visibility("default")))
PFN_vkVoidFunction PipelineLibrary_GetInstanceProcAddr(VkInstance instance, const char *name) {
    if (!name) return NULL;
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)PipelineLibrary_CreateInstance;
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)PipelineLibrary_GetInstanceProcAddr;
    if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)PipelineLibrary_CreateDevice;
    if (!strcmp(name, "vkGetPhysicalDeviceFeatures2")) return (PFN_vkVoidFunction)PipelineLibrary_GetPhysicalDeviceFeatures2;
    if (!strcmp(name, "vkGetPhysicalDeviceFeatures2KHR")) return (PFN_vkVoidFunction)PipelineLibrary_GetPhysicalDeviceFeatures2KHR;
    if (!strcmp(name, "vkGetPhysicalDeviceProperties2")) return (PFN_vkVoidFunction)PipelineLibrary_GetPhysicalDeviceProperties2;
    if (!strcmp(name, "vkGetPhysicalDeviceProperties2KHR")) return (PFN_vkVoidFunction)PipelineLibrary_GetPhysicalDeviceProperties2KHR;
    return next_gipa ? next_gipa(instance, name) : NULL;
}

__attribute__((visibility("default")))
PFN_vkVoidFunction PipelineLibrary_GetDeviceProcAddr(VkDevice device, const char *name) {
    if (!name) return NULL;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)PipelineLibrary_GetDeviceProcAddr;
    if (!strcmp(name, "vkCreateGraphicsPipelines")) return (PFN_vkVoidFunction)PipelineLibrary_CreateGraphicsPipelines;
    if (!strcmp(name, "vkDestroyPipeline")) return (PFN_vkVoidFunction)PipelineLibrary_DestroyPipeline;
    if (!strcmp(name, "vkCmdBindPipeline")) return (PFN_vkVoidFunction)PipelineLibrary_CmdBindPipeline;
    return next_gdpa ? next_gdpa(device, name) : NULL;
}
