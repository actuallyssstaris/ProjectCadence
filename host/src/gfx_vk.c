/*
 * Vulkan renderer on PS5_Vulkan's RADV. Draws everything at 1080p into an
 * offscreen target and blits it up to the 4K VideoOut swapchain, since the
 * only swapchain usage the winsys is known to handle is transfer.
 * Display setup follows radv/radv_smoke.c from PS5_Vulkan.
 */
#include "gfx.h"
#include "log.h"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shape_frag.h"
#include "shape_vert.h"

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);

#define INSTANCE_FUNCS(X)                     \
    X(EnumeratePhysicalDevices)               \
    X(GetPhysicalDeviceQueueFamilyProperties) \
    X(GetPhysicalDeviceMemoryProperties)      \
    X(CreateDevice)                           \
    X(GetDeviceProcAddr)                      \
    X(GetPhysicalDeviceDisplayPropertiesKHR)  \
    X(GetDisplayModePropertiesKHR)            \
    X(CreateDisplayPlaneSurfaceKHR)           \
    X(GetPhysicalDeviceSurfaceCapabilitiesKHR)

#define DEVICE_FUNCS(X)          \
    X(GetDeviceQueue)            \
    X(CreateSwapchainKHR)        \
    X(GetSwapchainImagesKHR)     \
    X(AcquireNextImageKHR)       \
    X(QueuePresentKHR)           \
    X(CreateSemaphore)           \
    X(CreateFence)               \
    X(WaitForFences)             \
    X(ResetFences)               \
    X(CreateCommandPool)         \
    X(AllocateCommandBuffers)    \
    X(BeginCommandBuffer)        \
    X(EndCommandBuffer)          \
    X(ResetCommandBuffer)        \
    X(QueueSubmit)               \
    X(QueueWaitIdle)             \
    X(DeviceWaitIdle)            \
    X(CreateImage)               \
    X(DestroyImage)              \
    X(GetImageMemoryRequirements) \
    X(AllocateMemory)            \
    X(FreeMemory)                \
    X(BindImageMemory)           \
    X(CreateImageView)           \
    X(DestroyImageView)          \
    X(CreateBuffer)              \
    X(DestroyBuffer)             \
    X(GetBufferMemoryRequirements) \
    X(BindBufferMemory)          \
    X(MapMemory)                 \
    X(CreateShaderModule)        \
    X(DestroyShaderModule)       \
    X(CreatePipelineLayout)      \
    X(CreateGraphicsPipelines)   \
    X(CreateDescriptorSetLayout) \
    X(CreateDescriptorPool)      \
    X(AllocateDescriptorSets)    \
    X(FreeDescriptorSets)        \
    X(UpdateDescriptorSets)      \
    X(CreateSampler)             \
    X(CmdPipelineBarrier2)       \
    X(CmdBeginRendering)         \
    X(CmdEndRendering)           \
    X(CmdBindPipeline)           \
    X(CmdBindVertexBuffers)      \
    X(CmdBindDescriptorSets)     \
    X(CmdPushConstants)          \
    X(CmdSetViewport)            \
    X(CmdSetScissor)             \
    X(CmdDraw)                   \
    X(CmdBlitImage)              \
    X(CmdCopyBufferToImage)

static PFN_vkCreateInstance vkCreateInstance;
#define DECL(n) static PFN_vk##n vk##n;
INSTANCE_FUNCS(DECL)
DEVICE_FUNCS(DECL)
#undef DECL

#define MAX_INST 65536
#define MAX_DRAWS 1024
#define MAX_TEX 1024 /* storyboards can bring a few hundred images */

typedef struct {
    VkImage image;
    VkDeviceMemory mem;
    VkImageView view;
    VkDescriptorSet set;
    bool used;
} texture;

typedef struct {
    int tex;
    uint32_t first, count;
} draw_range;

static struct {
    VkInstance instance;
    VkPhysicalDevice phys;
    VkPhysicalDeviceMemoryProperties memprops;
    uint32_t family;
    VkDevice dev;
    VkQueue queue;

    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkImage sc_images[8];
    uint32_t sc_count;
    VkExtent2D sc_extent;

    VkImage target;
    VkDeviceMemory target_mem;
    VkImageView target_view;
    VkImage depth;
    VkDeviceMemory depth_mem;
    VkImageView depth_view;

    VkCommandPool pool;
    VkCommandBuffer cmd, upload_cmd;
    VkFence fence;
    VkSemaphore acquired;
    VkSemaphore rendered[8]; /* one per swapchain image, present may still be waiting on the last one */

