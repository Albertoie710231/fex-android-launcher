cmake_minimum_required(VERSION 3.22.1)

project(Vortek C)
message("Building ${PROJECT_NAME}")

set(CMAKE_VERBOSE_MAKEFILE on)
set(WARNS "-Wall -Wno-discarded-qualifiers")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 ${WARNS} -DVT_CLIENT -DVK_USE_PLATFORM_XLIB_KHR")

include_directories(include)
# Use our newer Vulkan headers (the vortek source references 1.4-era struct
# types like VkPhysicalDeviceMapMemoryPlacedFeaturesEXT that NDK r27's sysroot
# does not ship).
include_directories(BEFORE SYSTEM ${CMAKE_CURRENT_SOURCE_DIR}/../vulkan-headers-src/include)

add_library(vulkan_vortek SHARED
            src/main.c
            src/vulkan_calls.c
            src/vk_object.c
            src/vk_object_pool.c
            src/arrays.c
            src/descriptor_update_template.c
            src/ring_buffer.c)

target_link_libraries(vulkan_vortek PRIVATE log)