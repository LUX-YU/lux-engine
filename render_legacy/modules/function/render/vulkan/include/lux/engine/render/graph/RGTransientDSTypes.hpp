#pragma once
/**
 * @file RGTransientDSTypes.hpp
 * @brief RHI-neutral authoring PODs for transient descriptor sets
 *        (RGBuilder::createTransientDS / RGPassBuilder::bindTransientDS).
 *
 * Part of the SDK three-tier surface: descriptor-write is expressed with NEUTRAL
 * enums (EDescriptorType / EImageLayout) so this public header carries NO
 * <vulkan/vulkan.h> dependency. The only Vulkan touch is VkSampler, a forward-
 * declared opaque handle (core/vk_fwd.hpp). The render backend converts the
 * neutral enums to Vk* when it materialises the VkWriteDescriptorSet
 * (see RGVulkanRecorder).
 */

#include <cstdint>
#include <lux/engine/render/core/vk_fwd.hpp>                   // VkSampler (forward-declared handle)
#include <lux/engine/function/render/graph/RGForwardDecls.hpp> // RGResourceHandle

namespace lux::render
{
    /// Neutral mirror of VkDescriptorType (the subset the render graph uses).
    enum class EDescriptorType : uint8_t
    {
        SAMPLER,
        COMBINED_IMAGE_SAMPLER,
        SAMPLED_IMAGE,
        STORAGE_IMAGE,
        UNIFORM_BUFFER,
        STORAGE_BUFFER,
        UNIFORM_BUFFER_DYNAMIC,
        STORAGE_BUFFER_DYNAMIC,
        INPUT_ATTACHMENT,
    };

    /// Neutral mirror of VkImageLayout (the subset descriptor writes use).
    enum class EImageLayout : uint8_t
    {
        UNDEFINED,
        GENERAL,
        SHADER_READ_ONLY_OPTIMAL,
        DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        /// KHR_dynamic_rendering_local_read: input-attachment descriptors read
        /// inside a local-read merged scope.
        RENDERING_LOCAL_READ,
    };

    /// Describes a single descriptor write referencing a render-graph resource.
    /// Used at graph-build time; the actual VkImageView / VkBuffer is resolved
    /// per-view per-frame when the recorder allocates the transient DS.
    struct RGDescriptorWrite
    {
        uint32_t binding;
        EDescriptorType descriptor_type;
        RGResourceHandle resource; ///< RG transient or imported resource
        VkSampler sampler = {};    ///< {} == VK_NULL_HANDLE
        EImageLayout image_layout = EImageLayout::SHADER_READ_ONLY_OPTIMAL;
        uint32_t mip_level = ~0u; ///< ~0u = full-mip view; else a single per-mip view
    };

    /// Opaque handle referencing a transient DS description in the graph.
    struct RGTransientDSHandle
    {
        uint32_t index = ~0u;
        [[nodiscard]] bool valid() const noexcept
        {
            return index != ~0u;
        }
    };

} // namespace lux::render
