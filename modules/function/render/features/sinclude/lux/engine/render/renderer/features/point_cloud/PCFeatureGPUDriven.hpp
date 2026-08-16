#pragma once
/**
 * @file PCFeatureGPUDriven.hpp (refactored)
 * @brief Point cloud render feature — GPU-Driven mode (compute culling + indirect draw).
 *
 * Inherits all compute-side GPU infrastructure from PCFeatureIndirectBase.
 * This class is responsible only for the graphics draw pass: one
 * vkCmdDrawIndirectCount call using the indirect buffer filled by the compute pass.
 *
 * @see PCFeatureIndirectBase for compute/descriptor management
 * @see PCFeatureLOD         (Mode 3 — depth-scaled point size)
 * @see PCFeatureSplatting   (Mode 4 — Gaussian soft-splat)
 */

#include <lux/engine/render/renderer/features/point_cloud/PCFeatureIndirectBase.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
#include <lux/engine/function/visibility.h>
#include <atomic>
#include <string>

namespace lux::render
{
    /**
     * @brief GPU-Driven point cloud feature (EPointCloudMode::GPU_DRIVEN, Mode 2).
     *
     * Fixed screen-space point size (push constant), single indirect draw call.
     */
    class LUX_FUNCTION_PUBLIC PCFeatureGPUDriven final : public PCFeatureIndirectBase
    {
    public:
        struct Config
        {
            ShaderHandle     compute_shader{};     ///< pointcloud_culling.comp
            ShaderHandle     vertex_shader{};      ///< pointcloud_simple.vert
            ShaderHandle     fragment_shader{};    ///< pointcloud_simple.frag
            float        initial_point_size{3.0f};
            uint32_t     max_nodes{65536};
            std::string  color_target{"SceneColor"};
            std::string  depth_target{"SceneDepth"};
        };

        explicit PCFeatureGPUDriven(Config cfg);

        [[nodiscard]] EPointCloudMode mode() const noexcept override
        {
            return EPointCloudMode::GPU_DRIVEN;
        }

        [[nodiscard]] std::string_view name() const override { return "PointCloudGPUDriven"; }
        lux::render::Expected<void> initAndAttachTo(RenderScene& scene) override;

        void addPasses(RGBuilder& builder) override;

        void setPointSize(float pixels) noexcept override
        {
            point_size_ = pixels;
        }

    private:
        float              point_size_{3.0f};
        Config cfg_{};
    };

} // namespace lux::render