    VkDescriptorSetLayout set_layout;
    VkDescriptorPool desc_pool;
    VkSampler sampler;
    VkPipelineLayout layout;
    VkPipeline pipeline;

    VkBuffer inst_buf;
    VkDeviceMemory inst_mem;
    gfx_inst *inst;
    uint32_t inst_count;

    draw_range draws[MAX_DRAWS];
    int draw_count;
    int bound_tex;

    texture tex[MAX_TEX];
    float z;
    bool frame_open;
} g;

static void fail(const char *what, VkResult r)
{
    LOG("gfx: %s failed (%d)", what, (int)r);
}

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < g.memprops.memoryTypeCount; i++) {
        if ((bits & (1u << i)) && (g.memprops.memoryTypes[i].propertyFlags & want) == want)
            return i;
    }
    return UINT32_MAX;
}

static bool alloc_image(VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                        VkImage *img, VkDeviceMemory *mem, VkImageView *view)
{
    VkImageCreateInfo ii = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt,
        .extent = {w, h, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult r = vkCreateImage(g.dev, &ii, NULL, img);
    if (r != VK_SUCCESS) {
        fail("vkCreateImage", r);
        return false;
    }

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g.dev, *img, &req);
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    if (ai.memoryTypeIndex == UINT32_MAX)
        ai.memoryTypeIndex = mem_type(req.memoryTypeBits, 0);
    if ((r = vkAllocateMemory(g.dev, &ai, NULL, mem)) != VK_SUCCESS) {
        fail("image memory", r);
        return false;
    }
    vkBindImageMemory(g.dev, *img, *mem, 0);

    VkImageViewCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = *img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = fmt,
        .subresourceRange = {aspect, 0, 1, 0, 1},
    };
    return vkCreateImageView(g.dev, &vi, NULL, view) == VK_SUCCESS;
}

static bool alloc_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buf, VkDeviceMemory *mem, void **map)
{
    VkBufferCreateInfo bi = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(g.dev, &bi, NULL, buf) != VK_SUCCESS)
        return false;

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g.dev, *buf, &req);
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(g.dev, &ai, NULL, mem) != VK_SUCCESS)
        return false;
    vkBindBufferMemory(g.dev, *buf, *mem, 0);
    return vkMapMemory(g.dev, *mem, 0, VK_WHOLE_SIZE, 0, map) == VK_SUCCESS;
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                    VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da)
{
    VkImageMemoryBarrier2 b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = ss,
        .srcAccessMask = sa,
        .dstStageMask = ds,
        .dstAccessMask = da,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = {aspect, 0, 1, 0, 1},
    };
    VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &dep);
}

static bool create_device(void)
{
    vkCreateInstance = (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    if (!vkCreateInstance)
        return false;

    const char *exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME};
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "ProjectCadence",
        .apiVersion = VK_API_VERSION_1_3,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = exts,
    };
    VkResult r = vkCreateInstance(&ici, NULL, &g.instance);
    if (r != VK_SUCCESS) {
        fail("vkCreateInstance", r);
        return false;
    }

    bool ok = true;
#define LOAD(n) ok &= (vk##n = (PFN_vk##n)vk_icdGetInstanceProcAddr(g.instance, "vk" #n)) != NULL;
    INSTANCE_FUNCS(LOAD)
#undef LOAD
    if (!ok)
        return false;

    uint32_t n = 1;
    vkEnumeratePhysicalDevices(g.instance, &n, &g.phys);
    if (n == 0)
        return false;
    vkGetPhysicalDeviceMemoryProperties(g.phys, &g.memprops);

    VkQueueFamilyProperties fam[8];
    n = 8;
    vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &n, fam);
    g.family = UINT32_MAX;
    for (uint32_t i = 0; i < n; i++) {
        if (fam[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            g.family = i;
            break;
        }
    }
    if (g.family == UINT32_MAX)
        return false;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = g.family,
        .queueCount = 1,
        .pQueuePriorities = &prio,
    };
    VkPhysicalDeviceVulkan13Features f13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .synchronization2 = VK_TRUE,
        .dynamicRendering = VK_TRUE,
    };
    const char *dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &f13,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qi,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = dev_exts,
    };
    if ((r = vkCreateDevice(g.phys, &dci, NULL, &g.dev)) != VK_SUCCESS) {
        fail("vkCreateDevice", r);
        return false;
    }

#define LOAD(n) ok &= (vk##n = (PFN_vk##n)vkGetDeviceProcAddr(g.dev, "vk" #n)) != NULL;
    DEVICE_FUNCS(LOAD)
