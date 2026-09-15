#pragma once
#include <lux/engine/editor/gui/scene/ComponentBinding.hpp>
#include <lux/engine/editor/gui/DocumentPane.hpp>
#include <lux/engine/editor/scene/SceneEditor.hpp>
#include <lux/engine/editor/gui/scene/InspectorInteraction.hpp>

namespace lux::editor::gui
{
    class InspectorPane final : public DocumentPane<InspectorPane, scene::SceneEditor>
    {
      public:
        InspectorPane(scene::SceneEditor &, std::string id, std::shared_ptr<const std::vector<ComponentBinding>>);
        void requestClose() noexcept override;

      private:
        void draw(lux::ui::Frame &, lux::ui::PaneDrawContext &) override;
        std::shared_ptr<const std::vector<ComponentBinding>> bindings_;
        InspectorInteraction interaction_;
        std::vector<scene::SceneComponentInfo> components_;
        scene::SelectionNotice selection_;
        bool directory_dirty_{true};
        object::ScopedConnection selection_connection_;
        object::ScopedConnection objects_connection_;
    };
} // namespace lux::editor::gui
