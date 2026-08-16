#pragma once
#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
#include <lux/engine/render/gpu/pipeline/GraphicsPipelineTemplate.hpp>
#include <cstdint>
#include <string>
#include <lux/engine/function/visibility.h>

namespace lux::render
{
    /**
     * @brief Depth pre-pass feature — writes depth only, no colour output.
     *
     * Reduces overdraw in subsequent forward passes by establishing an
     * early Z buffer.  Uses the forward vertex shader + a depth-only
     * fragment shader.
     */
    class LUX_FUNCTION_PUBLIC DepthPrepassFeature : public RenderFeature
    {
    public:
        struct Config
        {
            ShaderHandle vertex_shader{};
            ShaderHandle fragment_shader{};
            std::string depth_target{"SceneDepth"};
        };

        explicit DepthPrepassFeature(Config cfg);
        ~DepthPrepassFeature() override = default;

        std::string_view name() const override { return "DepthPrepass"; }
        lux::render::Expected<void> initAndAttachTo(RenderScene& scene) override;

        void addPasses(RGBuilder& builder) override;

        GraphicsPipelineHandle pipelineHandle() const noexcept { return pipeline_handle_; }

    private:
        Config cfg_{};
        GraphicsPipelineHandle pipeline_handle_{kInvalidPipelineHandle};
    };

} // namespace lux::render