#undef LOAD
    if (!ok) {
        LOG("gfx: missing device entry points");
        return false;
    }

    vkGetDeviceQueue(g.dev, g.family, 0, &g.queue);
    return true;
}

static bool create_swapchain(void)
{
    VkDisplayPropertiesKHR disp;
    uint32_t n = 1;
    if (vkGetPhysicalDeviceDisplayPropertiesKHR(g.phys, &n, &disp) < 0 || n == 0)
        return false;

    VkDisplayModePropertiesKHR modes[4];
    n = 4;
    if (vkGetDisplayModePropertiesKHR(g.phys, disp.display, &n, modes) < 0 || n == 0)
        return false;

    g.sc_extent = disp.physicalResolution;

    VkDisplaySurfaceCreateInfoKHR si = {
        .sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
        .displayMode = modes[0].displayMode,
        .transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
        .imageExtent = g.sc_extent,
    };
    if (vkCreateDisplayPlaneSurfaceKHR(g.instance, &si, NULL, &g.surface) != VK_SUCCESS)
        return false;

    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.phys, g.surface, &caps);
    if (caps.currentExtent.width != UINT32_MAX)
        g.sc_extent = caps.currentExtent;

    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = g.surface,
        .minImageCount = 3,
        .imageFormat = VK_FORMAT_B8G8R8A8_UNORM,
        .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent = g.sc_extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
    };
    VkResult r = vkCreateSwapchainKHR(g.dev, &ci, NULL, &g.swapchain);
    if (r != VK_SUCCESS) {
        fail("vkCreateSwapchainKHR", r);
        return false;
    }
    g.sc_count = 8;
    vkGetSwapchainImagesKHR(g.dev, g.swapchain, &g.sc_count, g.sc_images);
    return true;
}

static VkShaderModule shader(const uint32_t *code, size_t size)
{
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code};
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(g.dev, &ci, NULL, &m);
    return m;
}

static bool create_pipeline(void)
{
    VkDescriptorSetLayoutBinding b = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    VkDescriptorSetLayoutCreateInfo dl = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
    vkCreateDescriptorSetLayout(g.dev, &dl, NULL, &g.set_layout);

    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_TEX};
    VkDescriptorPoolCreateInfo dp = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = MAX_TEX,
        .poolSizeCount = 1,
        .pPoolSizes = &ps,
    };
    vkCreateDescriptorPool(g.dev, &dp, NULL, &g.desc_pool);

    VkSamplerCreateInfo sc = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = 0,
    };
    vkCreateSampler(g.dev, &sc, NULL, &g.sampler);

    VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float) * 2};
    VkPipelineLayoutCreateInfo pl = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &g.set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pc,
    };
    vkCreatePipelineLayout(g.dev, &pl, NULL, &g.layout);

    VkShaderModule vs = shader(shape_vert, sizeof(shape_vert));
    VkShaderModule fs = shader(shape_frag, sizeof(shape_frag));
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"},
    };

    VkVertexInputBindingDescription bind = {0, sizeof(gfx_inst), VK_VERTEX_INPUT_RATE_INSTANCE};
    VkVertexInputAttributeDescription attrs[4] = {
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(gfx_inst, x)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(gfx_inst, rot)},
        {2, 0, VK_FORMAT_R32G32_UINT, offsetof(gfx_inst, colour)},
        {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(gfx_inst, p)},
    };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &bind,
        .vertexAttributeDescriptionCount = 4,
        .pVertexAttributeDescriptions = attrs,
    };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_LESS,
    };
    VkPipelineColorBlendAttachmentState blend = {
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = 0xF,
    };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blend,
    };
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};

    VkFormat colour_fmt = VK_FORMAT_B8G8R8A8_UNORM;
    VkPipelineRenderingCreateInfo ri = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &colour_fmt,
        .depthAttachmentFormat = VK_FORMAT_D32_SFLOAT,
    };
    VkGraphicsPipelineCreateInfo gp = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &ri,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vi,
        .pInputAssemblyState = &ia,
        .pViewportState = &vp,
        .pRasterizationState = &rs,
        .pMultisampleState = &ms,
        .pDepthStencilState = &ds,
        .pColorBlendState = &cb,
        .pDynamicState = &dy,
        .layout = g.layout,
    };
    VkResult r = vkCreateGraphicsPipelines(g.dev, VK_NULL_HANDLE, 1, &gp, NULL, &g.pipeline);
    vkDestroyShaderModule(g.dev, vs, NULL);
    vkDestroyShaderModule(g.dev, fs, NULL);
    if (r != VK_SUCCESS) {
        fail("vkCreateGraphicsPipelines", r);
        return false;
    }
    return true;
}

