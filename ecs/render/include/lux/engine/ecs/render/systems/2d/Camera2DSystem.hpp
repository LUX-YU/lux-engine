#pragma once

#include <lux/engine/math/Position.hpp>
// ============================================================================
//  Camera2DSystem.hpp — derive 2D orthographic view/proj (lux::ecs).
//
//  Mirrors d3::CameraSystem: pose comes from ResolvedTransform2D (NOT the camera
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
#include <lux/engine/ecs/render/components/2d/Camera2DComponent.hpp>
#include <lux/engine/ecs/render/components/PrimaryCameraTag.hpp>
#include <lux/engine/ecs/render/components/2d/Camera2DCacheComponent.hpp>
#include <lux/engine/ecs/render/components/ViewPresentComponent.hpp>
#include <lux/engine/ecs/render/systems/RenderSystem.hpp>
#include <lux/engine/ecs/components/ResolvedTransform2DComponent.hpp>
#include <lux/engine/ecs/SpatialTransformMath.hpp>

#include <lux/engine/math/eigen_extend.hpp>
#include <Eigen/Core>
#include <cassert>
#include <cmath>
#include <vector>

namespace lux::ecs
{

    class Camera2DSystem final : public lux::ecs::ISystem
    {
    public:
        [[nodiscard]] std::span<const Type>
        runsBefore() const noexcept override
        {
            static constexpr Type dependencies[]{systemType<RenderSystem>()};
            return dependencies;
        }

        void update(const lux::ecs::SystemUpdateContext& ctx) override
        {
            auto& registry = ctx.registry();
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

            for (const auto entity : registry.view<
                     ResolvedTransform2DComponent,
                     Camera2DComponent,
                     Camera2DCacheComponent>())
            {
                const auto& wc =
                    registry.get<ResolvedTransform2DComponent>(entity);
                const auto& cc = registry.get<Camera2DComponent>(entity);
                auto& cache = registry.get<Camera2DCacheComponent>(entity);
                cache.effective_aspect = cc.aspect;
                if (const auto* present =
                        registry.try_get<ViewPresentComponent>(entity);
                    present && present->extent.height != 0u)
                {
                    cache.effective_aspect =
                        static_cast<float>(present->extent.width) /
                        static_cast<float>(present->extent.height);
                }

                const Eigen::Vector3f position = Eigen::Vector3f::Zero();

                // Up = the camera's rotated +Y, de-scaled (normalized) so a scaled
                // transform doesn't skew the view. Forward is fixed to -Z (the 2D plane).
                Eigen::Vector3f up{
                    wc.linear(0, 1),
                    wc.linear(1, 1),
                    0.0f};
                const float n = up.norm();
                if (n > 1e-6f) up /= n;
                const Eigen::Vector3f forward(0.f, 0.f, -1.f);
                const Eigen::Vector3f target = forward;

                LuxEigenExt::Affine3<float> view_affine =
                    LuxEigenExt::TLookAt<Eigen::Vector3f, Eigen::Vector3f,
                                         Eigen::Vector3f, true>(position, target, up);

                // Orthographic extents from the single base scale + aspect.
                float half_h = cc.units_per_view_height * 0.5f;
                float half_w = half_h * cache.effective_aspect;
                switch (cc.scale_mode)
                {
                case ECamera2DScaleMode::FIT_WIDTH:
                    half_w = cc.units_per_view_height * 0.5f;
                    half_h = cache.effective_aspect > 1e-6f
                        ? half_w / cache.effective_aspect
                        : half_w;
                    break;
                case ECamera2DScaleMode::STRETCH:
                    half_w = half_h = cc.units_per_view_height * 0.5f;
                    break;
                case ECamera2DScaleMode::FIT_HEIGHT:
                default:
                    break;   // half_h/half_w already set
                }

                cache.prev_view_proj = cache.view_proj;
                cache.view = view_affine.matrix();
                cache.render_origin = wc.position;
                cache.proj = LuxEigenExt::TOrthographicProjection<float>(
                    -half_w, half_w, -half_h, half_h, kNearZ, kFarZ);
                // Render-client canonical clip space is ZO with Y down.
                cache.proj(1, 1) = -cache.proj(1, 1);
                cache.view_proj = cache.proj * cache.view;
            }
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

    /// The scene's active camera: the SINGLE entity carrying PrimaryCameraTag (+ a
    /// Camera2DComponent). Returns null_entity when there is zero (a Canvas can then
    /// skip rendering rather than read garbage) OR more than one (ambiguous). NEVER the
    /// implicit "first camera" — selection is explicit (design T2-03).
    [[nodiscard]] inline lux::meta::entity_id activeCamera(lux::meta::EntityRegistry& reg)
    {
        lux::meta::entity_id found = lux::meta::null_entity;
        int count = 0;
        for (auto e : reg.view<PrimaryCameraTag, Camera2DComponent>()) { found = e; ++count; }
        return count == 1 ? found : lux::meta::null_entity;
    }

    /// Screen pixel → view-relative point on the z=0 plane via a camera's
    /// rotation-only view_proj. @p viewport is (width, height) in pixels; the
    /// mapping honours the render-client Y-down clip convention. The returned float is deliberately
    /// relative to `cache.render_origin`, never a narrowed absolute coordinate.
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

    /// Double-precision absolute-position counterpart used by authoring commands.
    [[nodiscard]] inline std::optional<lux::math::Position2d>
    screenToWorldPosition(
        const Camera2DCacheComponent& cache,
        const Eigen::Vector2f& viewport,
        const Eigen::Vector2f& screen) noexcept
    {
        if (!viewport.allFinite() || !screen.allFinite() ||
            viewport.x() <= 0.0f || viewport.y() <= 0.0f)
        {
            return std::nullopt;
        }
        const auto relative = screenToWorld(cache, viewport, screen);
        if (!relative.allFinite())
            return std::nullopt;
        const lux::math::Position2d result{
            cache.render_origin.x + static_cast<double>(relative.x()),
            cache.render_origin.y + static_cast<double>(relative.y())};
        if (!lux::math::isFinite(result))
            return std::nullopt;
        return result;
    }

} // namespace lux::ecs
