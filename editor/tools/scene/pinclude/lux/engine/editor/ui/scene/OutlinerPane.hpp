#pragma once
#include <lux/engine/editor/detail/SignalDelivery.hpp>
#include <lux/engine/ui/Element.hpp>
#include <lux/engine/editor/scene/detail/SceneEditorImpl.hpp>
#include <unordered_set>

namespace lux::editor::ui
{
    class OutlinerElement final : public lux::ui::Element
    {
    public:
        OutlinerElement(lux::ui::Pane&, scene::SceneEditor::Impl&, lux::scene::SceneRuntime&, EditorResult<void>&);

    private:
        scene::SceneEditor::Impl& editor_;
        lux::scene::SceneRuntime& runtime_;
        struct ObjectRow final
        {
            lux::simulation::ecs::Entity object{lux::simulation::ecs::NullEntity}, parent{lux::simulation::ecs::NullEntity};
            std::string label;
        };
        std::vector<ObjectRow> objects_;
        struct Row final
        {
            std::size_t source, depth, end;
        };
        void rebuildRows();
        void rebuildVisibleRows();
        [[nodiscard]] EditorResult<void> select(lux::simulation::ecs::Entity);
        void draw() noexcept override;
        void drawObjects() noexcept;
        void update() noexcept override;
        std::optional<lux::simulation::ecs::Entity> selection_request_;
        lux::scene::SceneInstanceId observed_instance_;
        editing::Revision observed_revision_;
        scene::SelectionNotice selection_;
        std::vector<Row> rows_;
        std::vector<std::size_t> visible_rows_;
        std::unordered_set<lux::simulation::ecs::Entity> collapsed_;
        bool rows_dirty_{true};
        bool visible_dirty_{true};
        std::uint32_t partition_{};
        std::string error_;
        object::Connection selection_connection_;
        object::Connection objects_connection_;
    };
    class OutlinerPane final : public lux::ui::Pane
    {
    public:
        OutlinerPane(
            scene::SceneEditor::Impl& editor,
            lux::scene::SceneRuntime& runtime,
            lux::ui::PaneId id,
            EditorResult<void>& status
        )
            : lux::ui::Pane(*editor.editor, std::move(id), lux::ui::PaneTypeId{"lux.editor.outliner"}, "Outliner"),
              content_(*this, editor, runtime, status), close_(lux::editor::detail::takeConnection(
                                                            lux::object::LuxObject::connect(
                                                                this,
                                                                &lux::ui::Pane::closeRequested,
                                                                [this]() noexcept { hide_ = true; }
                                                            ),
                                                            status
                                                        ))
        {
            setContent(content_);
        }

    private:
        void update() noexcept override
        {
            if (std::exchange(hide_, false))
                setVisible(false);
        }
        OutlinerElement content_;
        bool hide_{};
        object::Connection close_;
    };
} // namespace lux::editor::ui
