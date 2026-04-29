/*
 * VK_LAYER_WRAPPER_SHADER_IDENTIFIER
 *
 * Metadata implementation of VK_EXT_shader_module_identifier. The real Mali
 * wrapper does not expose the extension, but DXVK can use it as a pipeline
 * cache fast path. We provide stable identifiers derived from the shader
 * module create-info bytes and track created VkShaderModule handles so
 * vkGetShaderModuleIdentifierEXT can answer later queries.
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

typedef struct ShaderIdNode {
    VkDevice device;
    VkShaderModule module;
    uint8_t id[VK_MAX_SHADER_MODULE_IDENTIFIER_SIZE_EXT];
    struct ShaderIdNode *next;
} ShaderIdNode;

static PFN_vkGetInstanceProcAddr next_gipa;
static PFN_vkGetDeviceProcAddr next_gdpa;
static PFN_vkGetPhysicalDeviceFeatures2 next_get_features2;
static PFN_vkGetPhysicalDeviceFeatures2 next_get_features2_khr;
static PFN_vkGetPhysicalDeviceProperties2 next_get_properties2;
static PFN_vkGetPhysicalDeviceProperties2 next_get_properties2_khr;
static PFN_vkCreateShaderModule next_create_shader_module;
static PFN_vkDestroyShaderModule next_destroy_shader_module;

static pthread_mutex_t shader_id_lock = PTHREAD_MUTEX_INITIALIZER;
static ShaderIdNode *shader_ids;

static const uint8_t kAlgorithmUuid[VK_UUID_SIZE] = {
    0x46, 0x45, 0x58, 0x2d, 0x44, 0x58, 0x56, 0x4b,
    0x2d, 0x53, 0x49, 0x44, 0x2d, 0x30, 0x30, 0x31,
};

static int layer_enabled(void) {
    const char *e = getenv("WRAPPER_SHADER_IDENTIFIER");
    return e && e[0] == '1';
}

static void hash_mix(uint64_t *h, const void *data, size_t size) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < size; i++) {
        *h ^= p[i];
        *h *= 1099511628211ull;
    }
}

static void make_identifier(const VkShaderModuleCreateInfo *info, uint8_t out[VK_MAX_SHADER_MODULE_IDENTIFIER_SIZE_EXT]) {
    uint64_t h[4] = {
        1469598103934665603ull,
        1099511628211ull,
        0x9e3779b97f4a7c15ull,
        0xd6e8feb86659fd93ull,
    };
    uint64_t code_size = info ? (uint64_t)info->codeSize : 0;
    uint32_t flags = info ? (uint32_t)info->flags : 0;

    for (int i = 0; i < 4; i++) {
        hash_mix(&h[i], &code_size, sizeof(code_size));
        hash_mix(&h[i], &flags, sizeof(flags));
        if (info && info->pCode && info->codeSize)
            hash_mix(&h[i], info->pCode, info->codeSize);
        h[i] ^= (uint64_t)i * 0x517cc1b727220a95ull;
    }
    memcpy(out, h, VK_MAX_SHADER_MODULE_IDENTIFIER_SIZE_EXT);
}

static void fill_identifier(VkShaderModuleIdentifierEXT *identifier,
                            const uint8_t id[VK_MAX_SHADER_MODULE_IDENTIFIER_SIZE_EXT]) {
    if (!identifier) return;
    identifier->identifierSize = VK_MAX_SHADER_MODULE_IDENTIFIER_SIZE_EXT;
    memcpy(identifier->identifier, id, VK_MAX_SHADER_MODULE_IDENTIFIER_SIZE_EXT);
}

static void remember_shader_id(VkDevice device, VkShaderModule module,
                               const uint8_t id[VK_MAX_SHADER_MODULE_IDENTIFIER_SIZE_EXT]) {
    ShaderIdNode *node = malloc(sizeof(*node));
    if (!node) return;
    node->device = device;
    node->module = module;
    memcpy(node->id, id, VK_MAX_SHADER_MODULE_IDENTIFIER_SIZE_EXT);
    pthread_mutex_lock(&shader_id_lock);
    node->next = shader_ids;
    shader_ids = node;
    pthread_mutex_unlock(&shader_id_lock);
}

static int find_shader_id(VkDevice device, VkShaderModule module,
                          uint8_t out[VK_MAX_SHADER_MODULE_IDENTIFIER_SIZE_EXT]) {
    int found = 0;
    pthread_mutex_lock(&shader_id_lock);
    for (ShaderIdNode *n = shader_ids; n; n = n->next) {
        if (n->device == device && n->module == module) {
            memcpy(out, n->id, VK_MAX_SHADER_MODULE_IDENTIFIER_SIZE_EXT);
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&shader_id_lock);
    return found;
}

static void forget_shader_id(VkDevice device, VkShaderModule module) {
    pthread_mutex_lock(&shader_id_lock);
    ShaderIdNode **pp = &shader_ids;
    while (*pp) {
        ShaderIdNode *n = *pp;
        if (n->device == device && n->module == module) {
            *pp = n->next;
            free(n);
            break;
        }
        pp = &n->next;
    }
    pthread_mutex_unlock(&shader_id_lock);
}

static void fill_features2(VkPhysicalDeviceFeatures2 *features) {
    if (!layer_enabled() || !features) return;
    for (VkBaseOutStructure *p = (VkBaseOutStructure *)features->pNext; p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT) {
            ((VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT *)p)->shaderModuleIdentifier = VK_TRUE;
        }
    }
}

static void fill_properties2(VkPhysicalDeviceProperties2 *properties) {
    if (!layer_enabled() || !properties) return;
    for (VkBaseOutStructure *p = (VkBaseOutStructure *)properties->pNext; p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_PROPERTIES_EXT) {
            VkPhysicalDeviceShaderModuleIdentifierPropertiesEXT *props =
                (VkPhysicalDeviceShaderModuleIdentifierPropertiesEXT *)p;
            memcpy(props->shaderModuleIdentifierAlgorithmUUID, kAlgorithmUuid, VK_UUID_SIZE);
        }
    }
}

static void resolve_device_funcs(VkDevice device) {
    if (!next_gdpa) return;
    if (!next_create_shader_module)
        next_create_shader_module = (PFN_vkCreateShaderModule)next_gdpa(device, "vkCreateShaderModule");
    if (!next_destroy_shader_module)
        next_destroy_shader_module = (PFN_vkDestroyShaderModule)next_gdpa(device, "vkDestroyShaderModule");
}

__attribute__((visibility("default")))
void ShaderIdentifier_GetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2 *features) {
    if (next_get_features2) next_get_features2(physicalDevice, features);
    fill_features2(features);
}

__attribute__((visibility("default")))
void ShaderIdentifier_GetPhysicalDeviceFeatures2KHR(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2 *features) {
    if (next_get_features2_khr) next_get_features2_khr(physicalDevice, features);
    else if (next_get_features2) next_get_features2(physicalDevice, features);
    fill_features2(features);
}

__attribute__((visibility("default")))
void ShaderIdentifier_GetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2 *properties) {
    if (next_get_properties2) next_get_properties2(physicalDevice, properties);
    fill_properties2(properties);
}

__attribute__((visibility("default")))
void ShaderIdentifier_GetPhysicalDeviceProperties2KHR(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2 *properties) {
    if (next_get_properties2_khr) next_get_properties2_khr(physicalDevice, properties);
    else if (next_get_properties2) next_get_properties2(physicalDevice, properties);
    fill_properties2(properties);
}

__attribute__((visibility("default")))
VkResult ShaderIdentifier_CreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *info,
                                             const VkAllocationCallbacks *allocator, VkShaderModule *module) {
    resolve_device_funcs(device);
    if (!next_create_shader_module) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = next_create_shader_module(device, info, allocator, module);
    if (r == VK_SUCCESS && module && *module && info) {
        uint8_t id[VK_MAX_SHADER_MODULE_IDENTIFIER_SIZE_EXT];
        make_identifier(info, id);
        remember_shader_id(device, *module, id);
    }
    return r;
}

__attribute__((visibility("default")))
void ShaderIdentifier_DestroyShaderModule(VkDevice device, VkShaderModule module,
                                          const VkAllocationCallbacks *allocator) {
    resolve_device_funcs(device);
    if (module) forget_shader_id(device, module);
    if (next_destroy_shader_module) next_destroy_shader_module(device, module, allocator);
}

__attribute__((visibility("default")))
void ShaderIdentifier_GetShaderModuleIdentifierEXT(VkDevice device, VkShaderModule module,
                                                   VkShaderModuleIdentifierEXT *identifier) {
    uint8_t id[VK_MAX_SHADER_MODULE_IDENTIFIER_SIZE_EXT] = {0};
    if (find_shader_id(device, module, id)) fill_identifier(identifier, id);
    else fill_identifier(identifier, id);
}

__attribute__((visibility("default")))
void ShaderIdentifier_GetShaderModuleCreateInfoIdentifierEXT(VkDevice device,
                                                             const VkShaderModuleCreateInfo *info,
                                                             VkShaderModuleIdentifierEXT *identifier) {
    (void)device;
    uint8_t id[VK_MAX_SHADER_MODULE_IDENTIFIER_SIZE_EXT];
    make_identifier(info, id);
    fill_identifier(identifier, id);
}

__attribute__((visibility("default")))
VkResult ShaderIdentifier_CreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *info,
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
        if (layer_enabled()) fprintf(stderr, "[wrapper_shader_identifier] device created, hooks armed\n");
    }
    return r;
}

__attribute__((visibility("default")))
VkResult ShaderIdentifier_CreateInstance(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *allocator,
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

    if (layer_enabled()) fprintf(stderr, "[wrapper_shader_identifier] instance created\n");
    return r;
}

__attribute__((visibility("default")))
PFN_vkVoidFunction ShaderIdentifier_GetInstanceProcAddr(VkInstance instance, const char *name) {
    if (!name) return NULL;
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)ShaderIdentifier_CreateInstance;
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)ShaderIdentifier_GetInstanceProcAddr;
    if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)ShaderIdentifier_CreateDevice;
    if (!strcmp(name, "vkGetPhysicalDeviceFeatures2")) return (PFN_vkVoidFunction)ShaderIdentifier_GetPhysicalDeviceFeatures2;
    if (!strcmp(name, "vkGetPhysicalDeviceFeatures2KHR")) return (PFN_vkVoidFunction)ShaderIdentifier_GetPhysicalDeviceFeatures2KHR;
    if (!strcmp(name, "vkGetPhysicalDeviceProperties2")) return (PFN_vkVoidFunction)ShaderIdentifier_GetPhysicalDeviceProperties2;
    if (!strcmp(name, "vkGetPhysicalDeviceProperties2KHR")) return (PFN_vkVoidFunction)ShaderIdentifier_GetPhysicalDeviceProperties2KHR;
    return next_gipa ? next_gipa(instance, name) : NULL;
}

__attribute__((visibility("default")))
PFN_vkVoidFunction ShaderIdentifier_GetDeviceProcAddr(VkDevice device, const char *name) {
    if (!name) return NULL;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)ShaderIdentifier_GetDeviceProcAddr;
    if (!strcmp(name, "vkCreateShaderModule")) return (PFN_vkVoidFunction)ShaderIdentifier_CreateShaderModule;
    if (!strcmp(name, "vkDestroyShaderModule")) return (PFN_vkVoidFunction)ShaderIdentifier_DestroyShaderModule;
    if (!strcmp(name, "vkGetShaderModuleIdentifierEXT")) return (PFN_vkVoidFunction)ShaderIdentifier_GetShaderModuleIdentifierEXT;
    if (!strcmp(name, "vkGetShaderModuleCreateInfoIdentifierEXT")) return (PFN_vkVoidFunction)ShaderIdentifier_GetShaderModuleCreateInfoIdentifierEXT;
    return next_gdpa ? next_gdpa(device, name) : NULL;
}
