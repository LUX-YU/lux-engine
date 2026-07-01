#pragma once
/**
 * @file TrajectoryLineFeature.hpp
 * @brief Trajectory render feature — Line mode (LINE_STRIP_WITH_ADJACENCY / LINE_STRIP).
 *
 * Renders every trajectory slot from TrajectoryGlobalBuffer as a LINE_STRIP draw.
 * The simplest/fastest mode; no compute expansion needed.
 *
 * @see ITrajectoryFeature
 * @see TrajectoryResources
 */

#include <lux/engine/render/renderer/features/trajectory/ITrajectoryFeature.hpp>
#include <lux/engine/render/core/ResourceHandle.hpp>
#include <lux/engine/render/renderer/RenderObjectExtraction.hpp>
#include <lux/engine/render/pipeline/GraphicsPipelineTemplate.hpp>
#include <cstdint>
#include <string>
#include <vector>
#include <lux/engine/function/visibility.h>

namespace lux::render
{

class TrajectoryGlobalBuffer;

class LUX_FUNCTION_PUBLIC TrajectoryLineFeature final : public ITrajectoryFeature
{
public:
    static constexpr phase_mask_t kExtractPhaseMask =
        phaseBit(static_cast<render_phase_id>(ECoreRenderPhase::ForwardTrans));

    struct Config
    {
        ShaderHandle vertex_shader{};
        ShaderHandle fragment_shader{};
        uint32_t max_global_vertices{1'000'000};
        std::string color_target{"SceneColor"};
        std::string depth_target{"SceneDepth"};
    };

    explicit TrajectoryLineFeature(Config cfg);
    ~TrajectoryLineFeature() override = default;

    // -----------------------------------------------------------------------
    //  ITrajectoryFeature
    // -----------------------------------------------------------------------
    [[nodiscard]] TrajectoryMode mode() const noexcept override
    {
        return TrajectoryMode::Line;
    }

    // -----------------------------------------------------------------------
    //  RenderFeature
    // -----------------------------------------------------------------------
    [[nodiscard]] std::string_view name() const override { return "TrajectoryLine"; }
    lux::render::Expected<void> initAndAttachTo(RenderScene& scene) override;
    void extractRenderObjects(
        const RenderObjectExtractContext& ctx,
        std::vector<DrawPacket>& out_packets);
    void addPasses(RGBuilder& builder) override;
    void onFrameBegin(const FeatureFrameContext& ctx) override;

private:
    GraphicsPipelineHandle pipeline_handle_{kInvalidPipelineHandle};

    // Non-owning — points into TrajectoryResources (outlives this feature).
    TrajectoryGlobalBuffer* global_buf_{nullptr};
    Config cfg_{};
};

} // namespace lux::render
