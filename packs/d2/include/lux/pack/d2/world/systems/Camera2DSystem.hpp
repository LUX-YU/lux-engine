#pragma once
// ============================================================================
//  Camera2DSystem.hpp — derive 2D orthographic view/proj (lux::pack).
//
//  Mirrors d3::CameraSystem: pose comes from WorldTransform2D (NOT the camera
//  component), projection math reuses the engine's Ortho/LookAt helpers (not
//  rewritten). Writes a system-generated Camera2DCacheComponent, auto-maintained on
//  any Camera2D entity (G-07 pattern). A 2D camera always looks down -Z (the plane);
//  its Transform2D rotation is a roll (rotates the up vector). Scale baked into the
//  world matrix is de-scaled (a scaled camera transform must not distort the view;
//  the single zoom control is Camera2DComponent::units_per_view_height, not scale).
//  Parallax is NOT consumed here (it is a per-layer property applied at draw-batch
//  time — design §4).
// ============================================================================

#include <lux/engine/ecs/systems/ISystem.hpp>
#include <lux/engine/ecs/systems/HierarchicalTransformSystem.hpp>   // maintainDerived
#include "../components/Camera2DComponent.hpp"
#include "../components/Camera2DCacheComponent.hpp"
#include "../components/WorldTransform2DComponent.hpp"

#include <lux/engine/math/eigen_extend.hpp>
#include <Eigen/Core>
#include <cassert>
#include <cmath>
#include <vector>
#include <lux/engine/ecs/World.hpp>

namespace lux::pack
{
    using lux::ecs::World;
    using lux::ecs::connectDerivedMaintenance;

    class Camera2DSystem final : public lux::ecs::ISystem
    {
    public:
        void update(lux::meta::EntityRegistry& registry, float /*dt*/) override
        {
            // G-07: the derived cache is maintained by entt construct/destroy signals
            // (a Camera2D entity gets a Camera2DCache; a cache whose Camera2D was
            // removed is dropped). Wired ONCE on the first update + a one-time
            // backfill, then zero per-frame cost. See connectDerivedMaintenance.
            if (!cache_maintenance_connected_)
            {
                connectDerivedMaintenance<Camera2DComponent, Camera2DCacheComponent>(registry, scratch_);
                cache_maintenance_connected_ = true;
#ifndef NDEBUG
                maintenance_registry_ = &registry;
#endif
            }
#ifndef NDEBUG
            assert(maintenance_registry_ == &registry &&
                   "Camera2DSystem reused across registries: its G-07 signal "
                   "maintenance is wired to a different one");
#endif

            auto view = registry.view<WorldTransform2DComponent, Camera2DComponent, Camera2DCacheComponent>();
            view.each([](const WorldTransform2DComponent& wc, const Camera2DComponent& cc, Camera2DCacheComponent& cache)
            {
                const Eigen::Vector3f position = wc.world.block<3, 1>(0, 3);

                // Up = the camera's rotated +Y, de-scaled (normalized) so a scaled
                // transform doesn't skew the view. Forward is fixed to -Z (the 2D plane).
                Eigen::Vector3f up = wc.world.block<3, 3>(0, 0).col(1);
                const float n = up.norm();
                if (n > 1e-6f) up /= n;
                const Eigen::Vector3f forward(0.f, 0.f, -1.f);
                const Eigen::Vector3f target = position + forward;

                LuxEigenExt::Affine3<float> view_affine =
                    LuxEigenExt::TLookAt<Eigen::Vector3f, Eigen::Vector3f,
                                         Eigen::Vector3f, true>(position, target, up);

                // Orthographic extents from the single base scale + aspect.
                float half_h = cc.units_per_view_height * 0.5f;
                float half_w = half_h * cc.aspect;
                switch (cc.scale_mode)
                {
                case Camera2DScaleMode::FitWidth:
                    half_w = cc.units_per_view_height * 0.5f;
                    half_h = (cc.aspect > 1e-6f) ? half_w / cc.aspect : half_w;
                    break;
                case Camera2DScaleMode::Stretch:
                    half_w = half_h = cc.units_per_view_height * 0.5f;
                    break;
                case Camera2DScaleMode::FitHeight:
                default:
                    break;   // half_h/half_w already set
                }

                cache.prev_view_proj = cache.view_proj;
                cache.view = view_affine.matrix();
                cache.proj = LuxEigenExt::TOrthographicProjection<float>(
                    -half_w, half_w, -half_h, half_h, kNearZ, kFarZ);
                if (cc.y_flip) cache.proj(1, 1) = -cache.proj(1, 1);   // Vulkan Y-down NDC (mirrors d3)
                cache.view_proj = cache.proj * cache.view;
            });
        }

