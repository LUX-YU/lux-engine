#include <lux/engine/editor/ui/scene/InspectorPane.hpp>
#include <lux/engine/editor/detail/SignalDelivery.hpp>

namespace lux::editor::ui
{
    InspectorPane::InspectorPane(
        lux::ui::Pane& parent,
        lux::ui::PaneId id,
        const lux::simulation::ecs::ComponentSchemaSet& schemas,
        const ComponentEditorRegistry& editors,
        EditorResult<void>& status
    )
        : lux::ui::Pane(parent, std::move(id), lux::ui::PaneTypeId{"lux.editor.inspector"}, "Inspector"),
          content_(*this, lux::ui::ElementId{"inspector"}, schemas, editors, status),
          close_connection_(lux::editor::detail::takeConnection(
              lux::object::LuxObject::connect(
                  this,
                  &lux::ui::Pane::closeRequested,
                  [this]() noexcept { hide_requested_ = true; }
              ),
              status
          ))
    {
        setContent(content_);
    }

    void InspectorPane::update() noexcept
    {
        if (hide_requested_ && finishEditing())
        {
            setVisible(false);
            hide_requested_ = false;
        }
    }
}
