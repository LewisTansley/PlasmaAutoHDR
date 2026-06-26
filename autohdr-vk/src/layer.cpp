#include "config_state.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef AUTOHDR_VK_VERT_SPIRV_PATH
#define AUTOHDR_VK_VERT_SPIRV_PATH "fullscreen.vert.spv"
#endif
#ifndef AUTOHDR_VK_FRAG_SPIRV_PATH
#define AUTOHDR_VK_FRAG_SPIRV_PATH "autohdr.frag.spv"
#endif

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#ifndef VK_LAYER_EXPORT
#define VK_LAYER_EXPORT
#endif

namespace autohdr_vk_internal {

constexpr uint32_t kLutSize = AutoHdrCore::kToneCurveLutSize;

struct PushConstants {
    float blackPoint;
    float colorVibrance;
    float gamutExpansion;
    float referenceNits;
    float displayPeak;
    float toneCurveInputSpan;
    int32_t colorMode;
};

struct SwapchainState {
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    VkImageView inputView = VK_NULL_HANDLE;
    VkImage outputImage = VK_NULL_HANDLE;
    VkDeviceMemory outputMemory = VK_NULL_HANDLE;
    VkImageView outputView = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkBuffer lutBuffer = VK_NULL_HANDLE;
    VkDeviceMemory lutMemory = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    bool initialized = false;
};

struct DeviceData {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    uint32_t graphicsQueueFamily = 0;
    PFN_vkGetDeviceProcAddr pfnGetDeviceProcAddr = nullptr;
    PFN_vkQueuePresentKHR pfnQueuePresentKHR = nullptr;
    std::unordered_map<VkSwapchainKHR, SwapchainState> swapchains;
    std::mutex mutex;
};

struct InstanceData {
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr pfnGetInstanceProcAddr = nullptr;
    PFN_vkCreateDevice pfnCreateDevice = nullptr;
    PFN_vkDestroyDevice pfnDestroyDevice = nullptr;
};

autohdr_vk::ConfigState gConfigState;
std::mutex gInstanceMutex;
std::unordered_map<VkInstance, InstanceData> gInstances;
std::mutex gDeviceMutex;
std::unordered_map<VkDevice, DeviceData *> gDevices;

std::vector<uint32_t> gVertSpirv;
std::vector<uint32_t> gFragSpirv;

void logMessage(const char *message)
{
    if (std::getenv("AUTOHDR_VK_DEBUG")) {
        std::fprintf(stderr, "[autohdr-vk] %s\n", message);
    }
}

bool loadSpirvFile(const char *path, std::vector<uint32_t> &out)
{
    FILE *file = std::fopen(path, "rb");
    if (!file) {
        return false;
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size <= 0 || size % 4 != 0) {
        std::fclose(file);
        return false;
    }
    out.resize(static_cast<size_t>(size) / 4);
    const size_t read = std::fread(out.data(), 4, out.size(), file);
    std::fclose(file);
    return read == out.size();
}

void initShaderModules()
{
    if (!gVertSpirv.empty()) {
        return;
    }
    const char *vertPath = std::getenv("AUTOHDR_VK_VERT_SPIRV");
    const char *fragPath = std::getenv("AUTOHDR_VK_FRAG_SPIRV");
    if (!vertPath) {
        vertPath = AUTOHDR_VK_VERT_SPIRV_PATH;
    }
    if (!fragPath) {
        fragPath = AUTOHDR_VK_FRAG_SPIRV_PATH;
    }
    if (!loadSpirvFile(vertPath, gVertSpirv)) {
        logMessage("failed to load vertex shader SPIR-V");
    }
    if (!loadSpirvFile(fragPath, gFragSpirv)) {
        logMessage("failed to load fragment shader SPIR-V");
    }
}

VkShaderModule createShaderModule(VkDevice device, const std::vector<uint32_t> &code)
{
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size() * sizeof(uint32_t);
    info.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &info, nullptr, &module);
    return module;
}

