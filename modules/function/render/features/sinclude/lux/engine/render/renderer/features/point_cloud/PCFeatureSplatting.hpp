#pragma once
/**
 * @file PCFeatureSplatting.hpp
 * @brief Point cloud render feature — Gaussian soft-splat mode (Mode 4).
 *
 * Shares the same depth-scaled vertex shader (pointcloud_lod.vert) as Mode 3,
 * but replaces the fragment shader with pointcloud_splat.frag which applies a
 * Gaussian falloff to each point image, producing smooth circular splats that
 * blend into a continuous surface when the point density is high.
 *
 * Pipeline differences from PCFeatureLOD (Mode 3):
 *   - Fragment shader:      pointcloud_splat.frag  (Gaussian alpha falloff)
 *   - Alpha blending:       enabled  (SRC_ALPHA / ONE_MINUS_SRC_ALPHA)
 *   - Depth write:          disabled (splats are semi-transparent)
 *   - Depth test:           enabled  (correct occlusion, no depth writes)
 *
 * Push constant: same LodPC {float point_size_world, float min_size, float max_size}.
 *
 * @see PCFeatureIndirectBase for shared compute infrastructure
 * @see PCFeatureLOD          (Mode 3 — solid depth-scaled points)
 */

#include <lux/engine/render/renderer/features/point_cloud/PCFeatureIndirectBase.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
#include <lux/engine/function/visibility.h>
#include <atomic>
#include <string>

namespace lux::render
{
    /**
     * @brief Gaussian soft-splat point cloud feature (EPointCloudMode::SPLATTING, Mode 4).
     */
    class LUX_FUNCTION_PUBLIC PCFeatureSplatting final : public PCFeatureIndirectBase
    {
    public:
        struct Config
        {
            ShaderHandle compute_shader{};  ///< pointcloud_culling.comp
            ShaderHandle vertex_shader{};   ///< pointcloud_lod.vert
            ShaderHandle fragment_shader{}; ///< pointcloud_splat.frag
            float point_size_world{0.05f};  ///< world-space point radius (m)
            float min_size{2.0f};           ///< minimum screen pixels (larger than LOD for splat)
            float max_size{30.0f};          ///< maximum screen pixels
            uint32_t max_nodes{65536};
            std::string color_target{"SceneColor"};
            std::string depth_target{"SceneDepth"};
        };

        explicit PCFeatureSplatting(Config cfg);

        [[nodiscard]] EPointCloudMode mode() const noexcept override
        {
            return EPointCloudMode::SPLATTING;
        }

        [[nodiscard]] std::string_view name() const override
        {
            return "PointCloudSplatting";
        }
        lux::render::Expected<void> initAndAttachTo(RenderScene& scene) override;

        void addPasses(RGBuilder& builder) override;

        void setPointSize(float pixels) noexcept override
        {
            max_size_ = pixels;
        }

    private:
        struct LodPC
        {
            float point_size_world{0.05f};
            float min_size{2.0f};
            float max_size{30.0f};
        };

        /// Runtime-mutable max_size clamp; world radius and min_size stay at Config.
        float max_size_{30.0f};
        Config cfg_{};
    };

} // namespace lux::render
