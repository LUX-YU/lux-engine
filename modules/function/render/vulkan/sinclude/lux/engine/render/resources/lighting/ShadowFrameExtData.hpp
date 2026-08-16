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
#include <lux/engine/render/resources/mesh/GpuDrivenMeshConsts.hpp>  // kMaxShadowBiasGroups(唯一真相源)
#include <vulkan/vulkan.h>
#include <cstdint>
#include <array>

namespace lux::render
{
    /// Per-frame shadow rendering data for the extension slot mechanism.
    struct ShadowFrameExtData
    {
        /// 转发自唯一真相源。这里与 ShadowFrameData::kMaxBiasGroups、
        /// kMaxShadowBiasGroups 描述的是**同一批 GPU lane**:
        /// ShadowKernels.cpp 一处按 kMaxShadowBiasGroups clamp、另一处按本常量
        /// 判界。此前三处各写各的 `= 8`,改一处不改另两处就是越界写或静默丢 lane。
        static constexpr uint32_t kMaxBiasGroups = kMaxShadowBiasGroups;

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
