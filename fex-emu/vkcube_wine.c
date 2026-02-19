/**
 * Spinning 3D cube for Wine/Vulkan — tests vertex buffers, push constants,
 * render pass, back-face culling through the full Wine→thunks→Vortek pipeline.
 *
 * Compile:
 *   x86_64-w64-mingw32-gcc -o vkcube_wine.exe vkcube_wine.c -O2 -mconsole -lgdi32
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define PI 3.14159265358979f

/* ===== Vulkan types ===== */
typedef void*    VkInstance;
typedef void*    VkPhysicalDevice;
typedef void*    VkDevice;
typedef void*    VkCommandPool;
typedef void*    VkCommandBuffer;
typedef void*    VkQueue;
typedef uint64_t VkSurfaceKHR;
typedef uint64_t VkSwapchainKHR;
typedef uint64_t VkImage;
typedef uint64_t VkSemaphore;
typedef uint64_t VkFence;
typedef uint64_t VkRenderPass;
typedef uint64_t VkShaderModule;
typedef uint64_t VkPipelineLayout;
typedef uint64_t VkPipeline;
typedef uint64_t VkFramebuffer;
typedef uint64_t VkImageView;
typedef uint64_t VkPipelineCache;
typedef uint64_t VkBuffer;
typedef uint64_t VkDeviceMemory;
typedef uint32_t VkFlags;
typedef int32_t  VkResult;
typedef uint64_t VkDeviceSize;

#define VK_NULL_HANDLE 0
#define VK_SUCCESS     0

/* sType values */
#define STYPE_INSTANCE_CI               1
#define STYPE_DEVICE_QUEUE_CI           2
#define STYPE_DEVICE_CI                 3
#define STYPE_SUBMIT_INFO               4
#define STYPE_MEMORY_ALLOC_INFO         5
#define STYPE_FENCE_CI                  8
#define STYPE_SEMAPHORE_CI              9
#define STYPE_BUFFER_CI                 12
#define STYPE_IMAGE_VIEW_CI             15
#define STYPE_SHADER_MODULE_CI          16
#define STYPE_PIPELINE_SHADER_STAGE_CI  18
#define STYPE_PIPELINE_VERTEX_INPUT_CI  19
#define STYPE_PIPELINE_INPUT_ASM_CI     20
#define STYPE_PIPELINE_VIEWPORT_CI      22
#define STYPE_PIPELINE_RASTER_CI        23
#define STYPE_PIPELINE_MULTISAMPLE_CI   24
#define STYPE_PIPELINE_COLORBLEND_CI    26
#define STYPE_GRAPHICS_PIPELINE_CI      28
#define STYPE_PIPELINE_LAYOUT_CI        30
#define STYPE_FRAMEBUFFER_CI            37
#define STYPE_RENDER_PASS_CI            38
#define STYPE_CMD_POOL_CI               39
#define STYPE_CMD_BUF_AI                40
#define STYPE_CMD_BUF_BEGIN             42
#define STYPE_RENDER_PASS_BEGIN         43
#define STYPE_WIN32_SURFACE_CI          1000009000
#define STYPE_SWAPCHAIN_CI              1000001000
#define STYPE_PRESENT_INFO              1000001001

/* Enums/flags */
#define VK_FORMAT_B8G8R8A8_UNORM           44
#define VK_FORMAT_R32G32B32_SFLOAT         106
#define VK_COLOR_SPACE_SRGB_NONLINEAR      0
#define VK_PRESENT_MODE_FIFO               2
#define VK_IMAGE_LAYOUT_UNDEFINED          0
#define VK_IMAGE_LAYOUT_COLOR_ATTACH_OPT   2
#define VK_IMAGE_LAYOUT_PRESENT_SRC        1000001002
#define VK_COMPOSITE_ALPHA_OPAQUE          0x01
#define VK_COMPOSITE_ALPHA_INHERIT         0x08
#define VK_IMAGE_USAGE_TRANSFER_SRC        0x01
#define VK_IMAGE_USAGE_COLOR_ATTACHMENT    0x10
#define VK_SURFACE_TRANSFORM_IDENTITY      0x01
#define VK_QUEUE_GRAPHICS_BIT              0x01
#define VK_CMD_POOL_RESET_BIT              0x02
#define VK_CMD_BUF_LEVEL_PRIMARY           0
#define VK_CMD_BUF_USAGE_ONE_TIME          0x01
#define VK_FENCE_CREATE_SIGNALED           0x01
#define VK_SHARING_MODE_EXCLUSIVE          0
#define VK_IMAGE_VIEW_TYPE_2D              1
#define VK_IMAGE_ASPECT_COLOR_BIT          0x01
#define VK_ATTACHMENT_LOAD_OP_CLEAR        1
#define VK_ATTACHMENT_STORE_OP_STORE       0
#define VK_ATTACHMENT_LOAD_OP_DONT_CARE    2
#define VK_ATTACHMENT_STORE_OP_DONT_CARE   1
#define VK_PIPELINE_BIND_POINT_GRAPHICS    0
#define VK_SHADER_STAGE_VERTEX_BIT         0x01
#define VK_SHADER_STAGE_FRAGMENT_BIT       0x10
#define VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST 3
#define VK_POLYGON_MODE_FILL               0
#define VK_CULL_MODE_NONE                  0
#define VK_CULL_MODE_BACK                  2
#define VK_FRONT_FACE_CCW                  0
#define VK_SAMPLE_COUNT_1_BIT              1
#define VK_COLOR_COMPONENT_RGBA            0x0F
#define VK_SUBPASS_CONTENTS_INLINE         0
#define VK_PIPELINE_STAGE_COLOR_ATTACH_OUT 0x00000400
#define VK_ACCESS_COLOR_ATTACH_WRITE       0x00000100
#define VK_MEMORY_PROPERTY_HOST_VISIBLE    0x02
#define VK_MEMORY_PROPERTY_HOST_COHERENT   0x04
#define VK_BUFFER_USAGE_VERTEX_BUFFER      0x80
#define VK_VERTEX_INPUT_RATE_VERTEX        0

/* ===== Structures ===== */
typedef struct { uint32_t sType; const void *pNext; VkFlags flags;
    const void *pAppInfo; uint32_t layerCount; const char* const* ppLayers;
    uint32_t extCount; const char* const* ppExts; } VkInstanceCI;

typedef struct { uint32_t sType; const void *pNext; VkFlags flags;
    uint32_t queueFamilyIndex; uint32_t queueCount;
    const float *pPriorities; } VkDeviceQueueCI;

typedef struct { uint32_t sType; const void *pNext; VkFlags flags;
    uint32_t queueCICount; const VkDeviceQueueCI *pQueueCIs;
    uint32_t layerCount; const char* const* ppLayers;
    uint32_t extCount; const char* const* ppExts;
    const void *pFeatures; } VkDeviceCI;

typedef struct { VkFlags queueFlags; uint32_t queueCount;
    uint32_t timestampValidBits; uint32_t granularity[3]; } VkQueueFamilyProps;

typedef struct { uint32_t sType; const void *pNext; VkFlags flags;
    HINSTANCE hinstance; HWND hwnd; } VkWin32SurfaceCI;

typedef struct { uint32_t minImgCount; uint32_t maxImgCount;
    uint32_t curW; uint32_t curH; uint32_t minW; uint32_t minH;
    uint32_t maxW; uint32_t maxH; uint32_t maxLayers;
    VkFlags supportedTransforms; VkFlags currentTransform;
    VkFlags supportedComposite; VkFlags supportedUsage; } VkSurfaceCaps;

typedef struct { uint32_t format; uint32_t colorSpace; } VkSurfaceFormat;

typedef struct { uint32_t sType; const void *pNext; VkFlags flags;
    VkSurfaceKHR surface; uint32_t minImgCount; uint32_t imageFormat;
    uint32_t imageColorSpace; uint32_t extW; uint32_t extH;
    uint32_t arrayLayers; VkFlags imageUsage; uint32_t sharingMode;
    uint32_t qfIndexCount; const uint32_t *pQfIndices; VkFlags preTransform;
    VkFlags compositeAlpha; uint32_t presentMode; uint32_t clipped;
    VkSwapchainKHR oldSwapchain; } VkSwapchainCI;

typedef struct { uint32_t sType; const void *pNext; VkFlags flags;
    VkImage image; uint32_t viewType; uint32_t format;
    uint32_t r; uint32_t g; uint32_t b; uint32_t a;
    VkFlags aspectMask; uint32_t baseMip; uint32_t mipCount;
    uint32_t baseLayer; uint32_t layerCount; } VkImageViewCI;

typedef struct { VkFlags flags; uint32_t format; uint32_t samples; uint32_t loadOp;
    uint32_t storeOp; uint32_t stencilLoadOp; uint32_t stencilStoreOp;
    uint32_t initialLayout; uint32_t finalLayout; } VkAttachmentDesc;

typedef struct { uint32_t attachment; uint32_t layout; } VkAttachmentRef;

typedef struct { VkFlags flags; uint32_t pipelineBindPoint;
    uint32_t inputCount; const VkAttachmentRef *pInputs;
    uint32_t colorCount; const VkAttachmentRef *pColors;
    const VkAttachmentRef *pResolve; const VkAttachmentRef *pDepthStencil;
    uint32_t preserveCount; const uint32_t *pPreserve; } VkSubpassDesc;

