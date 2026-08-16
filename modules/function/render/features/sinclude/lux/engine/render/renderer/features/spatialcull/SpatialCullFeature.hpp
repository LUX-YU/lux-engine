#pragma once
/**
 * @file SpatialCullFeature.hpp
 * @brief Spatial-cell-grid coarse-cull feature (owns the SpatialCullGrid).
 *
 * This is the DECOUPLED home of the spatial coarse-cull mechanism. The render
 * core (RenderScene / GpuDrivenMeshFeatureBase) knows nothing about SpatialCullGrid;
 * it only knows a domain-neutral primitive — the per-frame "instance cull-mask
 * GPU address" on the scene (RenderScene::instanceCullMaskAddress). This feature:
 *   - initAndAttachTo: emplaces + inits the SpatialCullGrid in the scene registry
 *     (the PointCloud/Trajectory feature-owned-resource pattern).
 *   - onFrameBegin:    classifies cells active/dormant from the active-view
 *     cameras, uploads the per-slot mask, and publishes its GPU address into the
 *     scene primitive (scene.setInstanceCullMaskAddress).
 *   - addPasses:       none — the grid is a host-mapped buffer read by the GPU
 *     cull via buffer-device-address, not an RG pass.
 *
 * Adding this feature IS the opt-in to spatial coarse cull. A scene that doesn't
 * add it pays nothing (no GPU mask ring, no per-frame cell update) and the cull
 * sees a null mask address (= every instance active). No Config flag, no
 * `if (domain)`. Large-world streaming is the canonical client, but anything that
 * wants distance/cell-based coarse cull can use it.
 *
 * See .internal/render-architecture-decoupling-design-2026-06-19.md (§4, category A;
 * Contract C2 — scene resources are feature-owned; C4 — domain-neutral scene primitives).
 */

#include <lux/engine/render/SceneFeature.hpp>
#include <lux/engine/function/render/client/features/spatialcull/SpatialCullParams.hpp>
#include <lux/engine/function/visibility.h>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace lux::render
{
    class SpatialCullGrid;
    class InstanceResources;
    class ViewCameraResource;

    class LUX_FUNCTION_PUBLIC SpatialCullFeature final : public SceneFeature
    {
    public:
        struct Config
        {
            std::string name{"SpatialCull"};
            float       cell_size{128.0f};      ///< cell edge length (world units)
            float       cull_distance{512.0f};  ///< cull distance (cells farther than this from the camera go dormant)
        };

        SpatialCullFeature();
        explicit SpatialCullFeature(Config cfg);

        lux::render::Expected<void> initAndAttachTo(RenderScene& scene) override;
        void onDetachFromScene(RenderScene& scene) override;
        void onFrameBegin(const FeatureFrameContext& ctx) override;

        // --- Feature-driven quality seam (settings panel; all params are HOT) ---
        [[nodiscard]] std::string_view paramStructName() const override
        {
            return "lux::render::SpatialCullParams";
        }
        [[nodiscard]] void*       paramData() noexcept override       { return &params_; }
        [[nodiscard]] std::size_t paramSize() const noexcept override { return sizeof(SpatialCullParams); }
        EParamApply applyParams(const void* src, std::size_t size) override;

    private:
        SpatialCullParams params_{};                        ///< live, render-thread-owned tunable params
        std::vector<std::array<float, 3>> camera_scratch_;  ///< reused each frame
        /// 自建资源:本 feature 在 initAndAttachTo 里 emplace 它,attach 后恒非空。
        SpatialCullGrid*    grid_{nullptr};
        /// 记忆化:InstanceResources 归 StandardMeshStack 所有,而本 feature 没有
        /// 声明 requires= —— 没有 mesh 栈的场景装本 feature 是合法的(粗剔除对空
        /// 实例表 no-op),所以判空保留、只把每帧查找收敛掉。
        InstanceResources*  inst_cache_{nullptr};
        /// 可选相机资源的记忆化缓存(见 resolveViewCameraOnce)。
        ViewCameraResource* cam_cache_{nullptr};
    };

    // No-arg ctor defined out-of-class so Config{} is evaluated where the class is
    // complete (GCC 11/12 reject Config{} / {} as an in-class default argument).
    inline SpatialCullFeature::SpatialCullFeature() : SpatialCullFeature(Config{}) {}

} // namespace lux::render
