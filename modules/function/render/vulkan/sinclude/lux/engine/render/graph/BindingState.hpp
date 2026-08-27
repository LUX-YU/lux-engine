#pragma once
/// @file BindingState.hpp
/// @brief Vulkan binding state tracker for VBO/IBO and pipeline dedup.
///
/// Tracks last-bound vertex/index buffers and pipeline handles to eliminate
/// redundant vkCmdBind* calls during command recording.
/// Descriptor set binding is handled entirely at compile time via bind_ds_mask.

#include <vulkan/vulkan.h>

namespace lux::render
{
    struct BindingState
    {
        // --- Vertex / index buffer dedup ---
        VkBuffer last_vbo{VK_NULL_HANDLE};
        VkDeviceSize last_vbo_offset{0};
        VkBuffer last_ibo{VK_NULL_HANDLE};
        VkDeviceSize last_ibo_offset{0};
        VkIndexType last_index_type{VK_INDEX_TYPE_MAX_ENUM};

        // --- Pipeline / layout dedup ---
        VkPipeline last_graphics_pipeline{VK_NULL_HANDLE};
        VkPipeline last_compute_pipeline{VK_NULL_HANDLE};
        VkPipelineLayout last_pipeline_layout{VK_NULL_HANDLE};

        void reset() noexcept
        {
            last_vbo = VK_NULL_HANDLE;
            last_vbo_offset = 0;
            last_ibo = VK_NULL_HANDLE;
            last_ibo_offset = 0;
            last_index_type = VK_INDEX_TYPE_MAX_ENUM;
            last_graphics_pipeline = VK_NULL_HANDLE;
            last_compute_pipeline = VK_NULL_HANDLE;
            last_pipeline_layout = VK_NULL_HANDLE;
        }
    };

} // namespace lux::render
