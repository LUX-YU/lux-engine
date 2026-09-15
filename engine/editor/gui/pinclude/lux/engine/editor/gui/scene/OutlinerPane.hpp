#pragma once
#include <lux/engine/editor/gui/scene/DocumentPane.hpp>

namespace lux::editor::gui
{
    class OutlinerPane final : public DocumentPane<OutlinerPane>
    {
      public:
        OutlinerPane(scene::SceneEditor &, std::string id);

      private:
        void draw(lux::ui::Frame &, lux::ui::PaneDrawContext &) override;
        scene::SelectionNotice selection_;
        object::ScopedConnection selection_connection_;
    };
} // namespace lux::editor::gui