typedef struct { uint32_t srcSubpass; uint32_t dstSubpass;
    VkFlags srcStageMask; VkFlags dstStageMask;
    VkFlags srcAccessMask; VkFlags dstAccessMask;
    VkFlags dependencyFlags; } VkSubpassDep;

typedef struct { uint32_t sType; const void *pNext; VkFlags flags;
    uint32_t attachCount; const VkAttachmentDesc *pAttachments;
    uint32_t subpassCount; const VkSubpassDesc *pSubpasses;
    uint32_t depCount; const VkSubpassDep *pDeps; } VkRenderPassCI;

typedef struct { uint32_t sType; const void *pNext; VkFlags flags;
    size_t codeSize; const uint32_t *pCode; } VkShaderModuleCI;

typedef struct { uint32_t sType; const void *pNext; VkFlags flags;
    uint32_t stage; VkShaderModule module;
    const char *pName; const void *pSpecialization; } VkPipelineShaderStageCI;

typedef struct { uint32_t binding; uint32_t stride; uint32_t inputRate; } VkVIBindingDesc;
typedef struct { uint32_t location; uint32_t binding; uint32_t format; uint32_t offset; } VkVIAttrDesc;

typedef struct { uint32_t sType; const void *pNext; VkFlags flags;
    uint32_t bindingCount; const VkVIBindingDesc *pBindings;
    uint32_t attrCount; const VkVIAttrDesc *pAttrs; } VkPipelineVertexInputCI;

typedef struct { uint32_t sType; const void *pNext; VkFlags flags;
    uint32_t topology; uint32_t primitiveRestart; } VkPipelineInputAsmCI;

typedef struct { float x; float y; float w; float h;
    float minDepth; float maxDepth; } VkViewport;

typedef struct { int32_t x; int32_t y; uint32_t w; uint32_t h; } VkRect2D;

typedef struct { uint32_t sType; const void *pNext; VkFlags flags;
    uint32_t viewportCount; const VkViewport *pViewports;
    uint32_t scissorCount; const VkRect2D *pScissors; } VkPipelineViewportCI;

typedef struct { uint32_t sType; const void *pNext; VkFlags flags;
    uint32_t depthClampEnable; uint32_t rastDiscardEnable;
    uint32_t polygonMode; VkFlags cullMode; uint32_t frontFace;
    uint32_t depthBiasEnable; float depthBiasConst; float depthBiasClamp;
    float depthBiasSlope; float lineWidth; } VkPipelineRasterCI;

typedef struct { uint32_t sType; const void *pNext; VkFlags flags;
    uint32_t rasterSamples; uint32_t sampleShading;
    float minSampleShading; const void *pSampleMask;
    uint32_t alphaToCoverage; uint32_t alphaToOne; } VkPipelineMultisampleCI;

typedef struct { uint32_t blendEnable; uint32_t srcColorFactor;
    uint32_t dstColorFactor; uint32_t colorBlendOp; uint32_t srcAlphaFactor;
    uint32_t dstAlphaFactor; uint32_t alphaBlendOp;
    VkFlags colorWriteMask; } VkPipelineColorBlendAttach;

typedef struct { uint32_t sType; const void *pNext; VkFlags flags;
    uint32_t logicOpEnable; uint32_t logicOp;
    uint32_t attachCount; const VkPipelineColorBlendAttach *pAttachments;
    float blendConstants[4]; } VkPipelineColorBlendCI;

typedef struct { VkFlags stageFlags; uint32_t offset; uint32_t size; } VkPushConstantRange;

typedef struct { uint32_t sType; const void *pNext; VkFlags flags;
    uint32_t setLayoutCount; const void *pSetLayouts;
    uint32_t pushConstRangeCount; const VkPushConstantRange *pPushConstRanges; } VkPipelineLayoutCI;

typedef struct { uint32_t sType; const void *pNext; VkFlags flags;
    uint32_t stageCount; const VkPipelineShaderStageCI *pStages;
    const VkPipelineVertexInputCI *pVertexInput;
    const VkPipelineInputAsmCI *pInputAsm;
    const void *pTessellation;
    const VkPipelineViewportCI *pViewport;
    const VkPipelineRasterCI *pRaster;
    const VkPipelineMultisampleCI *pMultisample;
    const void *pDepthStencil;
    const VkPipelineColorBlendCI *pColorBlend;
    const void *pDynamic;
    VkPipelineLayout layout; VkRenderPass renderPass; uint32_t subpass;
    VkPipeline basePipeline; int32_t basePipelineIndex; } VkGraphicsPipelineCI;

typedef struct { uint32_t sType; const void *pNext; VkFlags flags;
    VkRenderPass renderPass; uint32_t attachCount;
    const VkImageView *pAttachments; uint32_t width; uint32_t height;
    uint32_t layers; } VkFramebufferCI;

typedef struct { uint32_t sType; const void *pNext; VkFlags flags;
    uint32_t queueFamilyIndex; } VkCommandPoolCI;

typedef struct { uint32_t sType; const void *pNext;
    VkCommandPool commandPool; uint32_t level; uint32_t count; } VkCommandBufferAI;

typedef struct { uint32_t sType; const void *pNext; VkFlags flags;
    const void *pInheritance; } VkCommandBufferBI;

typedef union {
    float color[4];
    struct { float depth; uint32_t stencil; } ds;
} VkClearValue;

typedef struct { uint32_t sType; const void *pNext;
    VkRenderPass renderPass; VkFramebuffer framebuffer;
    VkRect2D renderArea;
    uint32_t clearValueCount; const VkClearValue *pClearValues; } VkRenderPassBI;

typedef struct { uint32_t sType; const void *pNext;
    uint32_t waitSemCount; const VkSemaphore *pWaitSems;
    const VkFlags *pWaitDstStage;
    uint32_t cmdBufCount; const VkCommandBuffer *pCmdBufs;
    uint32_t sigSemCount; const VkSemaphore *pSigSems; } VkSubmitInfo;

typedef struct { uint32_t sType; const void *pNext;
    uint32_t waitSemCount; const VkSemaphore *pWaitSems;
    uint32_t swapchainCount; const VkSwapchainKHR *pSwapchains;
    const uint32_t *pImageIndices; VkResult *pResults; } VkPresentInfo;

typedef struct { uint32_t sType; const void *pNext; } VkSemaphoreCI;
typedef struct { uint32_t sType; const void *pNext; VkFlags flags; } VkFenceCI;

typedef struct { uint32_t sType; const void *pNext; VkFlags flags;
    VkDeviceSize size; VkFlags usage; uint32_t sharingMode;
    uint32_t qfIndexCount; const uint32_t *pQfIndices; } VkBufferCI;

typedef struct { VkDeviceSize size; VkDeviceSize alignment;
    uint32_t memoryTypeBits; } VkMemReqs;

typedef struct { uint32_t sType; const void *pNext;
    VkDeviceSize allocationSize; uint32_t memoryTypeIndex; } VkMemAllocInfo;

typedef struct { VkFlags propertyFlags; uint32_t heapIndex; } VkMemType;
typedef struct { VkDeviceSize size; VkFlags flags; } VkMemHeap;
typedef struct { uint32_t memTypeCount; VkMemType memTypes[32];
    uint32_t memHeapCount; VkMemHeap memHeaps[16]; } VkPhysDevMemProps;

