#pragma once
/**
 * @file Selection.hpp
 * @brief Editor shared-state type: the current ECS world + selected entity.
 *
 * One of the state types kept in the editor's StateRegistry. Panels that care
 * about selection `ensure<Selection>()` it at init and read it each frame
 * (immediate-mode, via registry()/entity()); every mutation goes through the
 * single writer select() (hierarchy click / viewport pick / scene swap), which
 * emits `changed` ONLY on a real transition.
 *
 * This is the ECS-coupled piece (`entt`), which is exactly why it lives in the
 * editor layer and NOT in the generic `ui` framework. registry() must be
 * non-null while a panel reads it; treat entity() as advisory and guard with
 * `registry()->valid(entity())` (a scene swap can leave a stale id for one
 * frame).
 *
 * The "current value" lives here (immediate-mode panels pull it every frame);
 * `changed` is the EDGE — connect to it only for the side-effects a transition
 * triggers (e.g. closing a stale picker popup), not for data already pulled.
 * Always held by `shared_ptr` (never copied/moved), so the embedded non-copyable
 * Signal is safe.
 */

#include <entt/entt.hpp>

#include <lux/cxx/event/Signal.hpp>

namespace lux::editor
{
    /// Payload of Selection::changed — the world + entity AFTER the transition.
    struct SelectionChanged
    {
        entt::registry* registry;
        entt::entity    entity;
    };

    class Selection
    {
    public:
        [[nodiscard]] entt::registry* registry() const noexcept { return registry_; }
        [[nodiscard]] entt::entity    entity()   const noexcept { return entity_; }

        /// Single mutation entry point: atomically set world + entity. Emits
        /// `changed` only when the value actually transitions (self-dedupes, so
        /// every-frame callers and repeated writes never spam subscribers).
        void select(entt::registry* reg, entt::entity e)
        {
            if (reg == registry_ && e == entity_)
                return;
            registry_ = reg;
            entity_   = e;
            changed.emit({reg, e});
        }

        /// Keep the current world, change the selected entity (hierarchy click,
        /// viewport pick, spawn). entt::null clears the selection.
        void selectEntity(entt::entity e) { select(registry_, e); }

        /// Clear the entity, keep the current world.
        void clear() { select(registry_, entt::null); }

        /// Emitted on every real selection transition. See file header: connect
        /// only for edge-triggered side-effects; pull the value via entity().
        lux::cxx::event::Signal<SelectionChanged> changed;

    private:
        entt::registry* registry_{nullptr};
        entt::entity    entity_{entt::null};
    };

} // namespace lux::editor
