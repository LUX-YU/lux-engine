#pragma once
/// @file RGBarrierUtils.hpp
/// @brief Shared resource state determination utilities for barrier computation.
///
/// Used by RenderGraphCompiler (compile-time).

#include <vulkan/vulkan.h>
#include <lux/engine/function/render/graph/RGEnums.hpp>
#include <lux/engine/render/graph/RGResourceTypes.hpp>
#include <lux/engine/render/graph/RGPassTypes.hpp>

namespace lux::render
{
    /// Tracks the synchronization state of a single resource during barrier computation.
    /// Used by RenderGraphCompiler at compile-time.
    struct ResourceStateTracker
    {
        VkImageLayout current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkPipelineStageFlags2 last_stage_mask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        VkAccessFlags2 last_access_mask = 0;
        bool touched = false;
    };

    struct VulkanResourceState
    {
        VkPipelineStageFlags2 stage_mask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        VkAccessFlags2 access_mask = 0;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    /// Check if an access mask contains any write operation
    inline bool isWriteAccess(VkAccessFlags2 flags)
    {
        return (flags & (VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
                         VK_ACCESS_2_HOST_WRITE_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT)) != 0;
    }

    /// The shader pipeline stage a pass runs its shader reads/writes in: a
    /// COMPUTE / ASYNC_COMPUTE pass orders against COMPUTE_SHADER, every other
    /// pass against FRAGMENT_SHADER. This choice is the SINGLE source of truth
    /// for both the intra-queue barriers (determineTextureState here) and the
    /// cross-queue semaphore sync stages (read/writeStageFor* in
    /// RenderGraphCompiler). A mismatch between the two would leave the EVSM blur
    /// compute passes' RAW hazard unsynchronized.
    inline VkPipelineStageFlags2 shaderStageForPass(ERGPassType pass_type)
    {
        return (pass_type == ERGPassType::COMPUTE || pass_type == ERGPassType::ASYNC_COMPUTE)
                   ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                   : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    }

    inline bool isDepthStencilFormat_shared(lux::rdesc::ETextureFormat format)
    {
        return format >= lux::rdesc::ETextureFormat::D16_UNORM;
    }

    inline bool hasStencilComponent_shared(lux::rdesc::ETextureFormat format)
    {
        return format == lux::rdesc::ETextureFormat::D16_UNORM_S8_UINT ||
               format == lux::rdesc::ETextureFormat::D24_UNORM_S8_UINT ||
               format == lux::rdesc::ETextureFormat::D32_SFLOAT_S8_UINT;
    }

    inline VulkanResourceState
    determineTextureState(const RGPassTextureRef& ref, const RGTextureDescription& desc, ERGPassType pass_type)
    {
        // Shader-stage for shader reads/writes depends on the pass type (see
        // shaderStageForPass): a COMPUTE pass that samples a texture must order
        // against COMPUTE_SHADER, not FRAGMENT_SHADER — the latter leaves the
        // compute read unsynchronized (a live RAW hazard in the EVSM blur passes).
        const VkPipelineStageFlags2 shader_stage = shaderStageForPass(pass_type);

        VulkanResourceState state{};
        state.stage_mask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        state.access_mask = 0;
        state.layout = VK_IMAGE_LAYOUT_UNDEFINED;

        switch (ref.role)
        {
        case lux::render::ETextureRole::COLOR_ATTACHMENT:
            state.stage_mask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            state.access_mask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            if (ref.usage == ERGResourceUsage::READ || ref.usage == ERGResourceUsage::READ_WRITE)
            {
                state.access_mask |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
            }
            state.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            break;

        case lux::render::ETextureRole::DEPTH_STENCIL_ATTACHMENT:
            state.stage_mask =
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            if (ref.usage == ERGResourceUsage::WRITE || ref.usage == ERGResourceUsage::READ_WRITE)
            {
                state.access_mask =
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
                state.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            }
            else
            {
                state.access_mask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
                state.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            }
            break;

        case lux::render::ETextureRole::SAMPLED:
            state.stage_mask = shader_stage;
            state.access_mask = VK_ACCESS_2_SHADER_READ_BIT;
            state.layout = isDepthStencilFormat_shared(desc.format) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                                    : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            break;

        case lux::render::ETextureRole::UNORDERED_ACCESS:
            state.stage_mask = shader_stage;
            state.access_mask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
            state.layout = VK_IMAGE_LAYOUT_GENERAL;
            break;

        case lux::render::ETextureRole::INPUT_ATTACHMENT:
            // Local-read merged scope only (line-B): tile-local reads keep the
            // attachment in RENDERING_LOCAL_READ for the whole scope.
            state.stage_mask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            state.access_mask = VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT;
            state.layout = VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ;
            break;
        }

        return state;
    }

    inline VulkanResourceState determineBufferState_shared(const RGPassBufferRef& ref, ERGPassType pass_type)
    {
        VulkanResourceState state{};
        state.layout = VK_IMAGE_LAYOUT_UNDEFINED;

        // A buffer reference in a transfer pass describes vkCmdUpdateBuffer /
        // vkCmdCopyBuffer / vkCmdFillBuffer access, not a shader access.  The
        // old role-only mapping silently produced SHADER_* barriers around
        // transfer writes, leaving persistent upload buffers racing their
        // previous-frame consumers.
        if (pass_type == ERGPassType::TRANSFER || pass_type == ERGPassType::ASYNC_TRANSFER)
        {
            state.stage_mask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            switch (ref.usage)
            {
            case ERGResourceUsage::READ:
                state.access_mask = VK_ACCESS_2_TRANSFER_READ_BIT;
                break;
            case ERGResourceUsage::WRITE:
                state.access_mask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                break;
            case ERGResourceUsage::READ_WRITE:
                state.access_mask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
                break;
            }
            return state;
        }

        switch (ref.role)
        {
        case ERGBufferRole::VERTEX:
            state.stage_mask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
            state.access_mask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
            break;
        case ERGBufferRole::INDEX:
            state.stage_mask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
            state.access_mask = VK_ACCESS_2_INDEX_READ_BIT;
            break;
        case ERGBufferRole::CONSTANT:
            state.stage_mask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            state.access_mask = VK_ACCESS_2_UNIFORM_READ_BIT;
            break;
        case ERGBufferRole::STORAGE:
            state.stage_mask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            if (ref.usage == ERGResourceUsage::WRITE || ref.usage == ERGResourceUsage::READ_WRITE)
            {
                state.access_mask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT;
            }
            else
            {
                state.access_mask = VK_ACCESS_2_SHADER_READ_BIT;
            }
            break;
        case ERGBufferRole::INDIRECT:
            state.stage_mask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
            state.access_mask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
            break;
        default:
            state.stage_mask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
            state.access_mask = 0;
            break;
        }

        return state;
    }

} // namespace lux::render