void destroySwapchainResources(DeviceData *deviceData, SwapchainState &state)
{
    if (!deviceData || deviceData->device == VK_NULL_HANDLE) {
        return;
    }
    VkDevice device = deviceData->device;
    if (state.commandPool) {
        vkDestroyCommandPool(device, state.commandPool, nullptr);
    }
    if (state.sampler) {
        vkDestroySampler(device, state.sampler, nullptr);
    }
    if (state.lutBuffer) {
        vkDestroyBuffer(device, state.lutBuffer, nullptr);
    }
    if (state.lutMemory) {
        vkFreeMemory(device, state.lutMemory, nullptr);
    }
    if (state.pipeline) {
        vkDestroyPipeline(device, state.pipeline, nullptr);
    }
    if (state.pipelineLayout) {
        vkDestroyPipelineLayout(device, state.pipelineLayout, nullptr);
    }
    if (state.descriptorPool) {
        vkDestroyDescriptorPool(device, state.descriptorPool, nullptr);
    }
    if (state.descriptorSetLayout) {
        vkDestroyDescriptorSetLayout(device, state.descriptorSetLayout, nullptr);
    }
    if (state.framebuffer) {
        vkDestroyFramebuffer(device, state.framebuffer, nullptr);
    }
    if (state.renderPass) {
        vkDestroyRenderPass(device, state.renderPass, nullptr);
    }
    if (state.inputView) {
        vkDestroyImageView(device, state.inputView, nullptr);
    }
    if (state.outputView) {
        vkDestroyImageView(device, state.outputView, nullptr);
    }
    if (state.outputImage) {
        vkDestroyImage(device, state.outputImage, nullptr);
    }
    if (state.outputMemory) {
        vkFreeMemory(device, state.outputMemory, nullptr);
    }
    state = {};
}

uint32_t findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeBits, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return 0;
}

