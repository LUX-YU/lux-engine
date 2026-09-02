#pragma once

#include <lux/engine/editor/inspector/InspectorContext.hpp>
#include <lux/engine/simulation/ecs/HierarchyIndex.hpp>
#include <lux/engine/simulation/ecs/Parent.hpp>

#include <entt/entity/entity.hpp>

#include <cstdint>

namespace lux::editor::inspector
{
    [[nodiscard]] inline bool applyParentRelation(
        simulation::ecs::Registry& registry,
        simulation::ecs::Entity child,
        const simulation::ecs::Entity& parent
    )
    {
        const auto result = parent == simulation::ecs::NullEntity ? simulation::ecs::detach(registry, child) :
                                                                   simulation::ecs::reparent(registry, child, parent);
        return result.has_value();
    }

    [[nodiscard]] inline bool editParentRelation(
        simulation::ecs::Registry& registry,
        simulation::ecs::Entity entity,
        InspectorContext& context,
        const GeneratedFieldSpec& spec
    ) noexcept
    {
        const auto* parent = registry.try_get<simulation::ecs::Parent>(entity);
        if (parent == nullptr)
            return false;
        const auto before = parent->entity;
        auto encoded = static_cast<std::uint32_t>(entt::to_integral(before));
        context.frame.propertyRow(spec.display_name.empty() ? "Parent" : spec.display_name);
        auto id = context.frame.id(ui::WidgetIdView{spec.name});
        auto edit = context.frame.editScalar("##value", encoded, ui::ScalarEditSpec<std::uint32_t>{});
        if (!spec.tooltip.empty())
            context.frame.tooltip(spec.tooltip);
        if (edit.began)
        {
            static_cast<void>(context.undo.begin<simulation::ecs::Parent, simulation::ecs::Entity>(
                context.target,
                spec.name,
                before,
                applyParentRelation
            ));
        }
        const bool is_detach = encoded ==
            static_cast<std::uint32_t>(entt::to_integral(simulation::ecs::NullEntity));
        const auto candidate = is_detach ?
            simulation::ecs::NullEntity : static_cast<simulation::ecs::Entity>(encoded);
        if (edit.changed)
        {
            if (!applyParentRelation(registry, entity, candidate))
            {
                static_cast<void>(context.undo.cancel(context.editor));
                return false;
            }
        }
        if (edit.cancelled)
            static_cast<void>(context.undo.cancel(context.editor));
        else if (edit.committed)
        {
            static_cast<void>(context.undo.commit<simulation::ecs::Parent, simulation::ecs::Entity>(
                context.target,
                spec.name,
                candidate
            ));
        }
        return edit.changed;
    }
} // namespace lux::editor::inspector
