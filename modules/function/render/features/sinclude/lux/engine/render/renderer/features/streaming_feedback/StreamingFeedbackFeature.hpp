#pragma once

#include <lux/engine/render/renderer/features/GpuDrivenMeshFeatureBase.hpp>
#include <lux/engine/function/render/client/features/streaming_feedback/StreamingFeedbackOperation.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
#include <lux/engine/function/visibility.h>

#include <chrono>
#include <cstdint>
#include <string>

#include <vulkan/vulkan.h>

namespace lux::render
{
    /// Optional mesh-surface streaming overlay.  It owns an extra mask and
    /// fullscreen composite pass; standard material shaders stay branch-free.
    class LUX_FUNCTION_PUBLIC StreamingFeedbackFeature final :
        public GpuDrivenMeshFeatureBase
    {
    public:
        struct Config final
        {
            ShaderHandle cull_shader{};
            ShaderHandle compact_shader{};
            ShaderHandle mask_vert{};
            ShaderHandle mask_frag{};
            ShaderHandle composite_frag{};
            std::uint32_t descriptor_layout_version{0};
            GpuDrivenMeshExtFlags extension_flags{};
            std::string color_target{"SceneColor"};
            std::string mask_target{"StreamingFeedbackMask"};
            float tile_size{18.0f};
            float speed{1.6f};
            float intensity{0.72f};
            float color[3]{0.18f, 0.72f, 1.0f};
            EStreamingFeedbackPattern pattern{
                EStreamingFeedbackPattern::MOSAIC_DITHER};
        };

        explicit StreamingFeedbackFeature(Config config);
        ~StreamingFeedbackFeature() override;

        std::string_view name() const override
        {
            return "StreamingFeedback";
        }

        Expected<void> initAndAttachTo(RenderScene& scene) override;
        void onDetachFromScene(RenderScene& scene) override;
        void addPasses(RGBuilder& builder) override;

    private:
        Expected<void> init();
        void releaseAll() noexcept;

        Config config_;
        VkDescriptorSetLayout visible_set_layout_{VK_NULL_HANDLE};
        VkDescriptorSetLayout composite_set_layout_{VK_NULL_HANDLE};
        GraphicsPipelineHandle composite_pipeline_{kInvalidPipelineHandle};
        VkSampler mask_sampler_{VK_NULL_HANDLE};
        std::chrono::steady_clock::time_point start_time_{
            std::chrono::steady_clock::now()};
    };
}
