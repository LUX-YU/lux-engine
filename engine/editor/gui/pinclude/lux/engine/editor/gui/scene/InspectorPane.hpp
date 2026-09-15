#pragma once
#include <lux/engine/editor/gui/scene/DocumentPane.hpp>
#include <lux/engine/editor/gui/scene/ComponentReadBinding.hpp>

namespace lux::editor::gui
{
    class InspectorPane final : public DocumentPane<InspectorPane>
    {
      public:
        InspectorPane(scene::SceneEditor &, std::string id);

      private:
        void draw(lux::ui::Frame &, lux::ui::PaneDrawContext &) override;
        std::vector<ComponentReadBinding> readers_;
        std::vector<scene::SceneComponentInfo> components_;
        scene::SelectionNotice selection_;
        bool directory_dirty_{true};
        object::ScopedConnection selection_connection_;
    };
} // namespace lux::editor::gui
