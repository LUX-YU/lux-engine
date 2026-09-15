#pragma once
#include <lux/engine/editor/gui/DocumentPane.hpp>
#include <lux/engine/editor/scene/SceneEditor.hpp>

namespace lux::editor::gui
{
class OutlinerPane final : public DocumentPane<OutlinerPane, scene::SceneEditor>
{
  public:
    OutlinerPane(scene::SceneEditor &, std::string id);

  private:
    struct Row final
    {
        std::size_t source, depth, end;
    };
    void rebuildRows();
    void draw(lux::ui::Frame &, lux::ui::PaneDrawContext &) override;
    scene::SelectionNotice selection_;
    object::ScopedConnection selection_connection_;
    object::ScopedConnection objects_connection_;
    std::vector<Row> rows_;
    bool rows_dirty_{true};
    std::uint32_t partition_{};
    std::string error_;
};
} // namespace lux::editor::gui
