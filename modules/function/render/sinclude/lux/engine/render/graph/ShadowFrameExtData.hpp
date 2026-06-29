#pragma once
/**
 * @file ShadowFrameExtData.hpp
 * @brief Per-frame shadow data injected into RGFrameContext via extension slot.
 *
 * Replaces the hardcoded shadow fields in RGFrameContext. Populated by
 * MeshShadowFeature::onFrameBegin() and read by shadow kernel replay
 * functions during ExecutionProgram replay.
 */

#include <lux/engine/render/graph/FrameExtensionRegistry.hpp>
#include <vulkan/vulkan.h>
#include <cstdint>
#include <array>

namespace lux::render
{
    /// Per-frame shadow rendering data for the extension slot mechanism.
    struct ShadowFrameExtData
    {
        static constexpr uint32_t kMaxBiasGroups = 8;

        struct DrawLane
        {
            VkRect2D     scissor{};
            float        depth_bias{0.0f};
            float        slope_bias{0.0f};
            bool         active{false};
        };

        std::array<DrawLane, kMaxBiasGroups> draw_lanes{};
        uint32_t        group_count{0};
        uint32_t        slice_count{0};
        VkBuffer        cull_ubo{VK_NULL_HANDLE};
        const void*     frustum_data{nullptr};
        uint32_t        frustum_size{0};
        const void*     group_map_data{nullptr};
        uint32_t        group_map_size{0};
    };

    /// Global slot ID for shadow frame extension data.
    /// Assigned at static-init time by LUX_REGISTER_FRAME_EXTENSION.
    LUX_FUNCTION_PUBLIC FrameExtensionSlotId shadowFrameExtSlot() noexcept;

} // namespace lux::render
