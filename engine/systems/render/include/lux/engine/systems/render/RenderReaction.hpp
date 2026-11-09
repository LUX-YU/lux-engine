#pragma once

#include <exec/task.hpp>
#include <lux/engine/ecs/Registry.hpp>
#include <entt/container/dense_set.hpp>
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/systems/render/visibility.h>
#include <optional>
#include <unordered_set>
#include <vector>

namespace lux::engine::render
{
    class RenderSystem;

    // Latest-state render coordination: one root per Entity; the dirty set retains changes during that root.
    // SceneSession owns task lifetime. Concrete reactions own only their actual resource candidates/results.
    class LUX_ENGINE_SYSTEM_RENDER_PUBLIC RenderReaction : public object::LuxObject
    {
    public:
        using Result = cxx::expected<void, error::Error>;
        ~RenderReaction() override = default;
        [[nodiscard]] lux::render::RenderResult<void> process() noexcept;

    protected:
        explicit RenderReaction(RenderSystem& system) noexcept;

        [[nodiscard]] RenderSystem& system() noexcept
        {
            return system_;
        }

        [[nodiscard]] ecs::Registry& registry() noexcept;
        template <class... Components> void watchComponents() noexcept
        {
            (event_connections_.emplace_back(registry().on_construct<Components>()
                .template connect<&RenderReaction::inputChanged>(*this)), ...);
            (event_connections_.emplace_back(registry().on_update<Components>()
                .template connect<&RenderReaction::inputChanged>(*this)), ...);
            (event_connections_.emplace_back(registry().on_destroy<Components>()
                .template connect<&RenderReaction::inputChanged>(*this)), ...);
            ([&]() noexcept { for (auto entity : registry().view<Components>()) notify(entity); }(), ...);
        }

        template <class Component, auto Member, class Derived> void connectConstruct(Derived& object) noexcept
        {
            event_connections_.emplace_back(registry().on_construct<Component>().template connect<Member>(object));
            for (const auto entity : registry().view<Component>())
            {
                (object.*Member)(registry(), entity);
            }
        }

        template <class Component, auto Member, class Derived> void connectRemoval(Derived& object) noexcept
        {
            event_connections_.emplace_back(registry().on_destroy<Component>().template connect<Member>(object));
        }

        void notify(ecs::Entity entity) noexcept;
        [[nodiscard]] bool canAdopt(ecs::Entity entity) const noexcept;
        // Enter and finish at Engine's Main safe point. CPU/IO stages return here before ECS adoption.
        // Resource replies must be adopted or released even when the entity/input changed while suspended.
        [[nodiscard]] virtual exec::task<Result> reconcile(ecs::Entity entity) noexcept = 0;
        virtual void reportFailure(ecs::Entity entity, error::Error error) noexcept;
        [[nodiscard]] virtual exec::task<Result> releaseResources() noexcept;

    private:
        friend class RenderSystem;
        RenderSystem& system_;
        void inputChanged(ecs::Registry&, ecs::Entity entity) noexcept;
        // Full Entity identities preserve both generations when a slot is reused before process().
        entt::dense_set<ecs::Entity> pending_;
        entt::dense_set<ecs::Entity> processing_;
        std::unordered_set<ecs::Entity> in_flight_entities_;
        std::optional<error::Error> failure_;
        std::vector<entt::scoped_connection> event_connections_;
    };
} // namespace lux::engine::render
