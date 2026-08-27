#pragma once
/**
 * @file PCFeatureLOD.hpp
 * @brief Point cloud render feature — LOD mode (Mode 3).
 *
 * Extends GPU-driven indirect rendering with perspective-correct point sizing:
 * each point's screen-space radius is derived from its world-space radius and the
 * view depth, so distant points shrink naturally (LOD-like behaviour without a
 * separate LOD hierarchy).
 *
 * Differences from PCFeatureGPUDriven (Mode 2):
 *   - Vertex shader: pointcloud_lod.vert (computes gl_PointSize from clip.w)
 *   - Push constant: {float point_size_world, float min_size, float max_size} (12 bytes)
 *
 * @see PCFeatureIndirectBase for shared compute infrastructure
 * @see PCFeatureGPUDriven    (Mode 2 — fixed screen-space point size)
 * @see PCFeatureSplatting    (Mode 4 — Gaussian soft-splat)
 */

#include <lux/engine/render/renderer/features/point_cloud/PCFeatureIndirectBase.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
#include <lux/engine/function/visibility.h>
#include <atomic>
#include <string>

namespace lux::render
{
    /**
     * @brief Depth-scaled LOD point cloud feature (EPointCloudMode::LOD, Mode 3).
     *
     * Push-constant controls the world-space point radius and clamping range.
     */
    class LUX_FUNCTION_PUBLIC PCFeatureLOD final : public PCFeatureIndirectBase
    {
    public:
        struct Config
        {
            ShaderHandle compute_shader{};  ///< pointcloud_culling.comp
            ShaderHandle vertex_shader{};   ///< pointcloud_lod.vert
            ShaderHandle fragment_shader{}; ///< pointcloud_simple.frag
            float point_size_world{0.05f};  ///< world-space point radius (m)
            float min_size{1.0f};           ///< minimum screen pixels
            float max_size{20.0f};          ///< maximum screen pixels
            uint32_t max_nodes{65536};
            std::string color_target{"SceneColor"};
            std::string depth_target{"SceneDepth"};
        };

        explicit PCFeatureLOD(Config cfg);

        [[nodiscard]] EPointCloudMode mode() const noexcept override
        {
            return EPointCloudMode::LOD;
        }

        [[nodiscard]] std::string_view name() const override
        {
            return "PointCloudLOD";
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
            float min_size{1.0f};
            float max_size{20.0f};
        };

        /// Runtime-mutable max_size clamp; world radius and min_size stay at Config.
        float max_size_{20.0f};
        Config cfg_{};
    };

} // namespace lux::render
