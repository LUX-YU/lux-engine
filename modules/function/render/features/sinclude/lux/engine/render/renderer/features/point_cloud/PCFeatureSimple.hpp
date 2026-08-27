#pragma once
/**
 * @file PCFeatureSimple.hpp
 * @brief Point cloud render feature — Simple mode (global SSBO, multi-draw).
 *
 * Shared GPU resources (PointCloudGlobalBuffer) are fetched from
 * GPUResourceRegistry::getResource<PointCloudResources>() during construction.
 * If PointCloudResources is not yet initialized the constructor will
 * auto-initialize it using the values from Config.
 *
 * @see IPointCloudFeature
 * @see PointCloudResources
 */

#include <lux/engine/render/renderer/features/point_cloud/IPointCloudFeature.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
#include <lux/engine/render/gpu/pipeline/GraphicsPipelineTemplate.hpp>
#include <lux/engine/function/render/graph/RGEnums.hpp>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include <lux/engine/function/visibility.h>

#include <cstddef>
#include <memory>

namespace lux::render
{
    class PointCloudGlobalBuffer;

    class LUX_FUNCTION_PUBLIC PCFeatureSimple final : public IPointCloudFeature
    {
    public:
        static constexpr phase_mask_t kExtractPhaseMask =
            phaseBit(static_cast<render_phase_id>(ECoreRenderPhase::PointCloud));

        struct Config
        {
            ShaderHandle vertex_shader{};
            ShaderHandle fragment_shader{};
            float initial_point_size{3.0f};
            /// Used to auto-initialize PointCloudResources if not yet done.
            uint32_t max_global_points{4'000'000};
            uint32_t max_octree_nodes{65'536};
            std::string color_target{"SceneColor"};
            std::string depth_target{"SceneDepth"};
        };

        explicit PCFeatureSimple(Config cfg);

        [[nodiscard]] EPointCloudMode mode() const noexcept override
        {
            return EPointCloudMode::SIMPLE;
        }

        // -----------------------------------------------------------------------
        //  RenderFeature
        // -----------------------------------------------------------------------
        [[nodiscard]] std::string_view name() const override
        {
            return "PointCloudSimple";
        }
        lux::render::Expected<void> initAndAttachTo(RenderScene& scene) override;

        void addPasses(RGBuilder& builder) override;
        void onFrameBegin(const FeatureFrameContext& ctx) override;

        void setPointSize(float pixels) noexcept override
        {
            point_size_ = pixels;
        }

    private:
        GraphicsPipelineHandle pipeline_handle_{kInvalidPipelineHandle};
        float point_size_{3.0f};

        // Non-owning — points into PointCloudResources (outlives this feature).
        PointCloudGlobalBuffer* global_buf_{nullptr};
        Config cfg_{};
    };

} // namespace lux::render
