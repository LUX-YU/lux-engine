#pragma once

#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/function/render/client/features/postprocess/FogOperation.hpp>
#include <lux/engine/function/render/client/core/PipelineHandle.hpp>
#include <lux/engine/function/render/client/RenderTargetLayout.hpp>
#include <lux/engine/function/render/graph/RGForwardDecls.hpp>
#include <lux/engine/function/visibility.h>

#include <vulkan/vulkan.h>

namespace lux::render
{
    class LUX_FUNCTION_PUBLIC FogFeature final : public RenderFeature
    {
    public:
        struct RenderState final
        {
            float color_r{0.55f};
            float color_g{0.62f};
            float color_b{0.70f};
            float density{0.0002f};
            float start_distance{0.0f};
            float reference_height{0.0f};
            float height_falloff{0.01f};
            float maximum_opacity{0.98f};
            std::uint32_t enabled{0u};
        };
        static_assert(sizeof(RenderState) == 36u);

        struct Config
        {
            ShaderHandle vertex_shader{};
            ShaderHandle fragment_shader{};
        };

        explicit FogFeature(Config config) noexcept
            : config_(config)
        {}

        [[nodiscard]] std::string_view name() const override
        {
            return "Fog";
        }

        [[nodiscard]] std::uint32_t requiredTargetSlots() const override
        {
            return 1u << static_cast<std::uint32_t>(
                TargetSlot::LINEAR_DEPTH);
        }

        Expected<void> initAndAttachTo(RenderScene&) override;
        void onDetachFromScene(RenderScene&) override;
        void addPasses(RGBuilder& builder) override;
        void setParams(const FogSetParamsPayload& params) noexcept;
        [[nodiscard]] const RenderState& renderState() const noexcept
        {
            return params_;
        }

    private:
        Config config_{};
        RenderState params_{};
        GraphicsPipelineHandle pipeline_{kInvalidPipelineHandle};
        VkDescriptorSetLayout input_layout_{VK_NULL_HANDLE};
        VkSampler color_sampler_{VK_NULL_HANDLE};
        VkSampler depth_sampler_{VK_NULL_HANDLE};
    };
} // namespace lux::render