/* ===== Embedded SPIR-V shaders ===== */
/* Vertex: push_constant mat4 mvp; in vec3 inPos (loc 0); in vec3 inColor (loc 1); out vec3 fragColor */
static const unsigned char vert_spv[] = {
  0x03,0x02,0x23,0x07,0x00,0x00,0x01,0x00,0x0b,0x00,0x08,0x00,
  0x27,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x11,0x00,0x02,0x00,
  0x01,0x00,0x00,0x00,0x0b,0x00,0x06,0x00,0x01,0x00,0x00,0x00,
  0x47,0x4c,0x53,0x4c,0x2e,0x73,0x74,0x64,0x2e,0x34,0x35,0x30,
  0x00,0x00,0x00,0x00,0x0e,0x00,0x03,0x00,0x00,0x00,0x00,0x00,
  0x01,0x00,0x00,0x00,0x0f,0x00,0x09,0x00,0x00,0x00,0x00,0x00,
  0x04,0x00,0x00,0x00,0x6d,0x61,0x69,0x6e,0x00,0x00,0x00,0x00,
  0x0d,0x00,0x00,0x00,0x19,0x00,0x00,0x00,0x24,0x00,0x00,0x00,
  0x25,0x00,0x00,0x00,0x03,0x00,0x03,0x00,0x02,0x00,0x00,0x00,
  0xc2,0x01,0x00,0x00,0x05,0x00,0x04,0x00,0x04,0x00,0x00,0x00,
  0x6d,0x61,0x69,0x6e,0x00,0x00,0x00,0x00,0x05,0x00,0x06,0x00,
  0x0b,0x00,0x00,0x00,0x67,0x6c,0x5f,0x50,0x65,0x72,0x56,0x65,
  0x72,0x74,0x65,0x78,0x00,0x00,0x00,0x00,0x06,0x00,0x06,0x00,
  0x0b,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x67,0x6c,0x5f,0x50,
  0x6f,0x73,0x69,0x74,0x69,0x6f,0x6e,0x00,0x06,0x00,0x07,0x00,
  0x0b,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x67,0x6c,0x5f,0x50,
  0x6f,0x69,0x6e,0x74,0x53,0x69,0x7a,0x65,0x00,0x00,0x00,0x00,
  0x06,0x00,0x07,0x00,0x0b,0x00,0x00,0x00,0x02,0x00,0x00,0x00,
  0x67,0x6c,0x5f,0x43,0x6c,0x69,0x70,0x44,0x69,0x73,0x74,0x61,
  0x6e,0x63,0x65,0x00,0x06,0x00,0x07,0x00,0x0b,0x00,0x00,0x00,
  0x03,0x00,0x00,0x00,0x67,0x6c,0x5f,0x43,0x75,0x6c,0x6c,0x44,
  0x69,0x73,0x74,0x61,0x6e,0x63,0x65,0x00,0x05,0x00,0x03,0x00,
  0x0d,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x05,0x00,0x03,0x00,
  0x11,0x00,0x00,0x00,0x50,0x43,0x00,0x00,0x06,0x00,0x04,0x00,
  0x11,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x6d,0x76,0x70,0x00,
  0x05,0x00,0x03,0x00,0x13,0x00,0x00,0x00,0x70,0x63,0x00,0x00,
  0x05,0x00,0x04,0x00,0x19,0x00,0x00,0x00,0x69,0x6e,0x50,0x6f,
  0x73,0x00,0x00,0x00,0x05,0x00,0x05,0x00,0x24,0x00,0x00,0x00,
  0x66,0x72,0x61,0x67,0x43,0x6f,0x6c,0x6f,0x72,0x00,0x00,0x00,
  0x05,0x00,0x04,0x00,0x25,0x00,0x00,0x00,0x69,0x6e,0x43,0x6f,
  0x6c,0x6f,0x72,0x00,0x47,0x00,0x03,0x00,0x0b,0x00,0x00,0x00,
  0x02,0x00,0x00,0x00,0x48,0x00,0x05,0x00,0x0b,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x0b,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x48,0x00,0x05,0x00,0x0b,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
  0x0b,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x48,0x00,0x05,0x00,
  0x0b,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x0b,0x00,0x00,0x00,
  0x03,0x00,0x00,0x00,0x48,0x00,0x05,0x00,0x0b,0x00,0x00,0x00,
  0x03,0x00,0x00,0x00,0x0b,0x00,0x00,0x00,0x04,0x00,0x00,0x00,
  0x47,0x00,0x03,0x00,0x11,0x00,0x00,0x00,0x02,0x00,0x00,0x00,
  0x48,0x00,0x04,0x00,0x11,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x05,0x00,0x00,0x00,0x48,0x00,0x05,0x00,0x11,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x07,0x00,0x00,0x00,0x10,0x00,0x00,0x00,
  0x48,0x00,0x05,0x00,0x11,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x23,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x47,0x00,0x04,0x00,
  0x19,0x00,0x00,0x00,0x1e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x47,0x00,0x04,0x00,0x24,0x00,0x00,0x00,0x1e,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x47,0x00,0x04,0x00,0x25,0x00,0x00,0x00,
  0x1e,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x13,0x00,0x02,0x00,
  0x02,0x00,0x00,0x00,0x21,0x00,0x03,0x00,0x03,0x00,0x00,0x00,
  0x02,0x00,0x00,0x00,0x16,0x00,0x03,0x00,0x06,0x00,0x00,0x00,
  0x20,0x00,0x00,0x00,0x17,0x00,0x04,0x00,0x07,0x00,0x00,0x00,
  0x06,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x15,0x00,0x04,0x00,
  0x08,0x00,0x00,0x00,0x20,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x2b,0x00,0x04,0x00,0x08,0x00,0x00,0x00,0x09,0x00,0x00,0x00,
  0x01,0x00,0x00,0x00,0x1c,0x00,0x04,0x00,0x0a,0x00,0x00,0x00,
  0x06,0x00,0x00,0x00,0x09,0x00,0x00,0x00,0x1e,0x00,0x06,0x00,
  0x0b,0x00,0x00,0x00,0x07,0x00,0x00,0x00,0x06,0x00,0x00,0x00,
  0x0a,0x00,0x00,0x00,0x0a,0x00,0x00,0x00,0x20,0x00,0x04,0x00,
  0x0c,0x00,0x00,0x00,0x03,0x00,0x00,0x00,0x0b,0x00,0x00,0x00,
  0x3b,0x00,0x04,0x00,0x0c,0x00,0x00,0x00,0x0d,0x00,0x00,0x00,
  0x03,0x00,0x00,0x00,0x15,0x00,0x04,0x00,0x0e,0x00,0x00,0x00,
  0x20,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x2b,0x00,0x04,0x00,
  0x0e,0x00,0x00,0x00,0x0f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x18,0x00,0x04,0x00,0x10,0x00,0x00,0x00,0x07,0x00,0x00,0x00,
  0x04,0x00,0x00,0x00,0x1e,0x00,0x03,0x00,0x11,0x00,0x00,0x00,
  0x10,0x00,0x00,0x00,0x20,0x00,0x04,0x00,0x12,0x00,0x00,0x00,
  0x09,0x00,0x00,0x00,0x11,0x00,0x00,0x00,0x3b,0x00,0x04,0x00,
  0x12,0x00,0x00,0x00,0x13,0x00,0x00,0x00,0x09,0x00,0x00,0x00,
  0x20,0x00,0x04,0x00,0x14,0x00,0x00,0x00,0x09,0x00,0x00,0x00,
  0x10,0x00,0x00,0x00,0x17,0x00,0x04,0x00,0x17,0x00,0x00,0x00,
  0x06,0x00,0x00,0x00,0x03,0x00,0x00,0x00,0x20,0x00,0x04,0x00,
  0x18,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x17,0x00,0x00,0x00,
  0x3b,0x00,0x04,0x00,0x18,0x00,0x00,0x00,0x19,0x00,0x00,0x00,
  0x01,0x00,0x00,0x00,0x2b,0x00,0x04,0x00,0x06,0x00,0x00,0x00,
  0x1b,0x00,0x00,0x00,0x00,0x00,0x80,0x3f,0x20,0x00,0x04,0x00,
  0x21,0x00,0x00,0x00,0x03,0x00,0x00,0x00,0x07,0x00,0x00,0x00,
  0x20,0x00,0x04,0x00,0x23,0x00,0x00,0x00,0x03,0x00,0x00,0x00,
  0x17,0x00,0x00,0x00,0x3b,0x00,0x04,0x00,0x23,0x00,0x00,0x00,
  0x24,0x00,0x00,0x00,0x03,0x00,0x00,0x00,0x3b,0x00,0x04,0x00,
  0x18,0x00,0x00,0x00,0x25,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
  0x36,0x00,0x05,0x00,0x02,0x00,0x00,0x00,0x04,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x03,0x00,0x00,0x00,0xf8,0x00,0x02,0x00,
  0x05,0x00,0x00,0x00,0x41,0x00,0x05,0x00,0x14,0x00,0x00,0x00,
  0x15,0x00,0x00,0x00,0x13,0x00,0x00,0x00,0x0f,0x00,0x00,0x00,
  0x3d,0x00,0x04,0x00,0x10,0x00,0x00,0x00,0x16,0x00,0x00,0x00,
  0x15,0x00,0x00,0x00,0x3d,0x00,0x04,0x00,0x17,0x00,0x00,0x00,
  0x1a,0x00,0x00,0x00,0x19,0x00,0x00,0x00,0x51,0x00,0x05,0x00,
  0x06,0x00,0x00,0x00,0x1c,0x00,0x00,0x00,0x1a,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x51,0x00,0x05,0x00,0x06,0x00,0x00,0x00,
  0x1d,0x00,0x00,0x00,0x1a,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
  0x51,0x00,0x05,0x00,0x06,0x00,0x00,0x00,0x1e,0x00,0x00,0x00,
  0x1a,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x50,0x00,0x07,0x00,
  0x07,0x00,0x00,0x00,0x1f,0x00,0x00,0x00,0x1c,0x00,0x00,0x00,
  0x1d,0x00,0x00,0x00,0x1e,0x00,0x00,0x00,0x1b,0x00,0x00,0x00,
  0x91,0x00,0x05,0x00,0x07,0x00,0x00,0x00,0x20,0x00,0x00,0x00,
  0x16,0x00,0x00,0x00,0x1f,0x00,0x00,0x00,0x41,0x00,0x05,0x00,
  0x21,0x00,0x00,0x00,0x22,0x00,0x00,0x00,0x0d,0x00,0x00,0x00,
  0x0f,0x00,0x00,0x00,0x3e,0x00,0x03,0x00,0x22,0x00,0x00,0x00,
  0x20,0x00,0x00,0x00,0x3d,0x00,0x04,0x00,0x17,0x00,0x00,0x00,
  0x26,0x00,0x00,0x00,0x25,0x00,0x00,0x00,0x3e,0x00,0x03,0x00,
  0x24,0x00,0x00,0x00,0x26,0x00,0x00,0x00,0xfd,0x00,0x01,0x00,
  0x38,0x00,0x01,0x00
};
/* Fragment: in vec3 fragColor; out vec4 outColor = vec4(fragColor, 1.0) */
static const unsigned char frag_spv[] = {
  0x03,0x02,0x23,0x07,0x00,0x00,0x01,0x00,0x0b,0x00,0x08,0x00,
  0x13,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x11,0x00,0x02,0x00,
  0x01,0x00,0x00,0x00,0x0b,0x00,0x06,0x00,0x01,0x00,0x00,0x00,
  0x47,0x4c,0x53,0x4c,0x2e,0x73,0x74,0x64,0x2e,0x34,0x35,0x30,
  0x00,0x00,0x00,0x00,0x0e,0x00,0x03,0x00,0x00,0x00,0x00,0x00,
  0x01,0x00,0x00,0x00,0x0f,0x00,0x07,0x00,0x04,0x00,0x00,0x00,
  0x04,0x00,0x00,0x00,0x6d,0x61,0x69,0x6e,0x00,0x00,0x00,0x00,
  0x09,0x00,0x00,0x00,0x0c,0x00,0x00,0x00,0x10,0x00,0x03,0x00,
  0x04,0x00,0x00,0x00,0x07,0x00,0x00,0x00,0x03,0x00,0x03,0x00,
  0x02,0x00,0x00,0x00,0xc2,0x01,0x00,0x00,0x05,0x00,0x04,0x00,
  0x04,0x00,0x00,0x00,0x6d,0x61,0x69,0x6e,0x00,0x00,0x00,0x00,
  0x05,0x00,0x05,0x00,0x09,0x00,0x00,0x00,0x6f,0x75,0x74,0x43,
  0x6f,0x6c,0x6f,0x72,0x00,0x00,0x00,0x00,0x05,0x00,0x05,0x00,
  0x0c,0x00,0x00,0x00,0x66,0x72,0x61,0x67,0x43,0x6f,0x6c,0x6f,
  0x72,0x00,0x00,0x00,0x47,0x00,0x04,0x00,0x09,0x00,0x00,0x00,
  0x1e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x47,0x00,0x04,0x00,
  0x0c,0x00,0x00,0x00,0x1e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x13,0x00,0x02,0x00,0x02,0x00,0x00,0x00,0x21,0x00,0x03,0x00,
  0x03,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x16,0x00,0x03,0x00,
  0x06,0x00,0x00,0x00,0x20,0x00,0x00,0x00,0x17,0x00,0x04,0x00,
  0x07,0x00,0x00,0x00,0x06,0x00,0x00,0x00,0x04,0x00,0x00,0x00,
  0x20,0x00,0x04,0x00,0x08,0x00,0x00,0x00,0x03,0x00,0x00,0x00,
  0x07,0x00,0x00,0x00,0x3b,0x00,0x04,0x00,0x08,0x00,0x00,0x00,
  0x09,0x00,0x00,0x00,0x03,0x00,0x00,0x00,0x17,0x00,0x04,0x00,
  0x0a,0x00,0x00,0x00,0x06,0x00,0x00,0x00,0x03,0x00,0x00,0x00,
  0x20,0x00,0x04,0x00,0x0b,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
  0x0a,0x00,0x00,0x00,0x3b,0x00,0x04,0x00,0x0b,0x00,0x00,0x00,
  0x0c,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x2b,0x00,0x04,0x00,
  0x06,0x00,0x00,0x00,0x0e,0x00,0x00,0x00,0x00,0x00,0x80,0x3f,
  0x36,0x00,0x05,0x00,0x02,0x00,0x00,0x00,0x04,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x03,0x00,0x00,0x00,0xf8,0x00,0x02,0x00,
  0x05,0x00,0x00,0x00,0x3d,0x00,0x04,0x00,0x0a,0x00,0x00,0x00,
  0x0d,0x00,0x00,0x00,0x0c,0x00,0x00,0x00,0x51,0x00,0x05,0x00,
  0x06,0x00,0x00,0x00,0x0f,0x00,0x00,0x00,0x0d,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x51,0x00,0x05,0x00,0x06,0x00,0x00,0x00,
  0x10,0x00,0x00,0x00,0x0d,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
  0x51,0x00,0x05,0x00,0x06,0x00,0x00,0x00,0x11,0x00,0x00,0x00,
  0x0d,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x50,0x00,0x07,0x00,
  0x07,0x00,0x00,0x00,0x12,0x00,0x00,0x00,0x0f,0x00,0x00,0x00,
  0x10,0x00,0x00,0x00,0x11,0x00,0x00,0x00,0x0e,0x00,0x00,0x00,
  0x3e,0x00,0x03,0x00,0x09,0x00,0x00,0x00,0x12,0x00,0x00,0x00,
  0xfd,0x00,0x01,0x00,0x38,0x00,0x01,0x00
};