bool gfx_init(void)
{
    memset(&g, 0, sizeof(g));

    if (!create_device())
        return false;
    if (!create_swapchain()) {
        LOG("gfx: no display");
        return false;
    }

    if (!alloc_image(VK_FORMAT_B8G8R8A8_UNORM, GFX_W, GFX_H,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                     &g.target, &g.target_mem, &g.target_view))
        return false;
    if (!alloc_image(VK_FORMAT_D32_SFLOAT, GFX_W, GFX_H, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
                     &g.depth, &g.depth_mem, &g.depth_view))
        return false;

    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = g.family,
    };
    vkCreateCommandPool(g.dev, &pci, NULL, &g.pool);
    VkCommandBuffer cbs[2];
    VkCommandBufferAllocateInfo cai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = g.pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 2,
    };
    vkAllocateCommandBuffers(g.dev, &cai, cbs);
    g.cmd = cbs[0];
    g.upload_cmd = cbs[1];

    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    vkCreateFence(g.dev, &fi, NULL, &g.fence);
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(g.dev, &si, NULL, &g.acquired);
    for (uint32_t i = 0; i < g.sc_count; i++)
        vkCreateSemaphore(g.dev, &si, NULL, &g.rendered[i]);

    if (!create_pipeline())
        return false;

    void *map;
    if (!alloc_buffer(sizeof(gfx_inst) * MAX_INST, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &g.inst_buf, &g.inst_mem, &map))
        return false;
    g.inst = map;

    uint32_t white = 0xFFFFFFFF;
    gfx_texture(1, 1, &white); /* slot 0 */
    return true;
}

