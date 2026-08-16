#pragma once
// ============================================================================
//  Grid2DPassFeature — the 2D scene reference grid (XY content plane).
//
//  One pipeline (fullscreen triangle, alpha blend, no depth — the 2D path
//  has no depth attachment, matching Canvas2D), TWO passes so the draw order
//  (on top of vs. under the content) is runtime-selectable with a STABLE
//  render-graph topology:
//    "Grid2DUnder" (Transparent stage) — draws when !onTop: grid sits under
//                  the Canvas2D content (Overlay stage comes later);
//    "Grid2DOver"  (Overlay stage, registered after Canvas2D in the plan so
//                  the write-after-write tie-break orders it later) — draws
//                  when onTop.
//  The inactive pass simply records nothing that frame.
// ============================================================================

#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
#include <lux/engine/render/gpu/pipeline/GraphicsPipelineTemplate.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <optional>
#include <string>

namespace lux::render
{
    /// Push-constant params (after the framework's scene/view indices).
    struct Grid2DParams
    {
        float cellSize   = 1.0f;   ///< world units per minor cell
        float majorEvery = 10.0f;  ///< minor cells per major line
        float linePx     = 1.0f;   ///< line width in pixels
        std::uint32_t onTop = 0;   ///< 1 = over the content, 0 = under it
    };

    class LUX_FUNCTION_PUBLIC Grid2DPassFeature : public RenderFeature
    {
    public:
        struct Config
        {
            ShaderHandle vertex_shader{};
            ShaderHandle fragment_shader{};
            std::string  color_target{"SceneColor"};
        };

        explicit Grid2DPassFeature(Config cfg);

        std::string_view name() const override { return "Grid2DPass"; }
        lux::render::Expected<void> initAndAttachTo(RenderScene& scene) override;
        void addPasses(RGBuilder& builder) override;
        void onFrameBegin(const FeatureFrameContext& ctx) override;

        /// Direct parameter update (used by the comm handler).
        void setGrid2DParams(const Grid2DParams& params) { pending_params_ = params; }

    private:
        Config                      cfg_{};
        GraphicsPipelineHandle      pipeline_{kInvalidPipelineHandle};
        Grid2DParams                params_{};
        std::optional<Grid2DParams> pending_params_;
    };

} // namespace lux::render
