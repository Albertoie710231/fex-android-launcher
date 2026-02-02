/**
 * Vulkan Bridge Implementation
 *
 * Provides Vulkan passthrough configuration and GPU detection
 * for running Linux Vulkan applications on Android.
 */

#include "vulkan_bridge.h"
#include <android/log.h>
#include <dlfcn.h>
#include <fstream>
#include <sstream>
#include <cstring>

#define LOG_TAG "VulkanBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

// Vulkan function pointers (loaded dynamically)
typedef void* PFN_vkVoidFunction;
typedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddr)(void* instance, const char* pName);
typedef int (*PFN_vkEnumerateInstanceVersion)(uint32_t* pApiVersion);

namespace vulkan_bridge {

static void* sVulkanLib = nullptr;
static PFN_vkGetInstanceProcAddr sGetInstanceProcAddr = nullptr;

bool initialize() {
    // Try to load Vulkan library
    sVulkanLib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!sVulkanLib) {
        LOGE("Failed to load libvulkan.so: %s", dlerror());
        return false;
    }

    sGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(sVulkanLib, "vkGetInstanceProcAddr");
    if (!sGetInstanceProcAddr) {
        LOGE("Failed to get vkGetInstanceProcAddr");
        dlclose(sVulkanLib);
        sVulkanLib = nullptr;
        return false;
    }

    LOGI("Vulkan bridge initialized");
    return true;
}

void cleanup() {
    if (sVulkanLib) {
        dlclose(sVulkanLib);
        sVulkanLib = nullptr;
    }
    sGetInstanceProcAddr = nullptr;
}

bool isVulkanAvailable() {
    if (!sVulkanLib && !initialize()) {
        return false;
    }
    return sGetInstanceProcAddr != nullptr;
}

VulkanCapabilities getCapabilities() {
    VulkanCapabilities caps = {};
    caps.supported = false;

    if (!isVulkanAvailable()) {
        return caps;
    }

    // Get Vulkan version
    auto vkEnumerateInstanceVersion = (PFN_vkEnumerateInstanceVersion)
        sGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion");

    if (vkEnumerateInstanceVersion) {
        uint32_t version = 0;
        if (vkEnumerateInstanceVersion(&version) == 0) { // VK_SUCCESS
            caps.apiVersionMajor = (version >> 22) & 0x3FF;
            caps.apiVersionMinor = (version >> 12) & 0x3FF;
            caps.apiVersionPatch = version & 0xFFF;
            caps.supported = true;

            LOGI("Vulkan version: %d.%d.%d",
                 caps.apiVersionMajor, caps.apiVersionMinor, caps.apiVersionPatch);
        }
    }

    // Get GPU info
    caps.gpu = getGpuInfo();

    return caps;
}

GpuInfo getGpuInfo() {
    GpuInfo info = {};
    info.name = "Mali GPU";
    info.vendor = "ARM";
    info.vendorId = 0x13B5; // ARM vendor ID

    // Try to detect specific GPU from system properties
    // In a real implementation, you'd query Vulkan for this
    info.name = "Mali-G710 (Dimensity)";

    return info;
}

std::vector<std::pair<std::string, std::string>> getEnvironmentVariables() {
    std::vector<std::pair<std::string, std::string>> env;

    // Vulkan ICD configuration
    env.push_back({"VK_ICD_FILENAMES", "/usr/share/vulkan/icd.d/android_icd.json"});

    // WSI configuration
    env.push_back({"MESA_VK_WSI_PRESENT_MODE", "fifo"});
    env.push_back({"VK_LAYER_PATH", "/usr/share/vulkan/explicit_layer.d"});

    // DXVK settings (for Proton/Wine)
    env.push_back({"DXVK_ASYNC", "1"});
    env.push_back({"DXVK_STATE_CACHE", "1"});
    env.push_back({"DXVK_LOG_LEVEL", "none"});

    // VKD3D settings (DirectX 12)
    env.push_back({"VKD3D_FEATURE_LEVEL", "12_1"});

    // Proton settings
    env.push_back({"PROTON_USE_WINED3D", "0"});
    env.push_back({"PROTON_NO_ESYNC", "0"});
    env.push_back({"PROTON_NO_FSYNC", "0"});

    // Mesa settings
    env.push_back({"MESA_GL_VERSION_OVERRIDE", "4.6"});
    env.push_back({"MESA_GLSL_VERSION_OVERRIDE", "460"});

    // Add Mali workarounds
    auto maliEnv = getMaliWorkarounds();
    env.insert(env.end(), maliEnv.begin(), maliEnv.end());

    return env;
}

std::vector<std::pair<std::string, std::string>> getMaliWorkarounds() {
    std::vector<std::pair<std::string, std::string>> env;

    // Mali-specific workarounds
    env.push_back({"MALI_NO_ASYNC_COMPUTE", "1"});

    // Disable features that cause issues on Mali
    env.push_back({"DXVK_CONFIG", "dxgi.maxFrameLatency = 1"});

    // BCn texture workaround (Mali doesn't natively support BCn)
    // Games using BCn textures may need software decompression
    env.push_back({"RADV_PERFTEST", "bolist"});

    return env;
}

bool writeIcdConfig(const std::string& path) {
    std::string icdJson = R"({
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "/system/lib64/libvulkan.so",
        "api_version": "1.3.0"
    }
})";

    std::ofstream file(path);
    if (!file.is_open()) {
        LOGE("Failed to open ICD config file: %s", path.c_str());
        return false;
    }

    file << icdJson;
    file.close();

    LOGI("ICD config written to: %s", path.c_str());
    return true;
}

bool supportsExtension(const std::string& extensionName) {
    // In a full implementation, you'd query Vulkan for supported extensions
    // For now, assume common extensions are supported on modern Mali GPUs

    static const std::vector<std::string> knownExtensions = {
        "VK_KHR_surface",
        "VK_KHR_android_surface",
        "VK_KHR_swapchain",
        "VK_KHR_maintenance1",
        "VK_KHR_maintenance2",
        "VK_KHR_maintenance3",
        "VK_KHR_multiview",
        "VK_KHR_shader_float16_int8",
        "VK_KHR_storage_buffer_storage_class",
        "VK_KHR_16bit_storage",
        "VK_KHR_8bit_storage",
        "VK_KHR_driver_properties",
        "VK_KHR_timeline_semaphore",
        "VK_EXT_descriptor_indexing",
        "VK_EXT_scalar_block_layout",
    };

    for (const auto& ext : knownExtensions) {
        if (ext == extensionName) {
            return true;
        }
    }

    return false;
}

} // namespace vulkan_bridge