    private:
        // 2D layers live in a wide symmetric z band (layer/int16 range) so anything on
        // the plane (z = 0) is always within the ortho frustum; draw order is by layer,
        // not depth.
        static constexpr float kNearZ = -1024.0f;
        static constexpr float kFarZ  =  1024.0f;

        /// Reused scratch for the ONE-TIME derived-cache-maintenance backfill.
        std::vector<lux::meta::entity_id> scratch_;
        /// G-07 signals are wired lazily on the first update.
        bool cache_maintenance_connected_{false};
#ifndef NDEBUG
        const void* maintenance_registry_{nullptr};   // 1:1-registry tripwire
#endif
    };

    // ── Camera helpers (T2-02/T2-03) ──

    /// The scene's active camera: the SINGLE entity carrying ActiveCamera2DTag (+ a
    /// Camera2DComponent). Returns null_entity when there is zero (a Canvas can then
    /// skip rendering rather than read garbage) OR more than one (ambiguous). NEVER the
    /// implicit "first camera" — selection is explicit (design T2-03).
    [[nodiscard]] inline lux::meta::entity_id activeCamera(lux::meta::EntityRegistry& reg)
    {
        lux::meta::entity_id found = lux::meta::null_entity;
        int count = 0;
        for (auto e : reg.view<ActiveCamera2DTag, Camera2DComponent>()) { found = e; ++count; }
        return count == 1 ? found : lux::meta::null_entity;
    }

    /// The camera gate every 2D PRODUCER bridge shares (P0-5): the single active camera,
    /// but only once its derived Camera2DCacheComponent exists (i.e. Camera2DSystem has
    /// run at least once), so callers never publish or frame content against a camera
    /// whose view/proj was never derived. Returns null_entity otherwise. Note what this
    /// deliberately does NOT check: whether the render side can resolve camera-upload
    /// ops — that is the upload bridge's own concern (a headless / feature-less scene
    /// still submits draw content; pinned by sprite_slice_a_test).
    [[nodiscard]] inline lux::meta::entity_id publishableCamera(lux::meta::EntityRegistry& reg)
    {
        const lux::meta::entity_id cam = activeCamera(reg);
        if (cam == lux::meta::null_entity) return lux::meta::null_entity;
        return reg.all_of<Camera2DCacheComponent>(cam) ? cam : lux::meta::null_entity;
    }

    /// World point on the z=0 plane → screen pixel via a camera's view_proj. @p viewport
    /// is (width, height) in pixels; the mapping honours the camera's y_flip (both helpers
    /// use the same convention, so they round-trip).
    [[nodiscard]] inline Eigen::Vector2f worldToScreen(
        const Camera2DCacheComponent& cache, const Eigen::Vector2f& viewport, const Eigen::Vector2f& world)
    {
        const Eigen::Vector4f n = cache.view_proj * Eigen::Vector4f(world.x(), world.y(), 0.f, 1.f);
        const float ndc_x = n.x() / n.w();
        const float ndc_y = n.y() / n.w();
        return Eigen::Vector2f((ndc_x * 0.5f + 0.5f) * viewport.x(),
                               (ndc_y * 0.5f + 0.5f) * viewport.y());
    }

    /// Inverse of worldToScreen: screen pixel → world point on the z=0 plane. (Ortho maps
    /// world z linearly, so every z=0 point shares one NDC z — recovered from the origin.)
    [[nodiscard]] inline Eigen::Vector2f screenToWorld(
        const Camera2DCacheComponent& cache, const Eigen::Vector2f& viewport, const Eigen::Vector2f& screen)
    {
        const float ndc_x = screen.x() / viewport.x() * 2.f - 1.f;
        const float ndc_y = screen.y() / viewport.y() * 2.f - 1.f;
        const Eigen::Vector4f o = cache.view_proj * Eigen::Vector4f(0.f, 0.f, 0.f, 1.f);
        const float ndc_z0 = o.z() / o.w();
        const Eigen::Vector4f w = cache.view_proj.inverse() * Eigen::Vector4f(ndc_x, ndc_y, ndc_z0, 1.f);
        return Eigen::Vector2f(w.x() / w.w(), w.y() / w.w());
    }

} // namespace lux::pack
