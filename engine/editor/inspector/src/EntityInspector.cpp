#include <lux/engine/editor/inspector/EntityInspector.hpp>

#include <lux/engine/editor/inspector/InspectorContext.hpp>
#include <lux/engine/scene/Scene.hpp>
#include <lux/engine/ui/Frame.hpp>

#include <utility>

namespace lux::editor::inspector
{
    EntityInspector::EntityInspector(
        object::ObjectDispatcherRef dispatcher,
        ui::PaneId id,
        EditorContext& context,
        ComponentEditorBindingTable bindings
    )
        : Object(
              std::move(dispatcher),
              std::move(id),
              ui::PaneTypeId{kEntityInspectorPaneType.name()},
              "Inspector"
          ),
          context_(&context),
          bindings_(std::move(bindings))
    {
    }

    InspectorUndoJournal& EntityInspector::undoJournal() noexcept { return undo_; }
    const InspectorUndoJournal& EntityInspector::undoJournal() const noexcept { return undo_; }
    const InspectorDrawStats& EntityInspector::lastDrawStats() const noexcept { return last_draw_; }

    void EntityInspector::draw(ui::Frame& frame, ui::PaneDrawContext&)
    {
        last_draw_ = {};
        const auto before_validation = context_->selection().current();
        if (!context_->selection().validate())
            last_draw_.stale_selection = before_validation.entity != simulation::ecs::NullEntity;
        const auto selection = context_->selection().current();
        if (!selection.scene.valid() || selection.entity == simulation::ecs::NullEntity)
        {
            frame.textMuted("No entity selected");
            return;
        }
        auto* scene = context_->selection().resolve(selection.scene);
        if (scene == nullptr || !scene->registry().valid(selection.entity))
        {
            last_draw_.stale_selection = true;
            frame.textMuted("Selection is no longer available");
            return;
        }

        InspectorContext inspector_context{*context_, frame, undo_, selection};
        auto& registry = scene->registry();
        for (const auto& schema : context_->sceneMeta().components().all())
        {
            if (!schema.editor_visible || !schema.operations.has(registry, selection.entity))
                continue;
            ++last_draw_.visible_components;
            const auto* binding = bindings_.find(schema.cpp_type);
            if (binding == nullptr || binding->schema != schema.id)
            {
                ++last_draw_.missing_bindings;
                frame.text(schema.id.name);
                frame.textMuted("<Editor binding unavailable>");
                continue;
            }
            frame.text(binding->display_name);
            auto table = frame.table(ui::TableSpec{
                ui::WidgetIdView{schema.id.name},
                2U,
                false,
                false,
                true
            });
            if (table.visible())
                static_cast<void>(binding->draw(registry, selection.entity, inspector_context));
        }
    }
} // namespace lux::editor::inspector
