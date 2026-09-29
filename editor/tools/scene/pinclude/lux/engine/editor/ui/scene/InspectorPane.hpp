#pragma once

#include <lux/engine/editor/ui/scene/InspectorElement.hpp>
#include <lux/engine/ui/Pane.hpp>

namespace lux::editor::ui
{
    class InspectorPane final : public lux::ui::Pane
    {
    public:
        InspectorPane(
            lux::ui::Pane& parent,
            lux::ui::PaneId id,
            const lux::simulation::ecs::ComponentSchemaSet& schemas,
            const ComponentEditorRegistry& editors,
            EditorResult<void>& status,
            const ProjectStorage* catalog
        );
        [[nodiscard]] InspectorElement& content() noexcept
        {
            return content_;
        }
        [[nodiscard]] EditorResult<void> finishEditing()
        {
            return content_.finishEditing();
        }

    private:
        void update() noexcept override;
        InspectorElement content_;
        object::Connection close_connection_;
        bool hide_requested_{};
    };
}
