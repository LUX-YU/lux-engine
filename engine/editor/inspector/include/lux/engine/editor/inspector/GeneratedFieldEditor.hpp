#pragma once

#include <lux/engine/editor/inspector/EditorValueBinding.hpp>
#include <lux/engine/editor/inspector/InspectorContext.hpp>

#include <type_traits>

namespace lux::editor::inspector
{
    template<class Component, auto Member>
    [[nodiscard]] bool applyPlainField(
        simulation::ecs::Registry& registry,
        simulation::ecs::Entity entity,
        const std::remove_cvref_t<decltype(std::declval<Component>().*Member)>& value
    )
    {
        auto* component = registry.template try_get<Component>(entity);
        if (component == nullptr)
            return false;
        component->*Member = value;
        registry.template patch<Component>(entity);
        return true;
    }

    template<class Component, auto Member>
    [[nodiscard]] bool editGeneratedField(
        simulation::ecs::Registry& registry,
        simulation::ecs::Entity entity,
        Component& component,
        InspectorContext& context,
        const GeneratedFieldSpec& spec
    ) noexcept
    {
        using Value = std::remove_cvref_t<decltype(component.*Member)>;
        const Value before = component.*Member;
        context.frame.propertyRow(spec.display_name.empty() ? spec.name : spec.display_name);
        auto id = context.frame.id(ui::WidgetIdView{spec.name});
        auto disabled = context.frame.disabled(spec.read_only || spec.widget == EGeneratedWidget::READ_ONLY);
        auto edit = EditorValueBinding<Value>::edit(context, "##value", component.*Member, spec);
        if (!spec.tooltip.empty())
            context.frame.tooltip(spec.tooltip);

        if (edit.began)
        {
            static_cast<void>(context.undo.begin<Component, Value>(
                context.target,
                spec.name,
                before,
                applyPlainField<Component, Member>
            ));
        }
        if (edit.changed)
            registry.template patch<Component>(entity);
        if (edit.cancelled)
            static_cast<void>(context.undo.cancel(context.editor));
        else if (edit.committed)
        {
            static_cast<void>(context.undo.commit<Component, Value>(
                context.target,
                spec.name,
                component.*Member
            ));
        }
        return edit.changed;
    }
} // namespace lux::editor::inspector
