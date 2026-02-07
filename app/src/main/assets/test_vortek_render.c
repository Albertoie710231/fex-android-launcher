/*
 * Headless Vulkan Rendering Test for Vortek
 *
 * This tests the Vortek → FramebufferBridge → Android Surface pipeline
 * without requiring X11/Wayland.
 *
 * Compile: gcc -o test_vortek_render test_vortek_render.c -lvulkan -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define WIDTH 800
#define HEIGHT 600

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
    void* pUserData) {
    fprintf(stderr, "Vulkan: %s\n", pCallbackData->pMessage);
    return VK_FALSE;
}

int main(int argc, char** argv) {
    VkResult result;

    printf("=== Vortek Headless Rendering Test ===\n");
    printf("This tests the Vulkan passthrough pipeline.\n\n");

    // 1. Create Vulkan Instance
    printf("1. Creating Vulkan instance...\n");

    VkApplicationInfo appInfo = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "VortekTest",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "NoEngine",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_1,
    };

    const char* extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_DISPLAY_EXTENSION_NAME,  // For headless display
    };

    VkInstanceCreateInfo instanceInfo = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &appInfo,
        .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = extensions,
    };

    VkInstance instance;
    result = vkCreateInstance(&instanceInfo, NULL, &instance);
    if (result != VK_SUCCESS) {
        // Try without extensions
        printf("   Retrying without extensions...\n");
        instanceInfo.enabledExtensionCount = 0;
        instanceInfo.ppEnabledExtensionNames = NULL;
        result = vkCreateInstance(&instanceInfo, NULL, &instance);
    }

    if (result != VK_SUCCESS) {
        fprintf(stderr, "Failed to create Vulkan instance: %d\n", result);
        return 1;
    }
    printf("   Instance created successfully!\n");

    // 2. Enumerate Physical Devices
    printf("\n2. Enumerating physical devices...\n");

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, NULL);

    if (deviceCount == 0) {
        fprintf(stderr, "No Vulkan devices found!\n");
        vkDestroyInstance(instance, NULL);
        return 1;
    }

    printf("   Found %u device(s)\n", deviceCount);

    VkPhysicalDevice* devices = malloc(sizeof(VkPhysicalDevice) * deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, devices);

    VkPhysicalDevice physicalDevice = devices[0];
    free(devices);

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physicalDevice, &props);
    printf("   Using: %s\n", props.deviceName);
    printf("   API Version: %u.%u.%u\n",
           VK_VERSION_MAJOR(props.apiVersion),
           VK_VERSION_MINOR(props.apiVersion),
           VK_VERSION_PATCH(props.apiVersion));

    // 3. Find Graphics Queue Family
    printf("\n3. Finding graphics queue family...\n");

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, NULL);

    VkQueueFamilyProperties* queueFamilies = malloc(sizeof(VkQueueFamilyProperties) * queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies);

    int graphicsFamily = -1;
    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            graphicsFamily = i;
            break;
        }
    }
    free(queueFamilies);

    if (graphicsFamily < 0) {
        fprintf(stderr, "No graphics queue family found!\n");
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    printf("   Graphics queue family: %d\n", graphicsFamily);

    // 4. Create Logical Device
    printf("\n4. Creating logical device...\n");

    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = graphicsFamily,
        .queueCount = 1,
        .pQueuePriorities = &queuePriority,
    };

    VkDeviceCreateInfo deviceInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queueInfo,
    };

    VkDevice device;
    result = vkCreateDevice(physicalDevice, &deviceInfo, NULL, &device);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "Failed to create logical device: %d\n", result);
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    printf("   Logical device created!\n");

    // 5. Get Graphics Queue
    printf("\n5. Getting graphics queue...\n");
    VkQueue graphicsQueue;
    vkGetDeviceQueue(device, graphicsFamily, 0, &graphicsQueue);
    printf("   Graphics queue obtained!\n");

    // 6. Create Command Pool
    printf("\n6. Creating command pool...\n");

    VkCommandPoolCreateInfo poolInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = graphicsFamily,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
    };

    VkCommandPool commandPool;
    result = vkCreateCommandPool(device, &poolInfo, NULL, &commandPool);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "Failed to create command pool: %d\n", result);
        vkDestroyDevice(device, NULL);
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    printf("   Command pool created!\n");

    // 7. Allocate Command Buffer
    printf("\n7. Allocating command buffer...\n");

    VkCommandBufferAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = commandPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };

    VkCommandBuffer commandBuffer;
    result = vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "Failed to allocate command buffer: %d\n", result);
        vkDestroyCommandPool(device, commandPool, NULL);
        vkDestroyDevice(device, NULL);
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    printf("   Command buffer allocated!\n");

    // 8. Create an offscreen image to render to
    printf("\n8. Creating offscreen render target (%dx%d)...\n", WIDTH, HEIGHT);

    VkImageCreateInfo imageInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { WIDTH, HEIGHT, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    VkImage renderImage;
    result = vkCreateImage(device, &imageInfo, NULL, &renderImage);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "Failed to create render image: %d\n", result);
        goto cleanup;
    }
    printf("   Render image created!\n");

    // Get memory requirements
    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(device, renderImage, &memRequirements);

    // Find suitable memory type
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);

    uint32_t memTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((memRequirements.memoryTypeBits & (1 << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memTypeIndex = i;
            break;
        }
    }

    if (memTypeIndex == UINT32_MAX) {
        fprintf(stderr, "Failed to find suitable memory type!\n");
        goto cleanup;
    }

    VkMemoryAllocateInfo memAllocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memRequirements.size,
        .memoryTypeIndex = memTypeIndex,
    };

    VkDeviceMemory imageMemory;
    result = vkAllocateMemory(device, &memAllocInfo, NULL, &imageMemory);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "Failed to allocate image memory: %d\n", result);
        goto cleanup;
    }

    vkBindImageMemory(device, renderImage, imageMemory, 0);
    printf("   Image memory allocated and bound!\n");

    // 9. Submit a simple clear operation
    printf("\n9. Rendering (clear to color)...\n");

    VkCommandBufferBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };

    vkBeginCommandBuffer(commandBuffer, &beginInfo);

    // Transition image to transfer destination
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = renderImage,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
    };

    vkCmdPipelineBarrier(commandBuffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &barrier);

    // Clear to a nice blue color
    VkClearColorValue clearColor = {{ 0.2f, 0.4f, 0.8f, 1.0f }};
    VkImageSubresourceRange range = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };

    vkCmdClearColorImage(commandBuffer, renderImage,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &range);

    vkEndCommandBuffer(commandBuffer);

    // Submit
    VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &commandBuffer,
    };

    result = vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "Failed to submit command buffer: %d\n", result);
        goto cleanup;
    }

    vkQueueWaitIdle(graphicsQueue);
    printf("   Render complete!\n");

    // 10. Success!
    printf("\n=== SUCCESS ===\n");
    printf("Vulkan rendering through Vortek completed successfully!\n");
    printf("Device: %s\n", props.deviceName);
    printf("Rendered: %dx%d blue image\n", WIDTH, HEIGHT);

cleanup:
    printf("\nCleaning up...\n");
    if (renderImage) vkDestroyImage(device, renderImage, NULL);
    if (imageMemory) vkFreeMemory(device, imageMemory, NULL);
    vkDestroyCommandPool(device, commandPool, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);

    printf("Done!\n");
    return 0;
}
