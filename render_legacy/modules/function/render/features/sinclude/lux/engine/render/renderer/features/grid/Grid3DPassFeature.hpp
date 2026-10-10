#pragma once
#include <lux/engine/function/render/features/visibility.h>
#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
#include <lux/engine/render/gpu/pipeline/GraphicsPipelineTemplate.hpp>
#include <lux/engine/function/render/features/grid/Grid3DPassTypes.hpp>
#include <cstdint>
#include <lux/engine/function/visibility.h>

#include <vector>
#include <string>

namespace lux::render
{
    class LUX_ENGINE_FUNCTION_RENDER_FEATURES_PUBLIC Grid3DPassFeature : public RenderFeature
    {
    public:
        struct Config
        {
            ShaderHandle vertex_shader{};
            ShaderHandle fragment_shader{};
            std::string color_target{"SceneColor"};
            std::string depth_target{"SceneDepth"};
        };

        explicit Grid3DPassFeature(Config cfg);

        std::string_view name() const override
        {
            return "Grid3DPass";
        }
        lux::render::Expected<void> initAndAttachTo(RenderScene& scene) override;

        GraphicsPipelineHandle gridHandle() const noexcept
        {
            return grid_handle_;
        }

        void addPasses(RGBuilder& builder) override;

        void setGrid3DParams(ViewHandle view, const Grid3DParams& params);
        void deallocateViewState(std::uint32_t view) override;
        [[nodiscard]] const Grid3DParams& params(ViewHandle view) const noexcept;

    private:
        Config cfg_{};
        GraphicsPipelineHandle grid_handle_{kInvalidPipelineHandle};
        Grid3DParams grid_params_{};
        struct ViewParams final
        {
            ViewHandle view;
            Grid3DParams params;
        };
        std::vector<ViewParams> view_params_;
    };

} // namespace lux::render
