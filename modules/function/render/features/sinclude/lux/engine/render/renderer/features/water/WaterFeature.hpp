#pragma once

#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/function/render/client/features/water/WaterOperation.hpp>
#include <lux/engine/render/renderer/features/postprocess/FogFeature.hpp>
#include <lux/engine/function/render/client/core/PipelineHandle.hpp>
#include <lux/engine/function/render/client/RenderTargetLayout.hpp>
#include <lux/engine/function/render/graph/RGForwardDecls.hpp>
#include <lux/engine/function/visibility.h>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace lux::render
{
    class LUX_FUNCTION_PUBLIC WaterFeature final : public RenderFeature
    {
    public:
        struct Config final
        {
            ShaderHandle vertex_shader{};
            ShaderHandle fragment_shader{};
            std::uint32_t maximum_surfaces{256u};
        };

        explicit WaterFeature(Config config) noexcept;

        [[nodiscard]] std::string_view name() const override
        {
            return "Water";
        }

        [[nodiscard]] std::uint32_t requiredTargetSlots() const override
        {
            return 1u << static_cast<std::uint32_t>(TargetSlot::LINEAR_DEPTH);
        }

        Expected<void> initAndAttachTo(RenderScene&) override;
        void onDetachFromScene(RenderScene&) override;
        void onFrameBegin(const FeatureFrameContext&) override;
        void addPasses(RGBuilder& builder) override;
        [[nodiscard]] bool canRebaseSceneOrigin(const std::int64_t origin_delta[3]) const noexcept override;
        void rebaseSceneOrigin(const std::int64_t origin_delta[3]) noexcept override;

        [[nodiscard]] WaterSurfaceCreatedReply createSurface(const WaterSurfaceDesc& surface) noexcept;
        void updateSurface(RWaterSurfaceHandle handle, const WaterSurfaceDesc& surface) noexcept;
        void destroySurface(RWaterSurfaceHandle handle) noexcept;
        [[nodiscard]] WaterStatsReply stats() const noexcept;

    private:
        struct SurfaceSlot final
        {
            WaterSurfaceDesc surface{};
            std::uint32_t generation{1u};
            float transition_start{0.0f};
            float transition_duration{0.35f};
            float start_coverage{0.0f};
            float target_coverage{1.0f};
            float coverage{0.0f};
            bool alive{false};
            bool retiring{false};
        };

        struct alignas(16) GpuSurface final
        {
            float basis_local[12]{};
            std::int32_t page_delta[4]{};
            float half_rough_normal[4]{};
            float absorption_distance[4]{};
            float scroll_wave[4]{};
            float coverage_time[4]{};
            std::uint32_t texture_seed_flags[4]{};
        };
        static_assert(sizeof(GpuSurface) == 144u);

        struct GpuHeader final
        {
            std::uint32_t surface_count{0u};
            std::uint32_t reserved[3]{};
        };
        static_assert(sizeof(GpuHeader) == 16u);

        [[nodiscard]] bool valid(RWaterSurfaceHandle handle) const noexcept;
        [[nodiscard]] float coverageAt(const SurfaceSlot& slot, float scene_time) const noexcept;
        void rebuildGpuSnapshot(float scene_time);

        Config config_{};
        std::vector<SurfaceSlot> slots_;
        std::vector<std::uint32_t> free_slots_;
        std::vector<std::byte> gpu_upload_;
        FogFeature::RenderState fog_{};
        GraphicsPipelineHandle pipeline_{kInvalidPipelineHandle};
        VkDescriptorSetLayout input_layout_{VK_NULL_HANDLE};
        std::uint32_t input_slot_{1u};
        VkSampler color_sampler_{VK_NULL_HANDLE};
        VkSampler depth_sampler_{VK_NULL_HANDLE};
        std::uint32_t visible_patches_{0u};
        std::uint32_t transitioning_surfaces_{0u};
    };
} // namespace lux::render
