#pragma once
// ============================================================================
//  Transform2DSystem.hpp — local 2D TRS → world, hierarchy-aware (lux::gameplay::d2).
//
//  A faithful mirror of d3::TransformSystem (same two-pass memoized DFS, the same
//  G-07 derived-component maintenance, the same G-08 cycle detection) — only the
//  per-node math differs: computeTRS2D embeds a 2D pose (x,y translation, +Z
//  rotation, x/y scale) into a 4x4 so the world matrix shares the 3D zero-copy path.
//  It reuses the NEUTRAL HierarchyComponent (2D defines no Parent2D). Dirty is
//  value-based (recompose vs previous frame), so any mutation source is picked up.
//
//  (Duplicated rather than shared with d3 by design — d2/d3 are independent peers.
//  If the resolution core is ever factored, keep both kits' behaviour identical.)
// ============================================================================

#include <lux/engine/gameplay/world/systems/ISystem.hpp>
#include "../components/Transform2DComponent.hpp"
#include "../components/WorldTransform2DComponent.hpp"
#include <lux/engine/gameplay/world/components/HierarchyComponent.hpp>

#include <Eigen/Geometry>
#include <cstddef>
#include <cstdio>
#include <unordered_set>
#include <vector>

namespace lux::gameplay::d2
{
    using lux::gameplay::HierarchyComponent;   // neutral parent link (shared with d3)

    class Transform2DSystem final : public lux::gameplay::ISystem
    {
    public:
        void update(lux::meta::EntityRegistry& registry, float /*dt*/) override
        {
            // G-07: the system owns the derived WorldTransform2D — no back-fill by callers.
            maintainDerived(registry);

            // Pass 1 — roots (no parent link): world = local 2D TRS.
            registry.view<Transform2DComponent, WorldTransform2DComponent>(
                entt::exclude<HierarchyComponent>)
                .each([](Transform2DComponent& tc, WorldTransform2DComponent& wc)
            {
                const Eigen::Matrix4f new_world = computeTRS2D(tc);
                wc.prev_world = wc.world;
                wc.dirty      = tc.dirty || wc.world != new_world;   // value-based
                wc.world      = new_world;
                tc.dirty      = false;
            });

            // Pass 2 — hierarchical, resolved parent-first (memoized DFS, any depth).
            resolved_.clear();
            visiting_.clear();
            cycles_last_update_ = 0;
            auto child_view = registry.view<Transform2DComponent, WorldTransform2DComponent,
                                            HierarchyComponent>();
            for (auto e : child_view)
                resolve(registry, e);

#ifndef NDEBUG
            if (cycles_last_update_ > 0)
                std::fprintf(stderr,
                    "[Transform2DSystem] hierarchy cycle detected: %zu back-edge(s) broken "
                    "this frame; cycle nodes fall back to their local TRS.\n",
                    cycles_last_update_);
#endif
        }

    public:
        /// Hierarchy back-edges (cycles) broken in the last update (0 = well-formed). G-08.
        [[nodiscard]] std::size_t cyclesLastUpdate() const noexcept { return cycles_last_update_; }

    private:
        void maintainDerived(lux::meta::EntityRegistry& registry)
        {
            scratch_.clear();
            for (auto e : registry.view<Transform2DComponent>(entt::exclude<WorldTransform2DComponent>))
                scratch_.push_back(e);
            for (auto e : scratch_)
                registry.emplace<WorldTransform2DComponent>(e);

            scratch_.clear();
            for (auto e : registry.view<WorldTransform2DComponent>(entt::exclude<Transform2DComponent>))
                scratch_.push_back(e);
            for (auto e : scratch_)
                registry.remove<WorldTransform2DComponent>(e);
        }

        void resolve(lux::meta::EntityRegistry& registry, lux::meta::entity_id e)
        {
            if (resolved_.count(e)) return;
            if (!visiting_.insert(e).second)          // back-edge on the current DFS stack → cycle
            {
                ++cycles_last_update_;                // G-08: break it (don't recurse)
                return;
            }

            auto* tc = registry.try_get<Transform2DComponent>(e);
            auto* wc = registry.try_get<WorldTransform2DComponent>(e);
            if (!tc || !wc) 
            { 
                visiting_.erase(e); 
                return; 
            }
            const auto* hc = registry.try_get<HierarchyComponent>(e);

            Eigen::Matrix4f parent_world = Eigen::Matrix4f::Identity();
            bool            parent_dirty = false;
            if (hc && registry.valid(hc->parent))
            {
                if (registry.all_of<HierarchyComponent>(hc->parent))
                    resolve(registry, hc->parent);
                // P1-1: skip a cycle-closing parent (still on the stack) so cycle nodes
                // fall back to their local TRS — a stable fixed point, no cross-frame drift.
                if (!visiting_.count(hc->parent))
                    if (const auto* pwc = registry.try_get<WorldTransform2DComponent>(hc->parent))
                    {
                        parent_world = pwc->world;
                        parent_dirty = pwc->dirty;
                    }
            }

            const Eigen::Matrix4f new_world = parent_world * computeTRS2D(*tc);
            wc->prev_world = wc->world;
            wc->dirty      = tc->dirty || parent_dirty || wc->world != new_world;
            wc->world      = new_world;
            tc->dirty      = false;

            visiting_.erase(e);
            resolved_.insert(e);
        }

        /// Embed the 2D pose in a 4x4: T(x,y,0) * Rz(rotation) * S(sx,sy,1).
        static Eigen::Matrix4f computeTRS2D(const Transform2DComponent& tc)
        {
            Eigen::Affine3f t = Eigen::Affine3f::Identity();
            t.translate(Eigen::Vector3f(tc.position.x(), tc.position.y(), 0.0f));
            t.rotate(Eigen::AngleAxisf(tc.rotation, Eigen::Vector3f::UnitZ()));
            t.scale(Eigen::Vector3f(tc.scale.x(), tc.scale.y(), 1.0f));
            return t.matrix();
        }

        std::unordered_set<lux::meta::entity_id> resolved_;
        std::unordered_set<lux::meta::entity_id> visiting_;
        std::size_t                              cycles_last_update_{0};
        std::vector<lux::meta::entity_id>        scratch_;
    };

} // namespace lux::gameplay::d2