/* ===== Cube geometry: 36 vertices, each {pos.xyz, color.rgb} ===== */
static const float cube_verts[36 * 6] = {
    /* Front (+Z) — Red */
    -0.5f,-0.5f, 0.5f,  1,0,0,   0.5f,-0.5f, 0.5f,  1,0,0,   0.5f, 0.5f, 0.5f,  1,0,0,
    -0.5f,-0.5f, 0.5f,  1,0,0,   0.5f, 0.5f, 0.5f,  1,0,0,  -0.5f, 0.5f, 0.5f,  1,0,0,
    /* Back (-Z) — Green */
     0.5f,-0.5f,-0.5f,  0,1,0,  -0.5f,-0.5f,-0.5f,  0,1,0,  -0.5f, 0.5f,-0.5f,  0,1,0,
     0.5f,-0.5f,-0.5f,  0,1,0,  -0.5f, 0.5f,-0.5f,  0,1,0,   0.5f, 0.5f,-0.5f,  0,1,0,
    /* Top (+Y) — Blue */
     0.5f, 0.5f, 0.5f,  0,0,1,   0.5f, 0.5f,-0.5f,  0,0,1,  -0.5f, 0.5f,-0.5f,  0,0,1,
     0.5f, 0.5f, 0.5f,  0,0,1,  -0.5f, 0.5f,-0.5f,  0,0,1,  -0.5f, 0.5f, 0.5f,  0,0,1,
    /* Bottom (-Y) — Yellow */
    -0.5f,-0.5f, 0.5f,  1,1,0,  -0.5f,-0.5f,-0.5f,  1,1,0,   0.5f,-0.5f,-0.5f,  1,1,0,
    -0.5f,-0.5f, 0.5f,  1,1,0,   0.5f,-0.5f,-0.5f,  1,1,0,   0.5f,-0.5f, 0.5f,  1,1,0,
    /* Right (+X) — Cyan */
     0.5f,-0.5f, 0.5f,  0,1,1,   0.5f,-0.5f,-0.5f,  0,1,1,   0.5f, 0.5f,-0.5f,  0,1,1,
     0.5f,-0.5f, 0.5f,  0,1,1,   0.5f, 0.5f,-0.5f,  0,1,1,   0.5f, 0.5f, 0.5f,  0,1,1,
    /* Left (-X) — Magenta */
    -0.5f,-0.5f,-0.5f,  1,0,1,  -0.5f,-0.5f, 0.5f,  1,0,1,  -0.5f, 0.5f, 0.5f,  1,0,1,
    -0.5f,-0.5f,-0.5f,  1,0,1,  -0.5f, 0.5f, 0.5f,  1,0,1,  -0.5f, 0.5f,-0.5f,  1,0,1,
};

/* ===== Matrix math (column-major) ===== */
static void mat4_identity(float *m) {
    memset(m, 0, 64);
    m[0]=m[5]=m[10]=m[15]=1.0f;
}
static void mat4_mul(float *out, const float *a, const float *b) {
    float tmp[16];
    for (int c=0;c<4;c++) for (int r=0;r<4;r++) {
        float s=0; for (int k=0;k<4;k++) s+=a[k*4+r]*b[c*4+k]; tmp[c*4+r]=s;
    }
    memcpy(out, tmp, 64);
}
static void mat4_perspective(float *m, float fovY, float aspect, float zn, float zf) {
    float f=1.0f/tanf(fovY*0.5f);
    memset(m,0,64);
    m[0]=f/aspect; m[5]=-f; /* Vulkan Y-down */
    m[10]=zf/(zn-zf); m[11]=-1.0f; m[14]=(zn*zf)/(zn-zf);
}
static void mat4_lookAt(float *m, float ex,float ey,float ez, float cx,float cy,float cz, float ux,float uy,float uz) {
    float fx=cx-ex, fy=cy-ey, fz=cz-ez;
    float fl=sqrtf(fx*fx+fy*fy+fz*fz); fx/=fl; fy/=fl; fz/=fl;
    float sx=fy*uz-fz*uy, sy=fz*ux-fx*uz, sz=fx*uy-fy*ux;
    float sl=sqrtf(sx*sx+sy*sy+sz*sz); sx/=sl; sy/=sl; sz/=sl;
    float uux=sy*fz-sz*fy, uuy=sz*fx-sx*fz, uuz=sx*fy-sy*fx;
    memset(m,0,64);
    m[0]=sx;  m[4]=sy;  m[8]=sz;   m[12]=-(sx*ex+sy*ey+sz*ez);
    m[1]=uux; m[5]=uuy; m[9]=uuz;  m[13]=-(uux*ex+uuy*ey+uuz*ez);
    m[2]=-fx; m[6]=-fy; m[10]=-fz; m[14]=(fx*ex+fy*ey+fz*ez);
    m[3]=0; m[7]=0; m[11]=0; m[15]=1;
}
static void mat4_rotateY(float *m, float a) {
    float c=cosf(a), s=sinf(a);
    memset(m,0,64); m[0]=c; m[2]=-s; m[5]=1; m[8]=s; m[10]=c; m[15]=1;
}
static void mat4_rotateX(float *m, float a) {
    float c=cosf(a), s=sinf(a);
    memset(m,0,64); m[0]=1; m[5]=c; m[6]=s; m[9]=-s; m[10]=c; m[15]=1;
}

