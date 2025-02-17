/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "vulkan/VulkanMemory.h"
#include "vulkan/VulkanPipelineCache.h"

#include <utils/Log.h>
#include <utils/Panic.h>

#include "VulkanConstants.h"
#include "VulkanHandles.h"
#include "VulkanUtility.h"

// Vulkan functions often immediately dereference pointers, so it's fine to pass in a pointer
// to a stack-allocated variable.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-stack-address"

using namespace bluevk;

namespace filament {
namespace backend {

static VulkanPipelineCache::RasterState createDefaultRasterState();

static VkShaderStageFlags getShaderStageFlags(utils::bitset64 key, uint16_t binding) {
    VkShaderStageFlags flags = 0;
    if (key.test(binding * 2 + 0)) {
        flags |= VK_SHADER_STAGE_VERTEX_BIT;
    }
    if (key.test(binding * 2 + 1)) {
        flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    return flags;
}

utils::bitset64
static getPipelineLayoutKey(const Program::SamplerGroupInfo& samplerGroupInfo) noexcept {
    utils::bitset64 key = {};
    for (uint32_t binding = 0; binding < Program::BINDING_COUNT; ++binding) {
        const auto& stageFlags = samplerGroupInfo[binding].stageFlags;
        for (const auto& sampler : samplerGroupInfo[binding].samplers) {
            if (stageFlags.vertex) {
                key.set(sampler.binding * 2 + 0);
            }
            if (stageFlags.fragment) {
                key.set(sampler.binding * 2 + 1);
            }
        }
    }
    return key;
}

VulkanPipelineCache::VulkanPipelineCache() : mDefaultRasterState(createDefaultRasterState()) {
    mDummyBufferWriteInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    mDummyBufferWriteInfo.pNext = nullptr;
    mDummyBufferWriteInfo.dstArrayElement = 0;
    mDummyBufferWriteInfo.descriptorCount = 1;
    mDummyBufferWriteInfo.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    mDummyBufferWriteInfo.pImageInfo = nullptr;
    mDummyBufferWriteInfo.pBufferInfo = &mDummyBufferInfo;
    mDummyBufferWriteInfo.pTexelBufferView = nullptr;

    mDummySamplerWriteInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    mDummySamplerWriteInfo.pNext = nullptr;
    mDummySamplerWriteInfo.dstArrayElement = 0;
    mDummySamplerWriteInfo.descriptorCount = 1;
    mDummySamplerWriteInfo.pImageInfo = &mDummySamplerInfo;
    mDummySamplerWriteInfo.pBufferInfo = nullptr;
    mDummySamplerWriteInfo.pTexelBufferView = nullptr;
    mDummySamplerWriteInfo.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

    mDummyTargetInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    mDummyTargetWriteInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    mDummyTargetWriteInfo.pNext = nullptr;
    mDummyTargetWriteInfo.dstArrayElement = 0;
    mDummyTargetWriteInfo.descriptorCount = 1;
    mDummyTargetWriteInfo.descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    mDummyTargetWriteInfo.pImageInfo = &mDummyTargetInfo;
    mDummyTargetWriteInfo.pBufferInfo = nullptr;
    mDummyTargetWriteInfo.pTexelBufferView = nullptr;
}

VulkanPipelineCache::~VulkanPipelineCache() {
    destroyCache();
}

void VulkanPipelineCache::setDevice(VkDevice device, VmaAllocator allocator) {
    assert_invariant(mDevice == VK_NULL_HANDLE);
    mDevice = device;
    mAllocator = allocator;
    mDescriptorPool = createDescriptorPool(mDescriptorPoolSize);

    // Formulate some dummy objects and dummy descriptor info used only for clearing out unused
    // bindings. This is especially crucial after a texture has been destroyed. Since core Vulkan
    // does not allow specifying VK_NULL_HANDLE without the robustness2 extension, we are forced to
    // bind dummy objects.

    VkBufferCreateInfo bufferInfo {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 16,
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
    };
    VmaAllocationCreateInfo allocInfo { .usage = VMA_MEMORY_USAGE_GPU_ONLY };
    vmaCreateBuffer(mAllocator, &bufferInfo, &allocInfo, &mDummyBuffer, &mDummyMemory, nullptr);

    mDummyBufferInfo.buffer = mDummyBuffer;
    mDummyBufferInfo.range = bufferInfo.size;
    mDummySamplerInfo.imageLayout = mDummyTargetInfo.imageLayout;

    VkSamplerCreateInfo samplerInfo {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .anisotropyEnable = VK_FALSE,
        .maxAnisotropy = 1,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_ALWAYS,
        .minLod = 0.0f,
        .maxLod = 1.0f,
        .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
        .unnormalizedCoordinates = VK_FALSE
    };
    vkCreateSampler(mDevice, &samplerInfo, VKALLOC, &mDummySamplerInfo.sampler);
}

bool VulkanPipelineCache::bindDescriptors(VkCommandBuffer cmdbuffer) noexcept {
    DescriptorMap::iterator descriptorIter = mDescriptorSets.find(mDescriptorRequirements);

    // Check if the required descriptors are already bound.
    if (DescEqual equals; UTILS_LIKELY(equals(mBoundDescriptor, mDescriptorRequirements))) {
        assert_invariant(descriptorIter != mDescriptorSets.end());
        descriptorIter.value().lastUsed = mCurrentTime;
        return true;
    }

    // If a cached object exists, re-use it, otherwise create a new one.
    DescriptorCacheEntry* cacheEntry = UTILS_LIKELY(descriptorIter != mDescriptorSets.end()) ?
            &descriptorIter.value() : createDescriptorSets();

    // If a descriptor set overflow occurred, allow higher levels to handle it gracefully.
    assert_invariant(cacheEntry != nullptr);
    if (UTILS_UNLIKELY(cacheEntry == nullptr)) {
        return false;
    }

    cacheEntry->lastUsed = mCurrentTime;
    mBoundDescriptor = mDescriptorRequirements;

    vkCmdBindDescriptorSets(cmdbuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            getOrCreatePipelineLayout()->handle, 0, VulkanPipelineCache::DESCRIPTOR_TYPE_COUNT,
            cacheEntry->handles.data(), 0, nullptr);

    return true;
}

bool VulkanPipelineCache::bindPipeline(VkCommandBuffer cmdbuffer) noexcept {
    PipelineMap::iterator pipelineIter = mPipelines.find(mPipelineRequirements);

    // Check if the required pipeline is already bound.
    if (PipelineEqual equals; UTILS_LIKELY(equals(mBoundPipeline, mPipelineRequirements))) {
        assert_invariant(pipelineIter != mPipelines.end());
        pipelineIter.value().lastUsed = mCurrentTime;
        return true;
    }

    // If a cached object exists, re-use it, otherwise create a new one.
    PipelineCacheEntry* cacheEntry = UTILS_LIKELY(pipelineIter != mPipelines.end()) ?
            &pipelineIter.value() : createPipeline();

    // If an error occurred, allow higher levels to handle it gracefully.
    assert_invariant(cacheEntry != nullptr);
    if (UTILS_UNLIKELY(cacheEntry == nullptr)) {
        return false;
    }

    cacheEntry->lastUsed = mCurrentTime;
    getOrCreatePipelineLayout()->lastUsed = mCurrentTime;

    mBoundPipeline = mPipelineRequirements;
    mBoundLayout = mLayoutRequirements;

    vkCmdBindPipeline(cmdbuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, cacheEntry->handle);
    return true;
}

void VulkanPipelineCache::bindScissor(VkCommandBuffer cmdbuffer, VkRect2D scissor) noexcept {
    if (UTILS_UNLIKELY(!equivalent(mCurrentScissor, scissor))) {
        mCurrentScissor = scissor;
        vkCmdSetScissor(cmdbuffer, 0, 1, &scissor);
    }
}

VulkanPipelineCache::DescriptorCacheEntry* VulkanPipelineCache::createDescriptorSets() noexcept {
    PipelineLayoutCacheEntry* layoutCacheEntry = getOrCreatePipelineLayout();

    DescriptorCacheEntry& descriptorCacheEntry = mDescriptorSets.emplace(
            std::make_pair(mDescriptorRequirements, DescriptorCacheEntry {})).first.value();

    descriptorCacheEntry.pipelineLayout = mLayoutRequirements;

    // Each of the arenas for this particular layout are guaranteed to have the same size. Check
    // the first arena to see if any descriptor sets are available that can be re-claimed. If not,
    // create brand new ones (one for each type). They will be added to the arena later, after they
    // are no longer used. This occurs during the cleanup phase during command buffer submission.
    auto& descriptorSetArenas = layoutCacheEntry->descriptorSetArenas;
    if (descriptorSetArenas[0].empty()) {
        if (mDescriptorSets.size() >= mDescriptorPoolSize) {
            growDescriptorPool();
        }
        VkDescriptorSetAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = mDescriptorPool;
        allocInfo.descriptorSetCount = DESCRIPTOR_TYPE_COUNT;
        allocInfo.pSetLayouts = layoutCacheEntry->descriptorSetLayouts.data();
        VkResult error = vkAllocateDescriptorSets(mDevice, &allocInfo,
                descriptorCacheEntry.handles.data());
        assert_invariant(error == VK_SUCCESS);
        if (error != VK_SUCCESS) {
            return nullptr;
        }
    } else {
        for (uint32_t i = 0; i < DESCRIPTOR_TYPE_COUNT; ++i) {
            descriptorCacheEntry.handles[i] = descriptorSetArenas[i].back();
            descriptorSetArenas[i].pop_back();
        }
    }

    // Rewrite every binding in the new descriptor sets.
    VkDescriptorBufferInfo descriptorBuffers[UBUFFER_BINDING_COUNT];
    VkDescriptorImageInfo descriptorSamplers[SAMPLER_BINDING_COUNT];
    VkDescriptorImageInfo descriptorInputAttachments[TARGET_BINDING_COUNT];
    VkWriteDescriptorSet descriptorWrites[UBUFFER_BINDING_COUNT + SAMPLER_BINDING_COUNT +
            TARGET_BINDING_COUNT];
    uint32_t nwrites = 0;
    VkWriteDescriptorSet* writes = descriptorWrites;
    nwrites = 0;
    for (uint32_t binding = 0; binding < UBUFFER_BINDING_COUNT; binding++) {
        VkWriteDescriptorSet& writeInfo = writes[nwrites++];
        if (mDescriptorRequirements.uniformBuffers[binding]) {
            VkDescriptorBufferInfo& bufferInfo = descriptorBuffers[binding];
            bufferInfo.buffer = mDescriptorRequirements.uniformBuffers[binding];
            bufferInfo.offset = mDescriptorRequirements.uniformBufferOffsets[binding];
            bufferInfo.range = mDescriptorRequirements.uniformBufferSizes[binding];
            writeInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writeInfo.pNext = nullptr;
            writeInfo.dstArrayElement = 0;
            writeInfo.descriptorCount = 1;
            writeInfo.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writeInfo.pImageInfo = nullptr;
            writeInfo.pBufferInfo = &bufferInfo;
            writeInfo.pTexelBufferView = nullptr;
        } else {
            writeInfo = mDummyBufferWriteInfo;
            assert_invariant(mDummyBufferWriteInfo.pBufferInfo->buffer);
        }
        assert_invariant(writeInfo.pBufferInfo->buffer);
        writeInfo.dstSet = descriptorCacheEntry.handles[0];
        writeInfo.dstBinding = binding;
    }
    for (uint32_t binding = 0; binding < SAMPLER_BINDING_COUNT; binding++) {
        VkWriteDescriptorSet& writeInfo = writes[nwrites++];
        if (mDescriptorRequirements.samplers[binding].sampler) {
            VkDescriptorImageInfo& imageInfo = descriptorSamplers[binding];
            imageInfo = mDescriptorRequirements.samplers[binding];
            writeInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writeInfo.pNext = nullptr;
            writeInfo.dstArrayElement = 0;
            writeInfo.descriptorCount = 1;
            writeInfo.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writeInfo.pImageInfo = &imageInfo;
            writeInfo.pBufferInfo = nullptr;
            writeInfo.pTexelBufferView = nullptr;
        } else {
            writeInfo = mDummySamplerWriteInfo;
            assert_invariant(mDummySamplerWriteInfo.pImageInfo->sampler);
            assert_invariant(mDummySamplerInfo.imageView);

        }
        writeInfo.dstSet = descriptorCacheEntry.handles[1];
        writeInfo.dstBinding = binding;
    }
    for (uint32_t binding = 0; binding < TARGET_BINDING_COUNT; binding++) {
        VkWriteDescriptorSet& writeInfo = writes[nwrites++];
        if (mDescriptorRequirements.inputAttachments[binding].imageView) {
            VkDescriptorImageInfo& imageInfo = descriptorInputAttachments[binding];
            imageInfo = mDescriptorRequirements.inputAttachments[binding];
            writeInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writeInfo.pNext = nullptr;
            writeInfo.dstArrayElement = 0;
            writeInfo.descriptorCount = 1;
            writeInfo.descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
            writeInfo.pImageInfo = &imageInfo;
            writeInfo.pBufferInfo = nullptr;
            writeInfo.pTexelBufferView = nullptr;
        } else {
            writeInfo = mDummyTargetWriteInfo;
            assert_invariant(mDummyTargetInfo.imageView);
        }
        writeInfo.dstSet = descriptorCacheEntry.handles[2];
        writeInfo.dstBinding = binding;
    }
    vkUpdateDescriptorSets(mDevice, nwrites, writes, 0, nullptr);
    return &descriptorCacheEntry;
}

VulkanPipelineCache::PipelineCacheEntry* VulkanPipelineCache::createPipeline() noexcept {
    assert_invariant(mPipelineRequirements.shaders[0] && "Vertex shader is not bound.");

    PipelineLayoutCacheEntry* layout = getOrCreatePipelineLayout();
    assert_invariant(layout);

    VkPipelineShaderStageCreateInfo shaderStages[SHADER_MODULE_COUNT];
    shaderStages[0] = VkPipelineShaderStageCreateInfo{};
    shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStages[0].pName = "main";

    shaderStages[1] = VkPipelineShaderStageCreateInfo{};
    shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderStages[1].pName = "main";

    VkPipelineColorBlendAttachmentState colorBlendAttachments[MRT::MAX_SUPPORTED_RENDER_TARGET_COUNT];
    VkPipelineColorBlendStateCreateInfo colorBlendState;
    colorBlendState = VkPipelineColorBlendStateCreateInfo{};
    colorBlendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlendState.attachmentCount = 1;
    colorBlendState.pAttachments = colorBlendAttachments;

    // If we reach this point, we need to create and stash a brand new pipeline object.
    shaderStages[0].module = mPipelineRequirements.shaders[0];
    shaderStages[1].module = mPipelineRequirements.shaders[1];

    // We don't store array sizes to save space, but it's quick to count all non-zero
    // entries because these arrays have a small fixed-size capacity.
    uint32_t numVertexAttribs = 0;
    uint32_t numVertexBuffers = 0;
    for (uint32_t i = 0; i < VERTEX_ATTRIBUTE_COUNT; i++) {
        if (mPipelineRequirements.vertexAttributes[i].format > 0) {
            numVertexAttribs++;
        }
        if (mPipelineRequirements.vertexBuffers[i].stride > 0) {
            numVertexBuffers++;
        }
    }

    VkPipelineVertexInputStateCreateInfo vertexInputState = {};
    vertexInputState.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputState.vertexBindingDescriptionCount = numVertexBuffers;
    vertexInputState.pVertexBindingDescriptions = mPipelineRequirements.vertexBuffers;
    vertexInputState.vertexAttributeDescriptionCount = numVertexAttribs;
    vertexInputState.pVertexAttributeDescriptions = mPipelineRequirements.vertexAttributes;

    VkPipelineInputAssemblyStateCreateInfo inputAssemblyState = {};
    inputAssemblyState.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssemblyState.topology = mPipelineRequirements.topology;

    VkPipelineViewportStateCreateInfo viewportState = {};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkDynamicState dynamicStateEnables[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    VkPipelineDynamicStateCreateInfo dynamicState = {};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.pDynamicStates = dynamicStateEnables;
    dynamicState.dynamicStateCount = 2;

    const bool hasFragmentShader = shaderStages[1].module != VK_NULL_HANDLE;

    VkGraphicsPipelineCreateInfo pipelineCreateInfo = {};
    pipelineCreateInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineCreateInfo.layout = layout->handle;
    pipelineCreateInfo.renderPass = mPipelineRequirements.renderPass;
    pipelineCreateInfo.subpass = mPipelineRequirements.subpassIndex;
    pipelineCreateInfo.stageCount = hasFragmentShader ? SHADER_MODULE_COUNT : 1;
    pipelineCreateInfo.pStages = shaderStages;
    pipelineCreateInfo.pVertexInputState = &vertexInputState;
    pipelineCreateInfo.pInputAssemblyState = &inputAssemblyState;

    VkPipelineRasterizationStateCreateInfo vkRaster = {};
    vkRaster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    pipelineCreateInfo.pRasterizationState = &vkRaster;

    VkPipelineMultisampleStateCreateInfo vkMs = {};
    vkMs.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    pipelineCreateInfo.pMultisampleState = &vkMs;

    VkPipelineDepthStencilStateCreateInfo vkDs = {};
    vkDs.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    vkDs.front = vkDs.back = {
            .failOp = VK_STENCIL_OP_KEEP,
            .passOp = VK_STENCIL_OP_KEEP,
            .depthFailOp = VK_STENCIL_OP_KEEP,
            .compareOp = VK_COMPARE_OP_ALWAYS,
            .compareMask = 0u,
            .writeMask = 0u,
            .reference = 0u,
    };
    pipelineCreateInfo.pDepthStencilState = &vkDs;

    const auto& raster = mPipelineRequirements.rasterState;

    vkRaster.depthClampEnable = raster.rasterization.depthClampEnable;
    vkRaster.rasterizerDiscardEnable = raster.rasterization.rasterizerDiscardEnable;
    vkRaster.polygonMode = raster.rasterization.polygonMode;
    vkRaster.cullMode = raster.rasterization.cullMode;
    vkRaster.frontFace = raster.rasterization.frontFace;
    vkRaster.depthBiasEnable = raster.rasterization.depthBiasEnable;
    vkRaster.depthBiasConstantFactor = raster.rasterization.depthBiasConstantFactor;
    vkRaster.depthBiasClamp = raster.rasterization.depthBiasClamp;
    vkRaster.depthBiasSlopeFactor = raster.rasterization.depthBiasSlopeFactor;
    vkRaster.lineWidth = raster.rasterization.lineWidth;

    vkMs.rasterizationSamples = raster.multisampling.rasterizationSamples;
    vkMs.sampleShadingEnable = raster.multisampling.sampleShadingEnable;
    vkMs.minSampleShading = raster.multisampling.minSampleShading;
    vkMs.alphaToCoverageEnable = raster.multisampling.alphaToCoverageEnable;
    vkMs.alphaToOneEnable = raster.multisampling.alphaToOneEnable;

    vkDs.depthTestEnable = raster.depthStencil.depthTestEnable;
    vkDs.depthWriteEnable = raster.depthStencil.depthWriteEnable;
    vkDs.depthCompareOp = raster.depthStencil.depthCompareOp;
    vkDs.depthBoundsTestEnable = raster.depthStencil.depthBoundsTestEnable;
    vkDs.stencilTestEnable = raster.depthStencil.stencilTestEnable;
    vkDs.minDepthBounds = raster.depthStencil.minDepthBounds;
    vkDs.maxDepthBounds = raster.depthStencil.maxDepthBounds;

    pipelineCreateInfo.pColorBlendState = &colorBlendState;
    pipelineCreateInfo.pViewportState = &viewportState;
    pipelineCreateInfo.pDynamicState = &dynamicState;

    // Filament assumes consistent blend state across all color attachments.
    colorBlendState.attachmentCount = mPipelineRequirements.rasterState.colorTargetCount;
    for (auto& target : colorBlendAttachments) {
        target = mPipelineRequirements.rasterState.blending;
    }

    // There are no color attachments if there is no bound fragment shader.  (e.g. shadow map gen)
    // TODO: This should be handled in a higher layer.
    if (!hasFragmentShader) {
        colorBlendState.attachmentCount = 0;
    }

    PipelineCacheEntry& bundle = mPipelines.emplace(
            std::make_pair(mPipelineRequirements, PipelineCacheEntry {})).first.value();

    #if FILAMENT_VULKAN_VERBOSE
    utils::slog.d << "vkCreateGraphicsPipelines with shaders = ("
            << shaderStages[0].module << ", " << shaderStages[1].module << ")" << utils::io::endl;
    #endif
    VkResult error = vkCreateGraphicsPipelines(mDevice, VK_NULL_HANDLE, 1, &pipelineCreateInfo,
            VKALLOC, &bundle.handle);
    assert_invariant(error == VK_SUCCESS);
    if (error != VK_SUCCESS) {
        utils::slog.e << "vkCreateGraphicsPipelines error " << error << utils::io::endl;
        return nullptr;
    }

    return &bundle;
}

VulkanPipelineCache::PipelineLayoutCacheEntry* VulkanPipelineCache::getOrCreatePipelineLayout() noexcept {
    auto iter = mPipelineLayouts.find(mLayoutRequirements);
    if (UTILS_LIKELY(iter != mPipelineLayouts.end())) {
        return &iter.value();
    }

    PipelineLayoutCacheEntry& cacheEntry = mPipelineLayouts.emplace(
            mLayoutRequirements, PipelineLayoutCacheEntry{}).first.value();

    VkDescriptorSetLayoutBinding binding = {};
    binding.descriptorCount = 1; // NOTE: We never use arrays-of-blocks.
    binding.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS; // NOTE: This is potentially non-optimal.

    // First create the descriptor set layout for UBO's.
    VkDescriptorSetLayoutBinding ubindings[UBUFFER_BINDING_COUNT];
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    for (uint32_t i = 0; i < UBUFFER_BINDING_COUNT; i++) {
        binding.binding = i;
        ubindings[i] = binding;
    }
    VkDescriptorSetLayoutCreateInfo dlinfo = {};
    dlinfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dlinfo.bindingCount = UBUFFER_BINDING_COUNT;
    dlinfo.pBindings = ubindings;
    vkCreateDescriptorSetLayout(mDevice, &dlinfo, VKALLOC, &cacheEntry.descriptorSetLayouts[0]);

    // Next create the descriptor set layout for samplers.
    VkDescriptorSetLayoutBinding sbindings[SAMPLER_BINDING_COUNT];
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    for (uint32_t i = 0; i < SAMPLER_BINDING_COUNT; i++) {
        binding.stageFlags = getShaderStageFlags(mLayoutRequirements, i);
        binding.binding = i;
        sbindings[i] = binding;
    }
    dlinfo.bindingCount = SAMPLER_BINDING_COUNT;
    dlinfo.pBindings = sbindings;
    vkCreateDescriptorSetLayout(mDevice, &dlinfo, VKALLOC, &cacheEntry.descriptorSetLayouts[1]);

    // Next create the descriptor set layout for input attachments.
    VkDescriptorSetLayoutBinding tbindings[TARGET_BINDING_COUNT];
    binding.descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    for (uint32_t i = 0; i < TARGET_BINDING_COUNT; i++) {
        binding.binding = i;
        tbindings[i] = binding;
    }
    dlinfo.bindingCount = TARGET_BINDING_COUNT;
    dlinfo.pBindings = tbindings;
    vkCreateDescriptorSetLayout(mDevice, &dlinfo, VKALLOC, &cacheEntry.descriptorSetLayouts[2]);

    // Create VkPipelineLayout based on how to resources are bounded.
    VkPipelineLayoutCreateInfo pPipelineLayoutCreateInfo = {};
    pPipelineLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pPipelineLayoutCreateInfo.setLayoutCount = cacheEntry.descriptorSetLayouts.size();
    pPipelineLayoutCreateInfo.pSetLayouts = cacheEntry.descriptorSetLayouts.data();
    VkResult error = vkCreatePipelineLayout(mDevice, &pPipelineLayoutCreateInfo, VKALLOC,
            &cacheEntry.handle);
    assert_invariant(error == VK_SUCCESS);
    return error == VK_SUCCESS ? &cacheEntry : nullptr;
}

void VulkanPipelineCache::bindProgram(const VulkanProgram& program) noexcept {
    const VkShaderModule shaders[2] = { program.bundle.vertex, program.bundle.fragment };
    for (uint32_t ssi = 0; ssi < SHADER_MODULE_COUNT; ssi++) {
        mPipelineRequirements.shaders[ssi] = shaders[ssi];
    }
    mLayoutRequirements = getPipelineLayoutKey(program.samplerGroupInfo);
}

void VulkanPipelineCache::bindRasterState(const RasterState& rasterState) noexcept {
    mPipelineRequirements.rasterState = rasterState;
}

void VulkanPipelineCache::bindRenderPass(VkRenderPass renderPass, int subpassIndex) noexcept {
    mPipelineRequirements.renderPass = renderPass;
    mPipelineRequirements.subpassIndex = subpassIndex;
}

void VulkanPipelineCache::bindPrimitiveTopology(VkPrimitiveTopology topology) noexcept {
    mPipelineRequirements.topology = topology;
}

void VulkanPipelineCache::bindVertexArray(const VertexArray& varray) noexcept {
    for (size_t i = 0; i < VERTEX_ATTRIBUTE_COUNT; i++) {
        mPipelineRequirements.vertexAttributes[i] = varray.attributes[i];
        mPipelineRequirements.vertexBuffers[i] = varray.buffers[i];
    }
}

VulkanPipelineCache::UniformBufferBinding VulkanPipelineCache::getUniformBufferBinding(uint32_t bindingIndex) const noexcept {
    auto& key = mDescriptorRequirements;
    return {
        key.uniformBuffers[bindingIndex],
        key.uniformBufferOffsets[bindingIndex],
        key.uniformBufferSizes[bindingIndex],
    };
}

void VulkanPipelineCache::unbindUniformBuffer(VkBuffer uniformBuffer) noexcept {
    auto& key = mDescriptorRequirements;
    for (uint32_t bindingIndex = 0u; bindingIndex < UBUFFER_BINDING_COUNT; ++bindingIndex) {
        if (key.uniformBuffers[bindingIndex] == uniformBuffer) {
            key.uniformBuffers[bindingIndex] = {};
            key.uniformBufferSizes[bindingIndex] = {};
            key.uniformBufferOffsets[bindingIndex] = {};
        }
    }
}

void VulkanPipelineCache::unbindImageView(VkImageView imageView) noexcept {
    for (auto& sampler : mDescriptorRequirements.samplers) {
        if (sampler.imageView == imageView) {
            sampler = {};
        }
    }
    for (auto& target : mDescriptorRequirements.inputAttachments) {
        if (target.imageView == imageView) {
            target = {};
        }
    }
}

void VulkanPipelineCache::bindUniformBuffer(uint32_t bindingIndex, VkBuffer uniformBuffer,
        VkDeviceSize offset, VkDeviceSize size) noexcept {
    ASSERT_POSTCONDITION(bindingIndex < UBUFFER_BINDING_COUNT,
            "Uniform bindings overflow: index = %d, capacity = %d.",
            bindingIndex, UBUFFER_BINDING_COUNT);
    auto& key = mDescriptorRequirements;
    key.uniformBuffers[bindingIndex] = uniformBuffer;
    key.uniformBufferOffsets[bindingIndex] = offset;
    key.uniformBufferSizes[bindingIndex] = size;
}

void VulkanPipelineCache::bindSamplers(VkDescriptorImageInfo samplers[SAMPLER_BINDING_COUNT]) noexcept {
    for (uint32_t bindingIndex = 0; bindingIndex < SAMPLER_BINDING_COUNT; bindingIndex++) {
        mDescriptorRequirements.samplers[bindingIndex] = samplers[bindingIndex];
    }
}

void VulkanPipelineCache::bindInputAttachment(uint32_t bindingIndex,
        VkDescriptorImageInfo targetInfo) noexcept {
    ASSERT_POSTCONDITION(bindingIndex < TARGET_BINDING_COUNT,
            "Input attachment bindings overflow: index = %d, capacity = %d.",
            bindingIndex, TARGET_BINDING_COUNT);
    mDescriptorRequirements.inputAttachments[bindingIndex] = targetInfo;
}

void VulkanPipelineCache::destroyCache() noexcept {
    // Symmetric to createLayoutsAndDescriptors.
    destroyLayoutsAndDescriptors();
    for (auto& iter : mPipelines) {
        vkDestroyPipeline(mDevice, iter.second.handle, VKALLOC);
    }
    mPipelines.clear();
    mBoundPipeline = {};
    if (mDummySamplerInfo.sampler) {
        vkDestroySampler(mDevice, mDummySamplerInfo.sampler, VKALLOC);
        mDummySamplerInfo.sampler = VK_NULL_HANDLE;
    }
    vmaDestroyBuffer(mAllocator, mDummyBuffer, mDummyMemory);
    mDummyBuffer = VK_NULL_HANDLE;
    mDummyMemory = VK_NULL_HANDLE;
}

void VulkanPipelineCache::onCommandBuffer(const VulkanCommandBuffer& cmdbuffer) {
    // The timestamp associated with a given cache entry represents "time" as a count of flush
    // events since the cache was constructed. If any cache entry was most recently used over
    // VK_MAX_PIPELINE_AGE flush events in the past, then we can be sure that it is no longer
    // being used by the GPU, and is therefore safe to destroy or reclaim.
    ++mCurrentTime;

    // The Vulkan spec says: "When a command buffer begins recording, all state in that command
    // buffer is undefined." Therefore we need to clear all bindings at this time.
    mBoundPipeline = {};
    mBoundLayout = {};
    mBoundDescriptor = {};
    mCurrentScissor = {};

    // NOTE: Due to robin_map restrictions, we cannot use auto or range-based loops.

    // Check if any bundles in the cache are no longer in use by any command buffer. Descriptors
    // from unused bundles are moved back to their respective arenas.
    using ConstDescIterator = decltype(mDescriptorSets)::const_iterator;
    for (ConstDescIterator iter = mDescriptorSets.begin(); iter != mDescriptorSets.end();) {
        const DescriptorCacheEntry& cacheEntry = iter.value();
        if (cacheEntry.lastUsed + VK_MAX_PIPELINE_AGE < mCurrentTime) {
            auto& arenas = mPipelineLayouts[cacheEntry.pipelineLayout].descriptorSetArenas;
            for (uint32_t i = 0; i < DESCRIPTOR_TYPE_COUNT; ++i) {
                arenas[i].push_back(cacheEntry.handles[i]);
            }
            iter = mDescriptorSets.erase(iter);
        } else {
            ++iter;
        }
    }

    // Evict any pipelines that have not been used in a while.
    // Any pipeline older than VK_MAX_COMMAND_BUFFERS can be safely destroyed.
    using ConstPipeIterator = decltype(mPipelines)::const_iterator;
    for (ConstPipeIterator iter = mPipelines.begin(); iter != mPipelines.end();) {
        const PipelineCacheEntry& cacheEntry = iter.value();
        if (cacheEntry.lastUsed + VK_MAX_PIPELINE_AGE < mCurrentTime) {
            vkDestroyPipeline(mDevice, iter->second.handle, VKALLOC);
            iter = mPipelines.erase(iter);
        } else {
            ++iter;
        }
    }

    // Evict any layouts that have not been used in a while.
    using ConstLayoutIterator = decltype(mPipelineLayouts)::const_iterator;
    for (ConstLayoutIterator iter = mPipelineLayouts.begin(); iter != mPipelineLayouts.end();) {
        const PipelineLayoutCacheEntry& cacheEntry = iter.value();
        if (cacheEntry.lastUsed + VK_MAX_PIPELINE_AGE < mCurrentTime) {
            vkDestroyPipelineLayout(mDevice, iter->second.handle, VKALLOC);
            for (auto setLayout : iter->second.descriptorSetLayouts) {
#ifndef NDEBUG
                PipelineLayoutKey key = iter.key();
                for (auto& pair : mDescriptorSets) {
                    assert_invariant(pair.second.pipelineLayout != key);
                }
#endif
                vkDestroyDescriptorSetLayout(mDevice, setLayout, VKALLOC);
            }
            iter = mPipelineLayouts.erase(iter);
        } else {
            ++iter;
        }
    }

    // If there are no descriptors from any extinct pool that are still in use, we can safely
    // destroy the extinct pools, which implicitly frees their associated descriptor sets.
    bool canPurgeExtinctPools = true;
    for (auto& bundle : mExtinctDescriptorBundles) {
        if (bundle.lastUsed + VK_MAX_PIPELINE_AGE >= mCurrentTime) {
            canPurgeExtinctPools = false;
            break;
        }
    }
    if (canPurgeExtinctPools) {
        for (VkDescriptorPool pool : mExtinctDescriptorPools) {
            vkDestroyDescriptorPool(mDevice, pool, VKALLOC);
        }
        mExtinctDescriptorPools.clear();
        mExtinctDescriptorBundles.clear();
    }
}

VkDescriptorPool VulkanPipelineCache::createDescriptorPool(uint32_t size) const {
    VkDescriptorPoolSize poolSizes[DESCRIPTOR_TYPE_COUNT] = {};
    VkDescriptorPoolCreateInfo poolInfo {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = size * DESCRIPTOR_TYPE_COUNT,
        .poolSizeCount = DESCRIPTOR_TYPE_COUNT,
        .pPoolSizes = poolSizes
    };
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = poolInfo.maxSets * UBUFFER_BINDING_COUNT;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = poolInfo.maxSets * SAMPLER_BINDING_COUNT;
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    poolSizes[2].descriptorCount = poolInfo.maxSets * TARGET_BINDING_COUNT;

    VkDescriptorPool pool;
    const UTILS_UNUSED VkResult result = vkCreateDescriptorPool(mDevice, &poolInfo, VKALLOC, &pool);
    assert_invariant(result == VK_SUCCESS);
    return pool;
}

void VulkanPipelineCache::destroyLayoutsAndDescriptors() noexcept {
    // Our current descriptor set strategy can cause the # of descriptor sets to explode in certain
    // situations, so it's interesting to report the number that get stuffed into the cache.
    #ifndef NDEBUG
    utils::slog.d << "Destroying " << mDescriptorSets.size() << " bundles of descriptor sets."
            << utils::io::endl;
    #endif

    mDescriptorSets.clear();

    // Our current layout bundle strategy can cause the # of layout bundles to explode in certain
    // situations, so it's interesting to report the number that get stuffed into the cache.
    #ifndef NDEBUG
    utils::slog.d << "Destroying " << mPipelineLayouts.size() << " pipeline layouts."
                  << utils::io::endl;
    #endif

    for (auto& iter : mPipelineLayouts) {
        vkDestroyPipelineLayout(mDevice, iter.second.handle, VKALLOC);
        for (auto setLayout : iter.second.descriptorSetLayouts) {
            vkDestroyDescriptorSetLayout(mDevice, setLayout, VKALLOC);
        }
        // There is no need to free descriptor sets individually since destroying the VkDescriptorPool
        // implicitly frees them.
    }
    mPipelineLayouts.clear();
    vkDestroyDescriptorPool(mDevice, mDescriptorPool, VKALLOC);
    mDescriptorPool = VK_NULL_HANDLE;

    for (VkDescriptorPool pool : mExtinctDescriptorPools) {
        vkDestroyDescriptorPool(mDevice, pool, VKALLOC);
    }
    mExtinctDescriptorPools.clear();
    mExtinctDescriptorBundles.clear();

    mBoundDescriptor = {};
}

void VulkanPipelineCache::growDescriptorPool() noexcept {
    // We need to destroy the old VkDescriptorPool, but we can't do so immediately because many
    // of its descriptors are still in use. So, stash it in an "extinct" list.
    mExtinctDescriptorPools.push_back(mDescriptorPool);

    // Create the new VkDescriptorPool, twice as big as the old one.
    mDescriptorPoolSize *= 2;
    mDescriptorPool = createDescriptorPool(mDescriptorPoolSize);

    // Clear out all unused descriptor sets in the arena so they don't get reclaimed. There is no
    // need to free them individually since the old VkDescriptorPool will be destroyed.
    for (auto iter = mPipelineLayouts.begin(); iter != mPipelineLayouts.end(); ++iter) {
        for (auto& arena : iter.value().descriptorSetArenas) {
            arena.clear();
        }
    }

    // Move all in-use descriptors from the primary cache into an "extinct" list, so that they will
    // later be destroyed rather than reclaimed.
    using DescIterator = decltype(mDescriptorSets)::iterator;
    for (DescIterator iter = mDescriptorSets.begin(); iter != mDescriptorSets.end(); ++iter) {
        mExtinctDescriptorBundles.push_back(iter.value());
    }
    mDescriptorSets.clear();
}

size_t VulkanPipelineCache::PipelineLayoutKeyHashFn::operator()(const PipelineLayoutKey& key) const {
    return key.getValue();
}

bool VulkanPipelineCache::PipelineLayoutKeyEqual::operator()(const PipelineLayoutKey& k1,
        const PipelineLayoutKey& k2) const {
    return k1 == k2;
}

bool VulkanPipelineCache::PipelineEqual::operator()(const VulkanPipelineCache::PipelineKey& k1,
        const VulkanPipelineCache::PipelineKey& k2) const {
    return 0 == memcmp((const void*) &k1, (const void*) &k2, sizeof(k1));
}

bool VulkanPipelineCache::DescEqual::operator()(const VulkanPipelineCache::DescriptorKey& k1,
        const VulkanPipelineCache::DescriptorKey& k2) const {
    for (uint32_t i = 0; i < UBUFFER_BINDING_COUNT; i++) {
        if (k1.uniformBuffers[i] != k2.uniformBuffers[i] ||
            k1.uniformBufferOffsets[i] != k2.uniformBufferOffsets[i] ||
            k1.uniformBufferSizes[i] != k2.uniformBufferSizes[i]) {
            return false;
        }
    }
    for (uint32_t i = 0; i < SAMPLER_BINDING_COUNT; i++) {
        if (k1.samplers[i].sampler != k2.samplers[i].sampler ||
            k1.samplers[i].imageView != k2.samplers[i].imageView ||
            k1.samplers[i].imageLayout != k2.samplers[i].imageLayout) {
            return false;
        }
    }
    for (uint32_t i = 0; i < TARGET_BINDING_COUNT; i++) {
        if (k1.inputAttachments[i].imageView != k2.inputAttachments[i].imageView ||
            k1.inputAttachments[i].imageLayout != k2.inputAttachments[i].imageLayout) {
            return false;
        }
    }
    return true;
}

static VulkanPipelineCache::RasterState createDefaultRasterState() {
    return VulkanPipelineCache::RasterState {
        .rasterization = {
            .depthClampEnable = VK_FALSE,
            .rasterizerDiscardEnable = VK_FALSE,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = VK_CULL_MODE_NONE,
            .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .depthBiasEnable = VK_FALSE,
            .depthBiasConstantFactor = 0.0f,
            .depthBiasClamp = 0.0f,
            .depthBiasSlopeFactor = 0.0f,
            .lineWidth = 1.0f,
        },
        .blending = {
            .blendEnable = VK_FALSE,
            .colorWriteMask = 0xf,
        },
        .depthStencil = {
            .depthTestEnable = VK_TRUE,
            .depthWriteEnable = VK_TRUE,
            .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
            .depthBoundsTestEnable = VK_FALSE,
            .stencilTestEnable = VK_FALSE,
        },
        .multisampling = {
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
            .alphaToCoverageEnable = true,
        },
        .colorTargetCount = 1,
    };
}

} // namespace filament
} // namespace backend

#pragma clang diagnostic pop
