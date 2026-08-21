#pragma once
/**
 * @file RegistrySnapshot.hpp
 * @brief In-memory, same-id registry content snapshot.
 *
 * Captures a filtered set of entities + their reflected components into a
 * private shadow registry; restore() destroys the live content set and
 * recreates every captured entity under its ORIGINAL identifier
 * (entt `create(hint)` forces index AND version onto a free slot), so
 * entity references inside components — and outside observers like the
 * editor Selection — survive a capture/restore round trip with NO remap.
 *
 * Component coverage = the injected ComponentTypeCatalog's clone shims
 * (copy-constructible reflected components; the same authored set the scene
 * serializer persists). The one structural exception, ParentComponent,
 * is deliberately unreflected — parent links are captured as (child, parent)
 * pairs and reapplied through setParent(), the sole legal write path, so
 * entt signals fire for event-driven consumers (HierarchyIndex).
 *
 * The predicate passed to capture()/restore() decides what is CONTENT
 * (snapshotted + destroyed-and-restored) versus scaffolding (untouched);
 * pass the same predicate to both. Restore is memory-only and cannot fail
 * (fallible logic has no place in a destructor — and there is nothing here
 * that could fail to hide in the first place).
 */

#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/ecs/components/ParentComponent.hpp>
#include <lux/engine/ecs/systems/HierarchicalTransformSystem.hpp>   // setParent

#include <entt/entt.hpp>

#include <cstdio>
#include <utility>
#include <vector>

namespace lux::ecs
{
    class RegistrySnapshot
    {
    public:
        explicit RegistrySnapshot(
            const ComponentTypeCatalog& components) noexcept
            : components_(components)
        {}

        /// Snapshot every entity for which @p is_content returns true.
        /// Read-only on @p live. Replaces any previous capture.
        template<typename ContentPred>
        void capture(
            lux::ecs::RegistryBase& live,
            ContentPred&& is_content)
        {
            shadow_.clear();
            map_.clear();
            parents_.clear();

            const auto types = components_.all();
            for (auto e : live.view<entt::entity>())
            {
                if (!is_content(e))
                    continue;
                const auto s = shadow_.create();
                map_.emplace_back(e, s);
                for (const auto& t : types)
                    if (t.operations.clone)
                        t.operations.clone(live, e, shadow_, s);
                if (const auto* h = live.try_get<ParentComponent>(e);
                    h && h->parent() != lux::ecs::kNullEntity)
                    parents_.emplace_back(e, h->parent());
            }
        }

        /// Destroy the live content set (@p is_content — same predicate as
        /// capture), then rebuild it from the shadow: original ids first (all
        /// targets exist before any component lands, so entity references are
        /// valid on arrival), components second, parent links last.
        template<typename ContentPred>
        void restore(
            lux::ecs::RegistryBase& live,
            ContentPred&& is_content)
        {
            std::vector<entt::entity> doomed;
            for (auto e : live.view<entt::entity>())
                if (is_content(e))
                    doomed.push_back(e);
            for (auto e : doomed)
                live.destroy(e);

            for (const auto& [orig, s] : map_)
            {
                if (const auto got = live.create(orig); got != orig)
                {
                    // Only reachable if a non-content entity occupied a
                    // captured slot mid-session — refs into this entity are
                    // stale. Loud, not silent; the entity itself is restored.
                    std::fprintf(stderr,
                        "[RegistrySnapshot] restore: id %u not recoverable "
                        "(got %u) — stale references possible\n",
                        static_cast<unsigned>(entt::to_integral(orig)),
                        static_cast<unsigned>(entt::to_integral(got)));
                }
            }

            const auto types = components_.all();
            for (const auto& [orig, s] : map_)
                for (const auto& t : types)
                    if (t.operations.clone)
                        t.operations.clone(shadow_, s, live, orig);

            for (const auto& [child, parent] : parents_)
                setParent(live, child, parent);
        }

        [[nodiscard]] bool empty() const noexcept { return map_.empty(); }

        /// Drop the captured state (shadow registry included).
        void clear()
        {
            shadow_.clear();
            map_.clear();
            parents_.clear();
        }

    private:
        const ComponentTypeCatalog&                         components_;
        lux::ecs::Registry                          shadow_;
        std::vector<std::pair<entt::entity, entt::entity>> map_;      // live id → shadow id
        std::vector<std::pair<entt::entity, entt::entity>> parents_;  // child → parent (live ids)
    };

} // namespace lux::ecs
