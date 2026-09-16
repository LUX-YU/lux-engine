#pragma once

#include <lux/engine/editor/gui/DocumentPane.hpp>
#include <lux/engine/editor/gui/scene/SceneCamera.hpp>
#include <lux/engine/editor/rendering/RenderView.hpp>
#include <lux/engine/editor/scene/SceneEditor.hpp>
#include <lux/engine/ui/ViewportElement.hpp>

namespace lux::editor::gui
{
    class RunPane final : public DocumentPane<RunPane, scene::SceneEditor>
    {
      public:
        RunPane(scene::SceneEditor &, std::string);
        EditorResult<void> attach(lux::ui::UISession &session)
        {
            return DocumentPane::attach(session, false);
        }
        void poll(PollBudget &) override;
        CloseStatus closeStatus() const override;
        void appendFrameImages(std::vector<rendering::ViewImage> &) const override;
        void releaseFrameImages() noexcept override;

      private:
        void draw(lux::ui::Frame &, lux::ui::PaneDrawContext &) override;
        void remember(std::string_view operation, const rendering::RendererFailure &);
        struct Idle final
        {
        };
        struct Preview final
        {
            scene::RunId run;
            scene::RunViewLease lease;
            rendering::ViewImage image;
            rendering::PixelExtent camera_extent;
        };
        std::variant<Idle, Preview> preview_;
        scene::RunId observed_;
        SceneCamera camera_;
        lux::ui::ViewportElement viewport_;
        std::string status_;
        EditorResult<void> view_result_;
        bool view_closing_{};
        bool reopen_requested_{};
    };
} // namespace lux::editor::gui
