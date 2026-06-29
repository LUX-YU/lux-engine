#pragma once
#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/render/core/ResourceHandle.hpp>
#include <lux/engine/render/pipeline/GraphicsPipelineTemplate.hpp>
#include <lux/engine/render/renderer/features/grid/GridPassTypes.hpp>
#include <lux/engine/render/renderer/features/grid/GridSyncCommands.hpp>
#include <cstdint>
#include <lux/engine/function/visibility.h>

#include <optional>
#include <string>

namespace lux::render
{
    class LUX_FUNCTION_PUBLIC GridPassFeature : public RenderFeature
    {
    public:
        struct Config
        {
            ShaderHandle vertex_shader{};
            ShaderHandle fragment_shader{};
            std::string color_target{"SceneColor"};
            std::string depth_target{"SceneDepth"};
        };

        explicit GridPassFeature(Config cfg);

        std::string_view name() const override { return "GridPass"; }
        void initAndAttachTo(RenderScene& scene) override;

        GraphicsPipelineHandle gridHandle() const noexcept { return grid_handle_; }

        void addPasses(RGBuilder& builder) override;

        // --- Per-frame feature phases ---------------------------------------
        void onFrameBegin(const FeatureFrameContext& ctx) override;

    public:
        // Sync handler — public for generated auto-registration.
        void onSyncSetGridParams(const SetGridParamsCmd& cmd, const FeatureFrameContext& ctx);

        /// Direct parameter update (used by comm handler).
        void setGridParams(const GridParams& params) { pending_grid_params_ = params; }

    private:
        Config                      cfg_{};
        GraphicsPipelineHandle      grid_handle_{kInvalidPipelineHandle};
        GridParams                  grid_params_{};
        std::optional<GridParams>   pending_grid_params_;
    };

} // namespace lux::render