/* ===== Function pointer types ===== */
#define FNDEF(ret, name, ...) typedef ret (WINAPI *PFN_##name)(__VA_ARGS__)

FNDEF(VkResult, vkCreateInstance, const VkInstanceCI*, const void*, VkInstance*);
FNDEF(void, vkDestroyInstance, VkInstance, const void*);
FNDEF(VkResult, vkEnumeratePhysicalDevices, VkInstance, uint32_t*, VkPhysicalDevice*);
FNDEF(void, vkGetPhysicalDeviceQueueFamilyProperties, VkPhysicalDevice, uint32_t*, VkQueueFamilyProps*);
FNDEF(VkResult, vkCreateDevice, VkPhysicalDevice, const VkDeviceCI*, const void*, VkDevice*);
FNDEF(void*, vkGetDeviceProcAddr, VkDevice, const char*);
FNDEF(VkResult, vkCreateWin32SurfaceKHR, VkInstance, const VkWin32SurfaceCI*, const void*, VkSurfaceKHR*);
FNDEF(void, vkDestroySurfaceKHR, VkInstance, VkSurfaceKHR, const void*);
FNDEF(VkResult, vkGetPhysicalDeviceSurfaceCapabilitiesKHR, VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCaps*);
FNDEF(VkResult, vkGetPhysicalDeviceSurfaceFormatsKHR, VkPhysicalDevice, VkSurfaceKHR, uint32_t*, VkSurfaceFormat*);
FNDEF(VkResult, vkGetPhysicalDeviceSurfaceSupportKHR, VkPhysicalDevice, uint32_t, VkSurfaceKHR, uint32_t*);
FNDEF(void, vkGetPhysicalDeviceMemoryProperties, VkPhysicalDevice, VkPhysDevMemProps*);

/* Device-level */
FNDEF(void, vkDestroyDevice, VkDevice, const void*);
FNDEF(void, vkGetDeviceQueue, VkDevice, uint32_t, uint32_t, VkQueue*);
FNDEF(VkResult, vkCreateSwapchainKHR, VkDevice, const VkSwapchainCI*, const void*, VkSwapchainKHR*);
FNDEF(void, vkDestroySwapchainKHR, VkDevice, VkSwapchainKHR, const void*);
FNDEF(VkResult, vkGetSwapchainImagesKHR, VkDevice, VkSwapchainKHR, uint32_t*, VkImage*);
FNDEF(VkResult, vkAcquireNextImageKHR, VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t*);
FNDEF(VkResult, vkQueuePresentKHR, VkQueue, const VkPresentInfo*);
FNDEF(VkResult, vkQueueSubmit, VkQueue, uint32_t, const VkSubmitInfo*, VkFence);
FNDEF(VkResult, vkDeviceWaitIdle, VkDevice);
FNDEF(VkResult, vkCreateImageView, VkDevice, const VkImageViewCI*, const void*, VkImageView*);
FNDEF(void, vkDestroyImageView, VkDevice, VkImageView, const void*);
FNDEF(VkResult, vkCreateRenderPass, VkDevice, const VkRenderPassCI*, const void*, VkRenderPass*);
FNDEF(void, vkDestroyRenderPass, VkDevice, VkRenderPass, const void*);
FNDEF(VkResult, vkCreateShaderModule, VkDevice, const VkShaderModuleCI*, const void*, VkShaderModule*);
FNDEF(void, vkDestroyShaderModule, VkDevice, VkShaderModule, const void*);
FNDEF(VkResult, vkCreatePipelineLayout, VkDevice, const VkPipelineLayoutCI*, const void*, VkPipelineLayout*);
FNDEF(void, vkDestroyPipelineLayout, VkDevice, VkPipelineLayout, const void*);
FNDEF(VkResult, vkCreateGraphicsPipelines, VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCI*, const void*, VkPipeline*);
FNDEF(void, vkDestroyPipeline, VkDevice, VkPipeline, const void*);
FNDEF(VkResult, vkCreateFramebuffer, VkDevice, const VkFramebufferCI*, const void*, VkFramebuffer*);
FNDEF(void, vkDestroyFramebuffer, VkDevice, VkFramebuffer, const void*);
FNDEF(VkResult, vkCreateCommandPool, VkDevice, const VkCommandPoolCI*, const void*, VkCommandPool*);
FNDEF(void, vkDestroyCommandPool, VkDevice, VkCommandPool, const void*);
FNDEF(VkResult, vkAllocateCommandBuffers, VkDevice, const VkCommandBufferAI*, VkCommandBuffer*);
FNDEF(VkResult, vkBeginCommandBuffer, VkCommandBuffer, const VkCommandBufferBI*);
FNDEF(VkResult, vkEndCommandBuffer, VkCommandBuffer);
FNDEF(VkResult, vkResetCommandBuffer, VkCommandBuffer, VkFlags);
FNDEF(void, vkCmdBeginRenderPass, VkCommandBuffer, const VkRenderPassBI*, uint32_t);
FNDEF(void, vkCmdEndRenderPass, VkCommandBuffer);
FNDEF(void, vkCmdBindPipeline, VkCommandBuffer, uint32_t, VkPipeline);
FNDEF(void, vkCmdDraw, VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t);
FNDEF(VkResult, vkCreateSemaphore, VkDevice, const VkSemaphoreCI*, const void*, VkSemaphore*);
FNDEF(void, vkDestroySemaphore, VkDevice, VkSemaphore, const void*);
FNDEF(VkResult, vkCreateFence, VkDevice, const VkFenceCI*, const void*, VkFence*);
FNDEF(void, vkDestroyFence, VkDevice, VkFence, const void*);
FNDEF(VkResult, vkWaitForFences, VkDevice, uint32_t, const VkFence*, uint32_t, uint64_t);
FNDEF(VkResult, vkResetFences, VkDevice, uint32_t, const VkFence*);
FNDEF(VkResult, vkCreateBuffer, VkDevice, const VkBufferCI*, const void*, VkBuffer*);
FNDEF(void, vkDestroyBuffer, VkDevice, VkBuffer, const void*);
FNDEF(void, vkGetBufferMemoryRequirements, VkDevice, VkBuffer, VkMemReqs*);
FNDEF(VkResult, vkAllocateMemory, VkDevice, const VkMemAllocInfo*, const void*, VkDeviceMemory*);
FNDEF(void, vkFreeMemory, VkDevice, VkDeviceMemory, const void*);
FNDEF(VkResult, vkBindBufferMemory, VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize);
FNDEF(VkResult, vkMapMemory, VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkFlags, void**);
FNDEF(void, vkUnmapMemory, VkDevice, VkDeviceMemory);
FNDEF(void, vkCmdPushConstants, VkCommandBuffer, VkPipelineLayout, VkFlags, uint32_t, uint32_t, const void*);
FNDEF(void, vkCmdBindVertexBuffers, VkCommandBuffer, uint32_t, uint32_t, const VkBuffer*, const VkDeviceSize*);

/* ===== Helpers ===== */
static HMODULE hVulkan;
#define LOAD(name) pfn_##name = (PFN_##name)GetProcAddress(hVulkan, #name)
#define DLOAD(name) pfn_##name = (PFN_##name)pfn_vkGetDeviceProcAddr(dev, #name)
#define P(fmt, ...) do { fprintf(stderr, fmt "\n", ##__VA_ARGS__); fflush(stderr); } while(0)
#define FAIL(msg, ...) do { P("FAIL: " msg, ##__VA_ARGS__); ExitProcess(1); } while(0)
#define CHK(call, msg) do { VkResult _r = (call); if (_r != VK_SUCCESS) FAIL("%s = %d", msg, _r); } while(0)

static uint32_t find_mem_type(const VkPhysDevMemProps *p, uint32_t bits, VkFlags req) {
    for (uint32_t i=0; i<p->memTypeCount; i++)
        if ((bits&(1u<<i)) && (p->memTypes[i].propertyFlags&req)==req) return i;
    return 0xFFFFFFFF;
}