static int upload(int w, int h, VkFormat fmt, int bpp, const void *pixels)
{
    int slot = -1;
    for (int i = 0; i < MAX_TEX; i++) {
        if (!g.tex[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return 0;

    texture *t = &g.tex[slot];
    if (!alloc_image(fmt, (uint32_t)w, (uint32_t)h, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, &t->image, &t->mem, &t->view))
        return 0;

    VkBuffer staging;
    VkDeviceMemory smem;
    void *map;
    VkDeviceSize size = (VkDeviceSize)w * h * bpp;
    if (!alloc_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging, &smem, &map)) {
        vkDestroyImageView(g.dev, t->view, NULL);
        vkDestroyImage(g.dev, t->image, NULL);
        vkFreeMemory(g.dev, t->mem, NULL);
        memset(t, 0, sizeof(*t));
        return 0;
    }
    memcpy(map, pixels, size);

    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkResetCommandBuffer(g.upload_cmd, 0);
    vkBeginCommandBuffer(g.upload_cmd, &bi);
    barrier(g.upload_cmd, t->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy copy = {
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {(uint32_t)w, (uint32_t)h, 1},
    };
    vkCmdCopyBufferToImage(g.upload_cmd, staging, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier(g.upload_cmd, t->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    vkEndCommandBuffer(g.upload_cmd);

    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &g.upload_cmd};
    vkQueueSubmit(g.queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(g.queue);

    vkDestroyBuffer(g.dev, staging, NULL);
    vkFreeMemory(g.dev, smem, NULL);

    VkDescriptorSetAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = g.desc_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &g.set_layout,
    };
    vkAllocateDescriptorSets(g.dev, &ai, &t->set);
    VkDescriptorImageInfo ii = {g.sampler, t->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = t->set,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &ii,
    };
    vkUpdateDescriptorSets(g.dev, 1, &wr, 0, NULL);

    t->used = true;
    return slot;
}

int gfx_texture(int w, int h, const void *rgba) { return upload(w, h, VK_FORMAT_R8G8B8A8_UNORM, 4, rgba); }

int gfx_texture_r8(int w, int h, const void *r8) { return upload(w, h, VK_FORMAT_R8_UNORM, 1, r8); }

void gfx_texture_free(int id)
{
    if (id <= 0 || id >= MAX_TEX || !g.tex[id].used)
        return;
    vkDeviceWaitIdle(g.dev);
    texture *t = &g.tex[id];
    vkFreeDescriptorSets(g.dev, g.desc_pool, 1, &t->set);
    vkDestroyImageView(g.dev, t->view, NULL);
    vkDestroyImage(g.dev, t->image, NULL);
    vkFreeMemory(g.dev, t->mem, NULL);
    memset(t, 0, sizeof(*t));
}

void gfx_begin(float r, float gr, float b)
{
    vkWaitForFences(g.dev, 1, &g.fence, VK_TRUE, UINT64_MAX);

    g.inst_count = 0;
    g.draw_count = 0;
    g.bound_tex = -1;
    g.z = 0.9999f;
    g.frame_open = true;

    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkResetCommandBuffer(g.cmd, 0);
    vkBeginCommandBuffer(g.cmd, &bi);

    barrier(g.cmd, g.target, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    barrier(g.cmd, g.depth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo colour = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = g.target_view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue.color = {{r, gr, b, 1.0f}},
    };

    VkRenderingAttachmentInfo depth = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = g.depth_view,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .clearValue.depthStencil = {1.0f, 0},
    };
    VkRenderingInfo ri = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {{0, 0}, {GFX_W, GFX_H}},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &colour,
        .pDepthAttachment = &depth,
    };
    vkCmdBeginRendering(g.cmd, &ri);

    VkViewport vp = {0, 0, GFX_W, GFX_H, 0, 1};
    VkRect2D sc = {{0, 0}, {GFX_W, GFX_H}};
    vkCmdSetViewport(g.cmd, 0, 1, &vp);
    vkCmdSetScissor(g.cmd, 0, 1, &sc);
    vkCmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipeline);
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(g.cmd, 0, 1, &g.inst_buf, &off);
    float screen[2] = {GFX_W, GFX_H};
    vkCmdPushConstants(g.cmd, g.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(screen), screen);

    gfx_bind(0);
}

float gfx_next_z(void)
{
    g.z -= 1.0f / 131072.0f;
    if (g.z < 0.0001f)
        g.z = 0.0001f;
    return g.z;
}

void gfx_bind(int tex)
{
    if (tex < 0 || tex >= MAX_TEX || !g.tex[tex].used)
        tex = 0;
    if (tex == g.bound_tex)
        return;
    g.bound_tex = tex;
    if (g.draw_count < MAX_DRAWS)
        g.draws[g.draw_count++] = (draw_range){tex, g.inst_count, 0};
}

void gfx_push(const gfx_inst *in)
{
    /* out of draw ranges: drop rather than draw with the previous texture */
    if (g.inst_count >= MAX_INST || g.draw_count == 0 || g.draws[g.draw_count - 1].tex != g.bound_tex)
        return;
    g.inst[g.inst_count++] = *in;
    g.draws[g.draw_count - 1].count++;
}

void gfx_end(void)
{
    if (!g.frame_open)
        return;
    g.frame_open = false;

    for (int i = 0; i < g.draw_count; i++) {
        draw_range *d = &g.draws[i];
        if (d->count == 0)
            continue;
        vkCmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, 1, &g.tex[d->tex].set, 0, NULL);
        vkCmdDraw(g.cmd, 6, d->count, 0, d->first);
    }
    vkCmdEndRendering(g.cmd);

    uint32_t index = 0;
    VkResult r = vkAcquireNextImageKHR(g.dev, g.swapchain, UINT64_MAX, g.acquired, VK_NULL_HANDLE, &index);
    bool have_image = r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;

    barrier(g.cmd, g.target, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT);

    if (have_image) {
        VkImage dst = g.sc_images[index];
        barrier(g.cmd, dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageBlit blit = {
            .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .srcOffsets = {{0, 0, 0}, {GFX_W, GFX_H, 1}},
            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .dstOffsets = {{0, 0, 0}, {(int32_t)g.sc_extent.width, (int32_t)g.sc_extent.height, 1}},
        };
        vkCmdBlitImage(g.cmd, g.target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_LINEAR);
        barrier(g.cmd, dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0);
    }
    vkEndCommandBuffer(g.cmd);

    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = have_image ? 1 : 0,
        .pWaitSemaphores = &g.acquired,
        .pWaitDstStageMask = &wait,
        .commandBufferCount = 1,
        .pCommandBuffers = &g.cmd,
        .signalSemaphoreCount = have_image ? 1 : 0,
        .pSignalSemaphores = &g.rendered[index],
    };
    vkResetFences(g.dev, 1, &g.fence);
    vkQueueSubmit(g.queue, 1, &submit, g.fence);

    if (have_image) {
        VkPresentInfoKHR pi = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &g.rendered[index],
            .swapchainCount = 1,
            .pSwapchains = &g.swapchain,
            .pImageIndices = &index,
        };
        vkQueuePresentKHR(g.queue, &pi);
    }
}
