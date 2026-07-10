/**
 * @file Physics2DWorld.cpp
 * @brief Minimal static-collider + kinematic-controller physics (P2-01).
 *        See the header for scope and ordering contracts.
 */

#include <lux/pack/physics2d_demo/Physics2DWorld.hpp>
#include <lux/pack/d2/world/components/Transform2DComponent.hpp>
#include <lux/pack/d2/world/components/WorldTransform2DComponent.hpp>

#include <algorithm>
#include <cmath>
#include <lux/pack/physics2d_demo/Physics2DDemoPack.hpp>
#include <lux/pack/d2/D2ScenePlan.hpp>
#include <lux/pack/d2/CollisionProbe2D.hpp>
#include <lux/pack/d2/world/systems/Simulation2DSystem.hpp>
#include <cassert>
#include <memory>

namespace lux::pack
{
    namespace
    {
        constexpr float kOneWayEps = 1e-4f;

        Aabb2 boxAt(const Eigen::Vector2f& center, const Eigen::Vector2f& half) noexcept
        {
            return {center - half, center + half};
        }
    }

    Physics2DWorld::Physics2DWorld(const Physics2DConfig& cfg) noexcept : cfg_(cfg) {}

    void Physics2DWorld::addProbe(ICollision2DProbe* probe)
    {
        if (probe != nullptr) probes_.push_back(probe);
    }

    void Physics2DWorld::removeProbe(ICollision2DProbe* probe)
    {
        std::erase(probes_, probe);
    }

    bool Physics2DWorld::blocked(const Aabb2& box, bool moving_down, float prev_bottom) const
    {
        for (const StaticBox& s : statics_)
        {
            if (!box.overlaps(s.box)) continue;
            if (s.one_way)
            {
                // Solid only when LANDED ON from above: the mover must be going
                // down and must have STARTED at or above the platform top.
                if (!moving_down || prev_bottom < s.box.max.y() - kOneWayEps)
                    continue;
            }
            return true;
        }
        for (const ICollision2DProbe* p : probes_)
            if (p->regionSolid(box)) return true;
        return false;
    }

    void Physics2DWorld::step(lux::meta::EntityRegistry& registry, float fixed_dt)
    {
        // ── gather the static set (per substep; MVP counts are small).
        // Entities carrying a controller or a (future-dynamic) rigid body are
        // NOT obstacles. Sorted by entity id → sweep order is pool-layout-free.
        statics_.clear();
        registry.view<Collider2DComponent, WorldTransform2DComponent>().each(
            [&](lux::meta::entity_id e, const Collider2DComponent& c,
                const WorldTransform2DComponent& wt)
            {
                if (registry.any_of<CharacterController2DComponent, RigidBody2DComponent>(e))
                    return;
                const Eigen::Vector2f center{wt.world(0, 3) + c.offset.x(),
                                             wt.world(1, 3) + c.offset.y()};
                statics_.push_back({static_cast<std::uint32_t>(e),
                                    boxAt(center, c.halfSize()), c.one_way});
            });
        std::sort(statics_.begin(), statics_.end(),
                  [](const StaticBox& a, const StaticBox& b) { return a.order < b.order; });

        // ── sweep every controller (axis-separated: X, then Y).
        registry.view<CharacterController2DComponent, Collider2DComponent,
                      Transform2DComponent>().each(
            [&](CharacterController2DComponent& cc, const Collider2DComponent& col,
                Transform2DComponent& t)
            {
                cc.velocity.y() += cfg_.gravity_y * cc.gravity_scale * fixed_dt;
                cc.velocity.x() += cfg_.gravity_x * cc.gravity_scale * fixed_dt;

                const Eigen::Vector2f half = col.halfSize();
                Eigen::Vector2f center = t.position + col.offset;
                const float start_bottom = center.y() - half.y();

                // One axis: advance as far as free along @p delta; on contact
                // stop `skin` short (bisection — uniform over boxes AND probes,
                // deterministic) and zero the axis velocity.
                const auto moveAxis = [&](int axis, float delta) -> bool
                {
                    if (delta == 0.f) return false;
                    const bool moving_down = (axis == 1 && delta < 0.f);
                    Eigen::Vector2f target = center;
                    target[axis] += delta;
                    if (!blocked(boxAt(target, half), moving_down, start_bottom))
                    {
                        center = target;
                        return false;
                    }
                    // Bisect the free fraction, then back off by the skin.
                    float lo = 0.f, hi = 1.f;
                    for (int i = 0; i < 20; ++i)
                    {
                        const float mid = 0.5f * (lo + hi);
                        Eigen::Vector2f probe = center;
                        probe[axis] += delta * mid;
                        if (blocked(boxAt(probe, half), moving_down, start_bottom))
                            hi = mid;
                        else
                            lo = mid;
                    }
                    float advance = delta * lo;
                    advance -= (advance > 0.f ? cc.skin : -cc.skin);
                    if ((delta > 0.f && advance > 0.f) || (delta < 0.f && advance < 0.f))
                        center[axis] += advance;
                    cc.velocity[axis] = 0.f;   // slide: the other axis keeps moving
                    return true;
                };

                moveAxis(0, cc.velocity.x() * fixed_dt);
                const bool hit_y = moveAxis(1, cc.velocity.y() * fixed_dt);
                (void)hit_y;

                // Standing check: solid within 2×skin below the feet.
                Aabb2 feet = boxAt(center, half);
                feet.min.y() -= 2.f * cc.skin;
                cc.grounded = blocked(feet, /*moving_down=*/true, center.y() - half.y());

                t.position = center - col.offset;
            });
    }

    // ── the registry entry (Physics2DDemoPack.hpp) ──────────────────────────
    void addPhysics2DDemoPack(lux::render_bridge::ScenePackRegistry& reg)
    {
        using lux::render_bridge::ScenePackContext;
        using lux::render_bridge::ScenePackEntry;

        // Order 70: after the d2 entries — the fixed-step coordinator (20)
        // exists and the domain side has published its probes (60).
        reg.add(ScenePackEntry{
            .backs        = static_cast<std::uint32_t>(D2Capability::Physics) |
                            static_cast<std::uint32_t>(D2Capability::CharacterController),
            .order        = 70,
            .install      = [](const ScenePackContext& ctx)
            {
                const auto& plan = *static_cast<const D2ScenePlan*>(ctx.plan);
                auto* sim = ctx.services.get<Simulation2DSystem>();
                assert(sim && "registry order contract broken: the fixed-step "
                              "coordinator (d2 entry, order 20) must precede physics");

                auto& physics = ctx.services.emplace(
                    std::make_unique<Physics2DWorld>(plan.physicsConfig()));

                // Drain the domain-published probe seam (pixel terrain, …).
                if (const auto* probes = ctx.services.get<CollisionProbes2D>())
                    for (ICollision2DProbe* p : probes->probes)
                        physics.addProbe(p);

                Physics2DWorld* raw = &physics;   // services outlive the World
                sim->setPhase(Simulation2DSystem::Phase::SimulatePhysics,
                    [raw](lux::meta::EntityRegistry& r, float dt) { raw->step(r, dt); });
            }});
    }

} // namespace lux::pack