bool ensureSwapchainResources(DeviceData *deviceData, SwapchainState &state)
{
    if (state.initialized || gVertSpirv.empty() || gFragSpirv.empty()) {
        return state.initialized;
    }

    VkDevice device = deviceData->device;

    VkImageViewCreateInfo inputViewInfo{};
    inputViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    inputViewInfo.image = state.images.front();
    inputViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    inputViewInfo.format = state.format;
    inputViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    inputViewInfo.subresourceRange.levelCount = 1;
    inputViewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &inputViewInfo, nullptr, &state.inputView) != VK_SUCCESS) {
        return false;
    }

    VkImageCreateInfo imageCreateInfo{};
    imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
    imageCreateInfo.format = state.format;
    imageCreateInfo.extent = {state.extent.width, state.extent.height, 1};
    imageCreateInfo.mipLevels = 1;
    imageCreateInfo.arrayLayers = 1;
    imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageCreateInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &imageCreateInfo, nullptr, &state.outputImage) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, state.outputImage, &requirements);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = requirements.size;
    allocInfo.memoryTypeIndex =
        findMemoryType(deviceData->physicalDevice, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device, &allocInfo, nullptr, &state.outputMemory) != VK_SUCCESS) {
        return false;
    }
    vkBindImageMemory(device, state.outputImage, state.outputMemory, 0);

    VkImageViewCreateInfo outputViewInfo = inputViewInfo;
    outputViewInfo.image = state.outputImage;
    if (vkCreateImageView(device, &outputViewInfo, nullptr, &state.outputView) != VK_SUCCESS) {
        return false;
    }

    VkAttachmentDescription attachment{};
    attachment.format = state.format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &attachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    if (vkCreateRenderPass(device, &renderPassInfo, nullptr, &state.renderPass) != VK_SUCCESS) {
        return false;
    }

    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = state.renderPass;
    framebufferInfo.attachmentCount = 1;
    framebufferInfo.pAttachments = &state.outputView;
    framebufferInfo.width = state.extent.width;
    framebufferInfo.height = state.extent.height;
    framebufferInfo.layers = 1;
    if (vkCreateFramebuffer(device, &framebufferInfo, nullptr, &state.framebuffer) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &state.descriptorSetLayout) != VK_SUCCESS) {
        return false;
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &state.descriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &state.pipelineLayout) != VK_SUCCESS) {
        return false;
    }

    VkBufferCreateInfo lutInfo{};
    lutInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    lutInfo.size = kLutSize * sizeof(float);
    lutInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (vkCreateBuffer(device, &lutInfo, nullptr, &state.lutBuffer) != VK_SUCCESS) {
        return false;
    }
    vkGetBufferMemoryRequirements(device, state.lutBuffer, &requirements);
    allocInfo.allocationSize = requirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(deviceData->physicalDevice, requirements.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(device, &allocInfo, nullptr, &state.lutMemory) != VK_SUCCESS) {
        return false;
    }
    vkBindBufferMemory(device, state.lutBuffer, state.lutMemory, 0);

    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 1;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[1].descriptorCount = 1;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &state.descriptorPool) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorSetAllocateInfo setAllocInfo{};
    setAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setAllocInfo.descriptorPool = state.descriptorPool;
    setAllocInfo.descriptorSetCount = 1;
    setAllocInfo.pSetLayouts = &state.descriptorSetLayout;
    if (vkAllocateDescriptorSets(device, &setAllocInfo, &state.descriptorSet) != VK_SUCCESS) {
        return false;
    }

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device, &samplerInfo, nullptr, &state.sampler) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorImageInfo descriptorImageInfo{};
    descriptorImageInfo.sampler = state.sampler;
    descriptorImageInfo.imageView = state.inputView;
    descriptorImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = state.lutBuffer;
    bufferInfo.range = kLutSize * sizeof(float);
    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = state.descriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &descriptorImageInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = state.descriptorSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[1].pBufferInfo = &bufferInfo;
    vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

    VkShaderModule vertModule = createShaderModule(device, gVertSpirv);
    VkShaderModule fragModule = createShaderModule(device, gFragSpirv);
    if (!vertModule || !fragModule) {
        if (vertModule) {
            vkDestroyShaderModule(device, vertModule, nullptr);
        }
        if (fragModule) {
            vkDestroyShaderModule(device, fragModule, nullptr);
        }
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{};
    viewport.width = static_cast<float>(state.extent.width);
    viewport.height = static_cast<float>(state.extent.height);
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{{0, 0}, state.extent};
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT
        | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pColorBlendState = &blend;
    pipelineInfo.layout = state.pipelineLayout;
    pipelineInfo.renderPass = state.renderPass;
    pipelineInfo.subpass = 0;
    const VkResult pipelineResult = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &state.pipeline);
    vkDestroyShaderModule(device, vertModule, nullptr);
    vkDestroyShaderModule(device, fragModule, nullptr);
    if (pipelineResult != VK_SUCCESS) {
        return false;
    }

    VkCommandPoolCreateInfo poolCreateInfo{};
    poolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolCreateInfo.queueFamilyIndex = deviceData->graphicsQueueFamily;
    if (vkCreateCommandPool(device, &poolCreateInfo, nullptr, &state.commandPool) != VK_SUCCESS) {
        return false;
    }

    VkCommandBufferAllocateInfo cmdAllocInfo{};
    cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAllocInfo.commandPool = state.commandPool;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device, &cmdAllocInfo, &state.commandBuffer) != VK_SUCCESS) {
        return false;
    }

    state.initialized = true;
    return true;
}

void updateLutBuffer(SwapchainState &state, const AutoHdrCore::GpuUniforms &uniforms, VkDevice device)
{
    void *mapped = nullptr;
    vkMapMemory(device, state.lutMemory, 0, kLutSize * sizeof(float), 0, &mapped);
    std::memcpy(mapped, uniforms.toneCurveLut, kLutSize * sizeof(float));
    vkUnmapMemory(device, state.lutMemory);
}

void transitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkAccessFlags srcAccess = 0;
    VkAccessFlags dstAccess = VK_ACCESS_TRANSFER_READ_BIT;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dstAccess = VK_ACCESS_SHADER_READ_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        srcAccess = VK_ACCESS_MEMORY_READ_BIT;
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dstAccess = VK_ACCESS_SHADER_READ_BIT;
    } else if (newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        dstAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
    } else if (newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        dstAccess = VK_ACCESS_TRANSFER_READ_BIT;
    }

    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

bool processSwapchainImage(DeviceData *deviceData, SwapchainState &state, VkImage sourceImage,
                           const autohdr_vk::RuntimeState &runtime)
{
    if (!runtime.enabled || !ensureSwapchainResources(deviceData, state)) {
        return false;
    }

    VkDevice device = deviceData->device;
    updateLutBuffer(state, runtime.uniforms, device);

    if (state.inputView) {
        vkDestroyImageView(device, state.inputView, nullptr);
        state.inputView = VK_NULL_HANDLE;
    }
    VkImageViewCreateInfo inputViewInfo{};
    inputViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    inputViewInfo.image = sourceImage;
    inputViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    inputViewInfo.format = state.format;
    inputViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    inputViewInfo.subresourceRange.levelCount = 1;
    inputViewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &inputViewInfo, nullptr, &state.inputView) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler = state.sampler;
    imageInfo.imageView = state.inputView;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = state.descriptorSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    vkResetCommandBuffer(state.commandBuffer, 0);
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(state.commandBuffer, &beginInfo);

    transitionImage(state.commandBuffer, sourceImage, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transitionImage(state.commandBuffer, state.outputImage, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    VkRenderPassBeginInfo renderBegin{};
    renderBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderBegin.renderPass = state.renderPass;
    renderBegin.framebuffer = state.framebuffer;
    renderBegin.renderArea.extent = state.extent;
    vkCmdBeginRenderPass(state.commandBuffer, &renderBegin, VK_SUBPASS_CONTENTS_INLINE);

    PushConstants push{};
    push.blackPoint = runtime.uniforms.blackPoint;
    push.colorVibrance = runtime.uniforms.colorVibrance;
    push.gamutExpansion = runtime.uniforms.gamutExpansion;
    push.referenceNits = runtime.uniforms.referenceNits;
    push.displayPeak = runtime.uniforms.displayPeak;
    push.toneCurveInputSpan = runtime.uniforms.toneCurveInputSpan;
    push.colorMode = static_cast<int32_t>(runtime.colorMode);

    vkCmdBindPipeline(state.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, state.pipeline);
    vkCmdBindDescriptorSets(state.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, state.pipelineLayout, 0, 1,
                            &state.descriptorSet, 0, nullptr);
    vkCmdPushConstants(state.commandBuffer, state.pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(state.commandBuffer, 3, 1, 0, 0);
    vkCmdEndRenderPass(state.commandBuffer);

    transitionImage(state.commandBuffer, state.outputImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transitionImage(state.commandBuffer, sourceImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkImageCopy copyRegion{};
    copyRegion.extent = {state.extent.width, state.extent.height, 1};
    vkCmdCopyImage(state.commandBuffer, state.outputImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sourceImage,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
    transitionImage(state.commandBuffer, sourceImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    vkEndCommandBuffer(state.commandBuffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &state.commandBuffer;
    if (vkQueueSubmit(deviceData->graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS) {
        return false;
    }
    vkQueueWaitIdle(deviceData->graphicsQueue);
    return true;
}

DeviceData *getDeviceData(VkDevice device)
{
    std::lock_guard lock(gDeviceMutex);
    const auto it = gDevices.find(device);
    return it == gDevices.end() ? nullptr : it->second;
}

} // namespace autohdr_vk_internal

using namespace autohdr_vk_internal;

VK_LAYER_EXPORT VkResult VKAPI_CALL AutoHdrVK_CreateInstance(const VkInstanceCreateInfo *createInfo,
                                                             const VkAllocationCallbacks *allocator,
                                                             VkInstance *instance)
{
    initShaderModules();
    gConfigState.reload();

    InstanceData *instanceData = new InstanceData();
    auto *chainInfo = const_cast<VkLayerInstanceCreateInfo *>(reinterpret_cast<const VkLayerInstanceCreateInfo *>(createInfo->pNext));
    while (chainInfo && (chainInfo->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO
                         || chainInfo->function != VK_LAYER_LINK_INFO)) {
        chainInfo = const_cast<VkLayerInstanceCreateInfo *>(
            reinterpret_cast<const VkLayerInstanceCreateInfo *>(chainInfo->pNext));
    }

    PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr = nullptr;
    if (chainInfo && chainInfo->u.pLayerInfo) {
        pfnNextGetInstanceProcAddr = chainInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
        chainInfo->u.pLayerInfo = chainInfo->u.pLayerInfo->pNext;
    }

    const VkResult result = vkCreateInstance(createInfo, allocator, instance);
    if (result != VK_SUCCESS) {
        delete instanceData;
        return result;
    }

    instanceData->instance = *instance;
    instanceData->pfnGetInstanceProcAddr = pfnNextGetInstanceProcAddr;
    instanceData->pfnCreateDevice =
        reinterpret_cast<PFN_vkCreateDevice>(pfnNextGetInstanceProcAddr(*instance, "vkCreateDevice"));
    instanceData->pfnDestroyDevice =
        reinterpret_cast<PFN_vkDestroyDevice>(pfnNextGetInstanceProcAddr(*instance, "vkDestroyDevice"));

    std::lock_guard lock(gInstanceMutex);
    gInstances[*instance] = *instanceData;
    delete instanceData;
    return VK_SUCCESS;
}

VK_LAYER_EXPORT void VKAPI_CALL AutoHdrVK_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *allocator)
{
    std::lock_guard lock(gInstanceMutex);
    gInstances.erase(instance);
    vkDestroyInstance(instance, allocator);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL AutoHdrVK_CreateDevice(VkPhysicalDevice physicalDevice,
                                                           const VkDeviceCreateInfo *createInfo,
                                                           const VkAllocationCallbacks *allocator, VkDevice *device)
{
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkCreateDevice pfnCreateDevice = nullptr;
    {
        std::lock_guard lock(gInstanceMutex);
        for (const auto &[inst, data] : gInstances) {
            pfnCreateDevice = data.pfnCreateDevice;
            instance = inst;
            if (pfnCreateDevice) {
                break;
            }
        }
    }

    auto *layerInfo = const_cast<VkLayerDeviceCreateInfo *>(reinterpret_cast<const VkLayerDeviceCreateInfo *>(createInfo->pNext));
    while (layerInfo && (layerInfo->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO
                         || layerInfo->function != VK_LAYER_LINK_INFO)) {
        layerInfo = const_cast<VkLayerDeviceCreateInfo *>(
            reinterpret_cast<const VkLayerDeviceCreateInfo *>(layerInfo->pNext));
    }

    PFN_vkGetDeviceProcAddr pfnNextGetDeviceProcAddr = nullptr;
    if (layerInfo && layerInfo->u.pLayerInfo) {
        pfnNextGetDeviceProcAddr = layerInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;
        layerInfo->u.pLayerInfo = layerInfo->u.pLayerInfo->pNext;
    }

    const VkResult result = pfnCreateDevice(physicalDevice, createInfo, allocator, device);
    if (result != VK_SUCCESS) {
        return result;
    }

    auto *deviceData = new DeviceData();
    deviceData->device = *device;
    deviceData->physicalDevice = physicalDevice;
    deviceData->pfnGetDeviceProcAddr = pfnNextGetDeviceProcAddr;
    deviceData->pfnQueuePresentKHR =
        reinterpret_cast<PFN_vkQueuePresentKHR>(pfnNextGetDeviceProcAddr(*device, "vkQueuePresentKHR"));

    const VkQueue globalQueue = VK_NULL_HANDLE;
    (void)globalQueue;
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, families.data());
    for (uint32_t i = 0; i < queueFamilyCount; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            deviceData->graphicsQueueFamily = i;
            break;
        }
    }
    vkGetDeviceQueue(*device, deviceData->graphicsQueueFamily, 0, &deviceData->graphicsQueue);

    std::lock_guard lock(gDeviceMutex);
    gDevices[*device] = deviceData;
    (void)instance;
    return VK_SUCCESS;
}

VK_LAYER_EXPORT void VKAPI_CALL AutoHdrVK_DestroyDevice(VkDevice device, const VkAllocationCallbacks *allocator)
{
    DeviceData *deviceData = getDeviceData(device);
    if (deviceData) {
        for (auto &[swapchain, state] : deviceData->swapchains) {
            destroySwapchainResources(deviceData, state);
            (void)swapchain;
        }
        std::lock_guard lock(gDeviceMutex);
        gDevices.erase(device);
        delete deviceData;
    }
    vkDestroyDevice(device, allocator);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL AutoHdrVK_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *createInfo,
                                                                 const VkAllocationCallbacks *allocator,
                                                                 VkSwapchainKHR *swapchain)
{
    DeviceData *deviceData = getDeviceData(device);
    PFN_vkCreateSwapchainKHR pfnCreateSwapchainKHR =
        reinterpret_cast<PFN_vkCreateSwapchainKHR>(deviceData->pfnGetDeviceProcAddr(device, "vkCreateSwapchainKHR"));
    const VkResult result = pfnCreateSwapchainKHR(device, createInfo, allocator, swapchain);
    if (result != VK_SUCCESS) {
        return result;
    }

    SwapchainState state;
    state.swapchain = *swapchain;
    state.format = createInfo->imageFormat;
    state.extent = createInfo->imageExtent;

    PFN_vkGetSwapchainImagesKHR pfnGetSwapchainImagesKHR =
        reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(deviceData->pfnGetDeviceProcAddr(device, "vkGetSwapchainImagesKHR"));
    uint32_t imageCount = 0;
    pfnGetSwapchainImagesKHR(device, *swapchain, &imageCount, nullptr);
    state.images.resize(imageCount);
    pfnGetSwapchainImagesKHR(device, *swapchain, &imageCount, state.images.data());

    std::lock_guard lock(deviceData->mutex);
    deviceData->swapchains[*swapchain] = std::move(state);
    return VK_SUCCESS;
}

VK_LAYER_EXPORT void VKAPI_CALL AutoHdrVK_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                              const VkAllocationCallbacks *allocator)
{
    DeviceData *deviceData = getDeviceData(device);
    if (deviceData) {
        std::lock_guard lock(deviceData->mutex);
        auto it = deviceData->swapchains.find(swapchain);
        if (it != deviceData->swapchains.end()) {
            destroySwapchainResources(deviceData, it->second);
            deviceData->swapchains.erase(it);
        }
    }
    PFN_vkDestroySwapchainKHR pfnDestroySwapchainKHR =
        reinterpret_cast<PFN_vkDestroySwapchainKHR>(getDeviceData(device)->pfnGetDeviceProcAddr(device, "vkDestroySwapchainKHR"));
    pfnDestroySwapchainKHR(device, swapchain, allocator);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL AutoHdrVK_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *presentInfo)
{
    DeviceData *deviceData = nullptr;
    for (auto &[device, data] : gDevices) {
        (void)device;
        if (data->graphicsQueue == queue) {
            deviceData = data;
            break;
        }
    }
    if (!deviceData) {
        deviceData = gDevices.empty() ? nullptr : gDevices.begin()->second;
    }

    static std::atomic<int> reloadCounter{0};
    if (reloadCounter.fetch_add(1) % 120 == 0) {
        gConfigState.reload();
    }
    const autohdr_vk::RuntimeState runtime = gConfigState.snapshot();

    if (deviceData && presentInfo && presentInfo->swapchainCount > 0 && runtime.enabled) {
        std::lock_guard lock(deviceData->mutex);
        for (uint32_t i = 0; i < presentInfo->swapchainCount; ++i) {
            auto it = deviceData->swapchains.find(presentInfo->pSwapchains[i]);
            if (it == deviceData->swapchains.end()) {
                continue;
            }
            const uint32_t imageIndex = presentInfo->pImageIndices ? presentInfo->pImageIndices[i] : 0;
            if (imageIndex < it->second.images.size()) {
                processSwapchainImage(deviceData, it->second, it->second.images[imageIndex], runtime);
            }
        }
    }

    return deviceData ? deviceData->pfnQueuePresentKHR(queue, presentInfo) : VK_ERROR_INITIALIZATION_FAILED;
}

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *name)
{
    if (std::strcmp(name, "vkGetInstanceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(vkGetInstanceProcAddr);
    }
    if (std::strcmp(name, "vkCreateInstance") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(AutoHdrVK_CreateInstance);
    }
    if (std::strcmp(name, "vkDestroyInstance") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(AutoHdrVK_DestroyInstance);
    }
    if (std::strcmp(name, "vkCreateDevice") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(AutoHdrVK_CreateDevice);
    }
    if (std::strcmp(name, "vkDestroyDevice") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(AutoHdrVK_DestroyDevice);
    }

    PFN_vkGetInstanceProcAddr pfn = nullptr;
    {
        std::lock_guard lock(gInstanceMutex);
        if (!gInstances.empty()) {
            pfn = gInstances.begin()->second.pfnGetInstanceProcAddr;
        }
    }
    if (!pfn && instance) {
        std::lock_guard lock(gInstanceMutex);
        const auto it = gInstances.find(instance);
        if (it != gInstances.end()) {
            pfn = it->second.pfnGetInstanceProcAddr;
        }
    }
    return pfn ? pfn(instance, name) : nullptr;
}

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *name)
{
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
    }
    if (std::strcmp(name, "vkCreateSwapchainKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(AutoHdrVK_CreateSwapchainKHR);
    }
    if (std::strcmp(name, "vkDestroySwapchainKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(AutoHdrVK_DestroySwapchainKHR);
    }
    if (std::strcmp(name, "vkQueuePresentKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(AutoHdrVK_QueuePresentKHR);
    }

    DeviceData *deviceData = getDeviceData(device);
    if (deviceData && deviceData->pfnGetDeviceProcAddr) {
        return deviceData->pfnGetDeviceProcAddr(device, name);
    }
    return nullptr;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *versionStruct)
{
    if (versionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (versionStruct->loaderLayerInterfaceVersion >= 2) {
        versionStruct->loaderLayerInterfaceVersion = 2;
    }
    versionStruct->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    versionStruct->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
    versionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
    logMessage("negotiated loader interface");
    return VK_SUCCESS;
}

extern "C" VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vk_layerGetInstanceProcAddr(VkInstance instance,
                                                                                      const char *funcName)
{
    return reinterpret_cast<PFN_vkVoidFunction>(vkGetInstanceProcAddr(instance, funcName)) ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
}

extern "C" VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vk_layerGetDeviceProcAddr(VkDevice device, const char *funcName)
{
    return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr(device, funcName)) ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
}

extern "C" VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vk_layerNegotiateLoaderLayerInterfaceVersion(
    VkNegotiateLayerInterface *versionStruct)
{
    return vkNegotiateLoaderLayerInterfaceVersion(versionStruct);
}
