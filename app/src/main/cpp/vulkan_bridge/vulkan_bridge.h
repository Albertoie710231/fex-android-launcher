/**
 * Vulkan Bridge Header
 *
 * Provides Vulkan passthrough functionality for running Linux Vulkan
 * applications on Android. Handles ICD configuration, WSI setup,
 * and Mali GPU-specific workarounds.
 */

#ifndef VULKAN_BRIDGE_H
#define VULKAN_BRIDGE_H

#include <string>
#include <vector>

namespace vulkan_bridge {

/**
 * GPU information structure.
 */
struct GpuInfo {
    std::string name;
    std::string vendor;
    uint32_t vendorId;
    uint32_t deviceId;
    uint32_t apiVersion;
    uint32_t driverVersion;
};

/**
 * Vulkan capabilities.
 */
struct VulkanCapabilities {
    bool supported;
    int apiVersionMajor;
    int apiVersionMinor;
    int apiVersionPatch;
    std::string driverVersion;
    std::vector<std::string> extensions;
    GpuInfo gpu;
};

/**
 * Check if Vulkan is available on the device.
 */
bool isVulkanAvailable();

/**
 * Get Vulkan capabilities.
 */
VulkanCapabilities getCapabilities();

/**
 * Get GPU information.
 */
GpuInfo getGpuInfo();

/**
 * Get recommended environment variables for Vulkan passthrough.
 */
std::vector<std::pair<std::string, std::string>> getEnvironmentVariables();

/**
 * Write ICD configuration file to the specified path.
 */
bool writeIcdConfig(const std::string& path);

/**
 * Check if the GPU supports a specific Vulkan extension.
 */
bool supportsExtension(const std::string& extensionName);

/**
 * Get Mali GPU workarounds as environment variables.
 */
std::vector<std::pair<std::string, std::string>> getMaliWorkarounds();

/**
 * Initialize Vulkan bridge (call once at startup).
 */
bool initialize();

/**
 * Cleanup Vulkan bridge resources.
 */
void cleanup();

} // namespace vulkan_bridge

#endif // VULKAN_BRIDGE_H
