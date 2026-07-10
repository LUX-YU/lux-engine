#pragma once
#include "systems/ISystem.hpp"
#include <lux/engine/meta/LuxObject.hpp>
#include <memory>
#include <vector>

namespace lux::ecs
{
    /// A domain-neutral ECS world: owns the registry + an ordered list of systems.
    ///
    /// World hardcodes NO systems — it knows nothing about 3D transforms, cameras,
    /// or animation. A dimension kit installs its systems explicitly: a 3D scene
    /// calls `d3::installSystems(world)` (Transform → Camera → Animation, in
    /// that order); a 2D / card / robotics kit installs its own. This is what lets
    /// the same World back any domain (the neutral-core split — see gameplay/3d, namespace d3).
    ///
    /// Systems run in registration order each `tick()`. Append with `addSystem`.
    class World
    {
    public:
        World() = default;

        // ------------------------------------------------------------------ //
        //  Entity management                                                  //
        // ------------------------------------------------------------------ //

        [[nodiscard]] lux::meta::entity_id createEntity()
        { return registry_.create(); }

        void destroyEntity(lux::meta::entity_id id)
        { if (registry_.valid(id)) registry_.destroy(id); }

        [[nodiscard]] bool valid(lux::meta::entity_id id) const noexcept
        { return registry_.valid(id); }

        // ------------------------------------------------------------------ //
        //  Component access                                                   //
        // ------------------------------------------------------------------ //

        // decltype(auto), not `C&`: entt's emplace<EmptyTag> returns void (empty-type
        // optimization), which `C&` can't forward — so `world.emplace<SomeTag>(e)` failed
        // to compile for empty tag components. 2D will lean on tags (dormant / disabled /
        // dirty / trigger), so support them here. Non-empty components still yield `C&`.
        template<typename C, typename... Args>
        decltype(auto) emplace(lux::meta::entity_id id, Args&&... args)
        { return registry_.emplace<C>(id, std::forward<Args>(args)...); }

        template<typename C>
        [[nodiscard]] C& get(lux::meta::entity_id id)
        { return registry_.get<C>(id); }

        template<typename C>
        [[nodiscard]] const C& get(lux::meta::entity_id id) const
        { return registry_.get<C>(id); }

        template<typename C>
        [[nodiscard]] C* tryGet(lux::meta::entity_id id)
        { return registry_.try_get<C>(id); }

        template<typename C>
        [[nodiscard]] const C* tryGet(lux::meta::entity_id id) const
        { return registry_.try_get<C>(id); }

        template<typename C>
        [[nodiscard]] bool has(lux::meta::entity_id id) const
        { return registry_.all_of<C>(id); }

        template<typename C>
        void remove(lux::meta::entity_id id)
        { registry_.remove<C>(id); }

        // ------------------------------------------------------------------ //
        //  Systems                                                            //
        // ------------------------------------------------------------------ //

        /// Append a user-defined system; runs after built-in systems each tick.
        void addSystem(std::unique_ptr<ISystem> system)
        { systems_.push_back(std::move(system)); }

        /// Number of installed systems (for diagnostics + install-contract tests, e.g.
        /// verifying an empty D2ScenePlan installs none).
        [[nodiscard]] std::size_t systemCount() const noexcept { return systems_.size(); }

        // ------------------------------------------------------------------ //
        //  Per-frame update                                                   //
        // ------------------------------------------------------------------ //

        void tick(float dt)
        {
            for (auto& sys : systems_)
                sys->update(registry_, dt);
        }

        // ------------------------------------------------------------------ //
        //  Registry access (for custom entt views/queries)                   //
        // ------------------------------------------------------------------ //

        [[nodiscard]] lux::meta::EntityRegistry&       registry()       noexcept { return registry_; }
        [[nodiscard]] const lux::meta::EntityRegistry& registry() const noexcept { return registry_; }

    private:
        lux::meta::EntityRegistry        registry_;
        std::vector<std::unique_ptr<ISystem>> systems_;
    };

} // namespace lux::ecs
