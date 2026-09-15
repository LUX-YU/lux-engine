#pragma once

#include <lux/engine/editor/gui/scene/DocumentPane.hpp>
#include <lux/engine/editor/gui/scene/SceneCamera.hpp>
#include <lux/engine/editor/rendering/EditorRenderer.hpp>
#include <lux/engine/ui/ViewportElement.hpp>

namespace lux::editor::gui
{
    class ScenePane final : public DocumentPane<ScenePane>
    {
      public:
        ScenePane(scene::SceneEditor &, rendering::EditorRenderer &, std::string);
        void poll(PollBudget &) override;
        void requestClose() noexcept override;
        CloseStatus closeStatus() const override;
        void appendFrameImages(std::vector<rendering::ViewImage> &) const override;
        void releaseFrameImages() noexcept override;

      private:
        void draw(lux::ui::Frame &, lux::ui::PaneDrawContext &) override;
        void updateCamera(rendering::PixelExtent);

        rendering::EditorRenderer &renderer_;
        SceneCamera camera_;
        lux::ui::ViewportElement viewport_;
        std::unique_ptr<rendering::RenderView> view_;
        rendering::ViewImage image_;
        std::string status_;
        std::uint64_t camera_revision_{1};
        std::uint64_t applied_camera_revision_{};
        rendering::PixelExtent camera_extent_;
        bool rotating_{}, panning_{}, closed_{};
    };
} // namespace lux::editor::gui
