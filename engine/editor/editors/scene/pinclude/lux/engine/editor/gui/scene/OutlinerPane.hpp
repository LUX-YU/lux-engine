#pragma once
#include <lux/engine/editor/gui/DocumentPane.hpp>
#include <lux/engine/editor/scene/SceneEditor.hpp>
#include <unordered_set>

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
        void rebuildVisibleRows();
        [[nodiscard]] EditorResult<void> select(lux::world::WorldObjectId);
        void draw(lux::ui::Frame &, lux::ui::PaneDrawContext &) override;
        scene::SelectionNotice selection_;
        std::vector<Row> rows_;
        std::vector<std::size_t> visible_rows_;
        std::unordered_set<lux::world::WorldObjectId, lux::world::WorldObjectIdHash> collapsed_;
        bool rows_dirty_{true};
        bool visible_dirty_{true};
        std::uint32_t partition_{};
        std::string error_;
        object::ScopedConnection selection_connection_;
        object::ScopedConnection objects_connection_;
    };
} // namespace lux::editor::gui