/* ===== Main ===== */
int main(int argc, char **argv) {
    int num_frames = 3000;
    if (argc > 1) num_frames = atoi(argv[1]);
    if (num_frames < 1) num_frames = 1;
    if (num_frames > 99999) num_frames = 99999;

    P("\n[vkcube] === Spinning 3D Cube (%d frames) ===", num_frames);

    VkInstance inst = NULL; VkDevice dev = NULL; VkQueue queue = NULL;
    VkPhysicalDevice gpu = NULL;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkShaderModule vertMod = VK_NULL_HANDLE, fragMod = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandPool cmdPool = NULL;
    VkFence fence = VK_NULL_HANDLE;
    VkBuffer vtxBuf = VK_NULL_HANDLE;
    VkDeviceMemory vtxMem = VK_NULL_HANDLE;
    HWND hwnd = NULL;
    uint32_t gfxQF = 0, swapW = 1280, swapH = 720;
    VkPhysDevMemProps memProps;
    #define MAX_SWAP 4
    VkImage swapImages[MAX_SWAP]; VkImageView swapViews[MAX_SWAP];
    VkFramebuffer swapFBs[MAX_SWAP]; VkCommandBuffer cmdBufs[MAX_SWAP];
    uint32_t swapCount = 0;
    memset(swapViews, 0, sizeof(swapViews));
    memset(swapFBs, 0, sizeof(swapFBs));
    memset(cmdBufs, 0, sizeof(cmdBufs));

    /* ALL function pointers */
    PFN_vkCreateInstance pfn_vkCreateInstance;
    PFN_vkDestroyInstance pfn_vkDestroyInstance;
    PFN_vkEnumeratePhysicalDevices pfn_vkEnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties pfn_vkGetPhysicalDeviceQueueFamilyProperties;
    PFN_vkCreateDevice pfn_vkCreateDevice;
    PFN_vkGetDeviceProcAddr pfn_vkGetDeviceProcAddr;
    PFN_vkCreateWin32SurfaceKHR pfn_vkCreateWin32SurfaceKHR;
    PFN_vkDestroySurfaceKHR pfn_vkDestroySurfaceKHR;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR pfn_vkGetPhysicalDeviceSurfaceCapabilitiesKHR;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR pfn_vkGetPhysicalDeviceSurfaceFormatsKHR;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR pfn_vkGetPhysicalDeviceSurfaceSupportKHR;
    PFN_vkGetPhysicalDeviceMemoryProperties pfn_vkGetPhysicalDeviceMemoryProperties;
    PFN_vkDestroyDevice pfn_vkDestroyDevice=NULL; PFN_vkGetDeviceQueue pfn_vkGetDeviceQueue=NULL;
    PFN_vkCreateSwapchainKHR pfn_vkCreateSwapchainKHR=NULL;
    PFN_vkDestroySwapchainKHR pfn_vkDestroySwapchainKHR=NULL;
    PFN_vkGetSwapchainImagesKHR pfn_vkGetSwapchainImagesKHR=NULL;
    PFN_vkAcquireNextImageKHR pfn_vkAcquireNextImageKHR=NULL;
    PFN_vkQueuePresentKHR pfn_vkQueuePresentKHR=NULL;
    PFN_vkQueueSubmit pfn_vkQueueSubmit=NULL; PFN_vkDeviceWaitIdle pfn_vkDeviceWaitIdle=NULL;
    PFN_vkCreateImageView pfn_vkCreateImageView=NULL; PFN_vkDestroyImageView pfn_vkDestroyImageView=NULL;
    PFN_vkCreateRenderPass pfn_vkCreateRenderPass=NULL; PFN_vkDestroyRenderPass pfn_vkDestroyRenderPass=NULL;
    PFN_vkCreateShaderModule pfn_vkCreateShaderModule=NULL; PFN_vkDestroyShaderModule pfn_vkDestroyShaderModule=NULL;
    PFN_vkCreatePipelineLayout pfn_vkCreatePipelineLayout=NULL; PFN_vkDestroyPipelineLayout pfn_vkDestroyPipelineLayout=NULL;
    PFN_vkCreateGraphicsPipelines pfn_vkCreateGraphicsPipelines=NULL; PFN_vkDestroyPipeline pfn_vkDestroyPipeline=NULL;
    PFN_vkCreateFramebuffer pfn_vkCreateFramebuffer=NULL; PFN_vkDestroyFramebuffer pfn_vkDestroyFramebuffer=NULL;
    PFN_vkCreateCommandPool pfn_vkCreateCommandPool=NULL; PFN_vkDestroyCommandPool pfn_vkDestroyCommandPool=NULL;
    PFN_vkAllocateCommandBuffers pfn_vkAllocateCommandBuffers=NULL;
    PFN_vkBeginCommandBuffer pfn_vkBeginCommandBuffer=NULL;
    PFN_vkEndCommandBuffer pfn_vkEndCommandBuffer=NULL;
    PFN_vkResetCommandBuffer pfn_vkResetCommandBuffer=NULL;
    PFN_vkCmdBeginRenderPass pfn_vkCmdBeginRenderPass=NULL;
    PFN_vkCmdEndRenderPass pfn_vkCmdEndRenderPass=NULL;
    PFN_vkCmdBindPipeline pfn_vkCmdBindPipeline=NULL; PFN_vkCmdDraw pfn_vkCmdDraw=NULL;
    PFN_vkCreateSemaphore pfn_vkCreateSemaphore=NULL; PFN_vkDestroySemaphore pfn_vkDestroySemaphore=NULL;
    PFN_vkCreateFence pfn_vkCreateFence=NULL; PFN_vkDestroyFence pfn_vkDestroyFence=NULL;
    PFN_vkWaitForFences pfn_vkWaitForFences=NULL; PFN_vkResetFences pfn_vkResetFences=NULL;
    PFN_vkCreateBuffer pfn_vkCreateBuffer=NULL; PFN_vkDestroyBuffer pfn_vkDestroyBuffer=NULL;
    PFN_vkGetBufferMemoryRequirements pfn_vkGetBufferMemoryRequirements=NULL;
    PFN_vkAllocateMemory pfn_vkAllocateMemory=NULL; PFN_vkFreeMemory pfn_vkFreeMemory=NULL;
    PFN_vkBindBufferMemory pfn_vkBindBufferMemory=NULL;
    PFN_vkMapMemory pfn_vkMapMemory=NULL; PFN_vkUnmapMemory pfn_vkUnmapMemory=NULL;
    PFN_vkCmdPushConstants pfn_vkCmdPushConstants=NULL;
    PFN_vkCmdBindVertexBuffers pfn_vkCmdBindVertexBuffers=NULL;

    /* 1. Load vulkan-1.dll + instance functions */
    hVulkan = LoadLibraryA("vulkan-1.dll");
    if (!hVulkan) FAIL("LoadLibrary(vulkan-1.dll) error %lu", GetLastError());
    LOAD(vkCreateInstance); LOAD(vkDestroyInstance); LOAD(vkEnumeratePhysicalDevices);
    LOAD(vkGetPhysicalDeviceQueueFamilyProperties); LOAD(vkCreateDevice); LOAD(vkGetDeviceProcAddr);
    LOAD(vkCreateWin32SurfaceKHR); LOAD(vkDestroySurfaceKHR);
    LOAD(vkGetPhysicalDeviceSurfaceCapabilitiesKHR); LOAD(vkGetPhysicalDeviceSurfaceFormatsKHR);
    LOAD(vkGetPhysicalDeviceSurfaceSupportKHR); LOAD(vkGetPhysicalDeviceMemoryProperties);
    if (!pfn_vkCreateInstance || !pfn_vkCreateWin32SurfaceKHR)
        FAIL("Missing critical instance functions");

    /* 2. Instance */
    { const char *exts[] = { "VK_KHR_surface", "VK_KHR_win32_surface" };
      VkInstanceCI ci; memset(&ci,0,sizeof(ci)); ci.sType=STYPE_INSTANCE_CI; ci.extCount=2; ci.ppExts=exts;
      CHK(pfn_vkCreateInstance(&ci, NULL, &inst), "vkCreateInstance"); }
    P("[vk] Instance created");

    /* 3. Physical device + memory properties */
    { uint32_t n=1; CHK(pfn_vkEnumeratePhysicalDevices(inst,&n,&gpu),"EnumPhysDev");
      uint32_t qfc=0; pfn_vkGetPhysicalDeviceQueueFamilyProperties(gpu,&qfc,NULL);
      VkQueueFamilyProps qfp[16]; if(qfc>16)qfc=16;
      pfn_vkGetPhysicalDeviceQueueFamilyProperties(gpu,&qfc,qfp);
      for(uint32_t i=0;i<qfc;i++) if(qfp[i].queueFlags&VK_QUEUE_GRAPHICS_BIT){gfxQF=i;break;}
      pfn_vkGetPhysicalDeviceMemoryProperties(gpu, &memProps); }

    /* 4. Device */
    { float p=1.0f; VkDeviceQueueCI qci; memset(&qci,0,sizeof(qci));
      qci.sType=STYPE_DEVICE_QUEUE_CI; qci.queueFamilyIndex=gfxQF; qci.queueCount=1; qci.pPriorities=&p;
      const char *de[]={"VK_KHR_swapchain"}; VkDeviceCI dci; memset(&dci,0,sizeof(dci));
      dci.sType=STYPE_DEVICE_CI; dci.queueCICount=1; dci.pQueueCIs=&qci; dci.extCount=1; dci.ppExts=de;
      CHK(pfn_vkCreateDevice(gpu,&dci,NULL,&dev),"vkCreateDevice"); }

    /* 5. Load device functions */
    DLOAD(vkDestroyDevice); DLOAD(vkGetDeviceQueue);
    DLOAD(vkCreateSwapchainKHR); DLOAD(vkDestroySwapchainKHR);
    DLOAD(vkGetSwapchainImagesKHR); DLOAD(vkAcquireNextImageKHR);
    DLOAD(vkQueuePresentKHR); DLOAD(vkQueueSubmit); DLOAD(vkDeviceWaitIdle);
    DLOAD(vkCreateImageView); DLOAD(vkDestroyImageView);
    DLOAD(vkCreateRenderPass); DLOAD(vkDestroyRenderPass);
    DLOAD(vkCreateShaderModule); DLOAD(vkDestroyShaderModule);
    DLOAD(vkCreatePipelineLayout); DLOAD(vkDestroyPipelineLayout);
    DLOAD(vkCreateGraphicsPipelines); DLOAD(vkDestroyPipeline);
    DLOAD(vkCreateFramebuffer); DLOAD(vkDestroyFramebuffer);
    DLOAD(vkCreateCommandPool); DLOAD(vkDestroyCommandPool);
    DLOAD(vkAllocateCommandBuffers); DLOAD(vkBeginCommandBuffer);
    DLOAD(vkEndCommandBuffer); DLOAD(vkResetCommandBuffer);
    DLOAD(vkCmdBeginRenderPass); DLOAD(vkCmdEndRenderPass);
    DLOAD(vkCmdBindPipeline); DLOAD(vkCmdDraw);
    DLOAD(vkCreateSemaphore); DLOAD(vkDestroySemaphore);
    DLOAD(vkCreateFence); DLOAD(vkDestroyFence);
    DLOAD(vkWaitForFences); DLOAD(vkResetFences);
    DLOAD(vkCreateBuffer); DLOAD(vkDestroyBuffer); DLOAD(vkGetBufferMemoryRequirements);
    DLOAD(vkAllocateMemory); DLOAD(vkFreeMemory); DLOAD(vkBindBufferMemory);
    DLOAD(vkMapMemory); DLOAD(vkUnmapMemory);
    DLOAD(vkCmdPushConstants); DLOAD(vkCmdBindVertexBuffers);

    pfn_vkGetDeviceQueue(dev, gfxQF, 0, &queue);
    P("[vk] Device + queue ready");

    /* 6. Window + surface */
    hwnd = CreateWindowExA(0,"STATIC","vkcube_wine",WS_OVERLAPPEDWINDOW,0,0,swapW,swapH,NULL,NULL,GetModuleHandleA(NULL),NULL);
    if (!hwnd) hwnd = GetDesktopWindow();
    if (!hwnd) FAIL("No window");
    { VkWin32SurfaceCI sci; memset(&sci,0,sizeof(sci)); sci.sType=STYPE_WIN32_SURFACE_CI;
      sci.hinstance=GetModuleHandleA(NULL); sci.hwnd=hwnd;
      CHK(pfn_vkCreateWin32SurfaceKHR(inst,&sci,NULL,&surface),"CreateSurface"); }

    /* 7. Swapchain */
    { VkSurfaceCaps caps; memset(&caps,0,sizeof(caps));
      pfn_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu,surface,&caps);
      if(caps.curW!=0xFFFFFFFF&&caps.curW>0){swapW=caps.curW;swapH=caps.curH;}
      uint32_t ic=caps.minImgCount<2?2:caps.minImgCount;
      if(caps.maxImgCount>0&&ic>caps.maxImgCount)ic=caps.maxImgCount;
      uint32_t fc=0; pfn_vkGetPhysicalDeviceSurfaceFormatsKHR(gpu,surface,&fc,NULL);
      VkSurfaceFormat sf[16]; if(fc>16)fc=16;
      pfn_vkGetPhysicalDeviceSurfaceFormatsKHR(gpu,surface,&fc,sf);
      VkSwapchainCI sci; memset(&sci,0,sizeof(sci)); sci.sType=STYPE_SWAPCHAIN_CI;
      sci.surface=surface; sci.minImgCount=ic;
      sci.imageFormat=fc>0?sf[0].format:VK_FORMAT_B8G8R8A8_UNORM;
      sci.imageColorSpace=fc>0?sf[0].colorSpace:0;
      sci.extW=swapW; sci.extH=swapH; sci.arrayLayers=1;
      sci.imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT|VK_IMAGE_USAGE_TRANSFER_SRC;
      sci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
      sci.preTransform=caps.currentTransform?caps.currentTransform:VK_SURFACE_TRANSFORM_IDENTITY;
      sci.compositeAlpha=(caps.supportedComposite&VK_COMPOSITE_ALPHA_OPAQUE)?VK_COMPOSITE_ALPHA_OPAQUE:VK_COMPOSITE_ALPHA_INHERIT;
      sci.presentMode=VK_PRESENT_MODE_FIFO; sci.clipped=1;
      CHK(pfn_vkCreateSwapchainKHR(dev,&sci,NULL,&swapchain),"CreateSwapchain");
      swapCount=MAX_SWAP; pfn_vkGetSwapchainImagesKHR(dev,swapchain,&swapCount,swapImages);
      P("[vk] Swapchain %ux%u (%u images)", swapW, swapH, swapCount); }

    /* 8. Image views */
    for(uint32_t i=0;i<swapCount;i++){
      VkImageViewCI v; memset(&v,0,sizeof(v)); v.sType=STYPE_IMAGE_VIEW_CI;
      v.image=swapImages[i]; v.viewType=VK_IMAGE_VIEW_TYPE_2D; v.format=VK_FORMAT_B8G8R8A8_UNORM;
      v.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; v.mipCount=1; v.layerCount=1;
      CHK(pfn_vkCreateImageView(dev,&v,NULL,&swapViews[i]),"CreateImageView"); }

    /* 9. Render pass */
    { VkAttachmentDesc att={0,VK_FORMAT_B8G8R8A8_UNORM,VK_SAMPLE_COUNT_1_BIT,
        VK_ATTACHMENT_LOAD_OP_CLEAR,VK_ATTACHMENT_STORE_OP_STORE,
        VK_ATTACHMENT_LOAD_OP_DONT_CARE,VK_ATTACHMENT_STORE_OP_DONT_CARE,
        VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_PRESENT_SRC};
      VkAttachmentRef ref={0,VK_IMAGE_LAYOUT_COLOR_ATTACH_OPT};
      VkSubpassDesc sub; memset(&sub,0,sizeof(sub)); sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
      sub.colorCount=1; sub.pColors=&ref;
      VkSubpassDep dep; memset(&dep,0,sizeof(dep)); dep.srcSubpass=0xFFFFFFFF; dep.dstSubpass=0;
      dep.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACH_OUT; dep.dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACH_OUT;
      dep.dstAccessMask=VK_ACCESS_COLOR_ATTACH_WRITE;
      VkRenderPassCI rp; memset(&rp,0,sizeof(rp)); rp.sType=STYPE_RENDER_PASS_CI;
      rp.attachCount=1; rp.pAttachments=&att; rp.subpassCount=1; rp.pSubpasses=&sub;
      rp.depCount=1; rp.pDeps=&dep;
      CHK(pfn_vkCreateRenderPass(dev,&rp,NULL,&renderPass),"CreateRenderPass"); }

    /* 10. Vertex buffer */
    { VkBufferCI bci; memset(&bci,0,sizeof(bci)); bci.sType=STYPE_BUFFER_CI;
      bci.size=sizeof(cube_verts); bci.usage=VK_BUFFER_USAGE_VERTEX_BUFFER;
      bci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
      CHK(pfn_vkCreateBuffer(dev,&bci,NULL,&vtxBuf),"CreateVtxBuf");
      VkMemReqs mr; pfn_vkGetBufferMemoryRequirements(dev,vtxBuf,&mr);
      uint32_t mt=find_mem_type(&memProps,mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE|VK_MEMORY_PROPERTY_HOST_COHERENT);
      if(mt==0xFFFFFFFF) FAIL("No host-visible memory for vertex buffer");
      VkMemAllocInfo mai; memset(&mai,0,sizeof(mai)); mai.sType=STYPE_MEMORY_ALLOC_INFO;
      mai.allocationSize=mr.size; mai.memoryTypeIndex=mt;
      CHK(pfn_vkAllocateMemory(dev,&mai,NULL,&vtxMem),"AllocVtxMem");
      CHK(pfn_vkBindBufferMemory(dev,vtxBuf,vtxMem,0),"BindVtxMem");
      void *mapped; CHK(pfn_vkMapMemory(dev,vtxMem,0,sizeof(cube_verts),0,&mapped),"MapVtxMem");
      memcpy(mapped, cube_verts, sizeof(cube_verts));
      pfn_vkUnmapMemory(dev,vtxMem);
      P("[vk] Vertex buffer created (%u bytes, %u vertices)", (uint32_t)sizeof(cube_verts), 36); }

    /* 11. Shaders + pipeline layout with push constants */
    { VkShaderModuleCI sm; memset(&sm,0,sizeof(sm)); sm.sType=STYPE_SHADER_MODULE_CI;
      sm.codeSize=sizeof(vert_spv); sm.pCode=(const uint32_t*)vert_spv;
      CHK(pfn_vkCreateShaderModule(dev,&sm,NULL,&vertMod),"CreateShader(vert)");
      sm.codeSize=sizeof(frag_spv); sm.pCode=(const uint32_t*)frag_spv;
      CHK(pfn_vkCreateShaderModule(dev,&sm,NULL,&fragMod),"CreateShader(frag)");
      VkPushConstantRange pcr={VK_SHADER_STAGE_VERTEX_BIT, 0, 64}; /* mat4 = 64 bytes */
      VkPipelineLayoutCI pl; memset(&pl,0,sizeof(pl)); pl.sType=STYPE_PIPELINE_LAYOUT_CI;
      pl.pushConstRangeCount=1; pl.pPushConstRanges=&pcr;
      CHK(pfn_vkCreatePipelineLayout(dev,&pl,NULL,&pipeLayout),"CreatePipelineLayout"); }

    /* 12. Graphics pipeline — vertex input, back-face culling */
    { VkPipelineShaderStageCI stages[2]; memset(stages,0,sizeof(stages));
      stages[0].sType=STYPE_PIPELINE_SHADER_STAGE_CI; stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;
      stages[0].module=vertMod; stages[0].pName="main";
      stages[1].sType=STYPE_PIPELINE_SHADER_STAGE_CI; stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;
      stages[1].module=fragMod; stages[1].pName="main";
      VkVIBindingDesc bind={0, 6*sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX};
      VkVIAttrDesc attrs[2]={{0,0,VK_FORMAT_R32G32B32_SFLOAT,0},{1,0,VK_FORMAT_R32G32B32_SFLOAT,3*sizeof(float)}};
      VkPipelineVertexInputCI vi; memset(&vi,0,sizeof(vi)); vi.sType=STYPE_PIPELINE_VERTEX_INPUT_CI;
      vi.bindingCount=1; vi.pBindings=&bind; vi.attrCount=2; vi.pAttrs=attrs;
      VkPipelineInputAsmCI ia; memset(&ia,0,sizeof(ia)); ia.sType=STYPE_PIPELINE_INPUT_ASM_CI;
      ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      VkViewport vp={0,0,(float)swapW,(float)swapH,0,1};
      VkRect2D sc={0,0,swapW,swapH};
      VkPipelineViewportCI vps; memset(&vps,0,sizeof(vps)); vps.sType=STYPE_PIPELINE_VIEWPORT_CI;
      vps.viewportCount=1; vps.pViewports=&vp; vps.scissorCount=1; vps.pScissors=&sc;
      VkPipelineRasterCI rs; memset(&rs,0,sizeof(rs)); rs.sType=STYPE_PIPELINE_RASTER_CI;
      rs.polygonMode=VK_POLYGON_MODE_FILL; rs.cullMode=VK_CULL_MODE_BACK;
      rs.frontFace=VK_FRONT_FACE_CCW; rs.lineWidth=1.0f;
      VkPipelineMultisampleCI ms; memset(&ms,0,sizeof(ms)); ms.sType=STYPE_PIPELINE_MULTISAMPLE_CI;
      ms.rasterSamples=VK_SAMPLE_COUNT_1_BIT;
      VkPipelineColorBlendAttach cba; memset(&cba,0,sizeof(cba)); cba.colorWriteMask=VK_COLOR_COMPONENT_RGBA;
      VkPipelineColorBlendCI cb; memset(&cb,0,sizeof(cb)); cb.sType=STYPE_PIPELINE_COLORBLEND_CI;
      cb.attachCount=1; cb.pAttachments=&cba;
      VkGraphicsPipelineCI gp; memset(&gp,0,sizeof(gp)); gp.sType=STYPE_GRAPHICS_PIPELINE_CI;
      gp.stageCount=2; gp.pStages=stages; gp.pVertexInput=&vi; gp.pInputAsm=&ia;
      gp.pViewport=&vps; gp.pRaster=&rs; gp.pMultisample=&ms; gp.pColorBlend=&cb;
      gp.layout=pipeLayout; gp.renderPass=renderPass; gp.basePipelineIndex=-1;
      CHK(pfn_vkCreateGraphicsPipelines(dev,VK_NULL_HANDLE,1,&gp,NULL,&pipeline),"CreatePipeline"); }
    P("[vk] Pipeline created (vertex buffer + push constants + backface culling)");

    /* 13. Framebuffers */
    for(uint32_t i=0;i<swapCount;i++){
      VkFramebufferCI fb; memset(&fb,0,sizeof(fb)); fb.sType=STYPE_FRAMEBUFFER_CI;
      fb.renderPass=renderPass; fb.attachCount=1; fb.pAttachments=&swapViews[i];
      fb.width=swapW; fb.height=swapH; fb.layers=1;
      CHK(pfn_vkCreateFramebuffer(dev,&fb,NULL,&swapFBs[i]),"CreateFB"); }

    /* 14. Command pool + buffers */
    { VkCommandPoolCI cp; memset(&cp,0,sizeof(cp)); cp.sType=STYPE_CMD_POOL_CI;
      cp.flags=VK_CMD_POOL_RESET_BIT; cp.queueFamilyIndex=gfxQF;
      CHK(pfn_vkCreateCommandPool(dev,&cp,NULL,&cmdPool),"CreateCmdPool");
      VkCommandBufferAI ca; memset(&ca,0,sizeof(ca)); ca.sType=STYPE_CMD_BUF_AI;
      ca.commandPool=cmdPool; ca.level=VK_CMD_BUF_LEVEL_PRIMARY; ca.count=swapCount;
      CHK(pfn_vkAllocateCommandBuffers(dev,&ca,cmdBufs),"AllocCmdBufs"); }

    /* 15. Sync objects */
    { VkFenceCI fc; memset(&fc,0,sizeof(fc)); fc.sType=STYPE_FENCE_CI; fc.flags=VK_FENCE_CREATE_SIGNALED;
      CHK(pfn_vkCreateFence(dev,&fc,NULL,&fence),"CreateFence"); }

    /* Pre-compute view and projection matrices */
    float proj[16], view[16], pv[16];
    mat4_perspective(proj, 45.0f*PI/180.0f, (float)swapW/(float)swapH, 0.1f, 100.0f);
    mat4_lookAt(view, 1.5f, 1.5f, 3.0f,  0,0,0,  0,1,0);
    mat4_mul(pv, proj, view);

    P("[vk] === Rendering %d frames (spinning cube) ===", num_frames);

    /* ===== RENDER LOOP ===== */
    for (int frame = 0; frame < num_frames; frame++) {
        uint32_t imgIdx = 0;
        VkResult acq = pfn_vkAcquireNextImageKHR(dev, swapchain, 5000000000ULL,
            VK_NULL_HANDLE, VK_NULL_HANDLE, &imgIdx);
        if (acq != VK_SUCCESS && acq != 1000001003) { P("[vk] Acquire failed: %d", acq); break; }

        /* Compute MVP: proj * view * rotY(angle) */
        float angle = (float)frame / (float)num_frames * 4.0f * PI; /* 2 full rotations */
        float rotY[16], mvp[16];
        mat4_rotateY(rotY, angle);
        mat4_mul(mvp, pv, rotY);

        VkCommandBuffer cmd = cmdBufs[imgIdx];
        pfn_vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBI bi; memset(&bi,0,sizeof(bi)); bi.sType=STYPE_CMD_BUF_BEGIN; bi.flags=VK_CMD_BUF_USAGE_ONE_TIME;
        pfn_vkBeginCommandBuffer(cmd, &bi);

        /* Dark blue-gray background */
        VkClearValue cv; cv.color[0]=0.1f; cv.color[1]=0.1f; cv.color[2]=0.15f; cv.color[3]=1.0f;

        VkRenderPassBI rpbi; memset(&rpbi,0,sizeof(rpbi)); rpbi.sType=STYPE_RENDER_PASS_BEGIN;
        rpbi.renderPass=renderPass; rpbi.framebuffer=swapFBs[imgIdx];
        rpbi.renderArea.w=swapW; rpbi.renderArea.h=swapH;
        rpbi.clearValueCount=1; rpbi.pClearValues=&cv;
        pfn_vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        pfn_vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        pfn_vkCmdPushConstants(cmd, pipeLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, mvp);
        { VkDeviceSize off=0; pfn_vkCmdBindVertexBuffers(cmd, 0, 1, &vtxBuf, &off); }
        pfn_vkCmdDraw(cmd, 36, 1, 0, 0);
        pfn_vkCmdEndRenderPass(cmd);
        pfn_vkEndCommandBuffer(cmd);

        pfn_vkResetFences(dev, 1, &fence);
        VkSubmitInfo si; memset(&si,0,sizeof(si)); si.sType=STYPE_SUBMIT_INFO;
        si.cmdBufCount=1; si.pCmdBufs=&cmd;
        VkResult sub = pfn_vkQueueSubmit(queue, 1, &si, fence);
        if (sub != VK_SUCCESS) { P("[vk] Submit failed: %d", sub); break; }

        pfn_vkWaitForFences(dev, 1, &fence, 1, 5000000000ULL);

        VkPresentInfo pi; memset(&pi,0,sizeof(pi)); pi.sType=STYPE_PRESENT_INFO;
        pi.swapchainCount=1; pi.pSwapchains=&swapchain; pi.pImageIndices=&imgIdx;
        VkResult pres = pfn_vkQueuePresentKHR(queue, &pi);

        if (frame < 3 || frame % 50 == 0)
            P("[vk] Frame %d/%d: acq=%d sub=%d pres=%d angle=%.1f°",
                frame+1, num_frames, acq, sub, pres, angle*180.0f/PI);

        if (pres != VK_SUCCESS && pres != 1000001003) { P("[vk] Present failed: %d", pres); break; }
    }

    P("[vk] === DONE — %d frames rendered ===", num_frames);
    fflush(stderr); fflush(stdout);
    ExitProcess(0);
}
