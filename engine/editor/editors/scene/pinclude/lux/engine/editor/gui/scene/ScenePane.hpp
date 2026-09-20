#pragma once

#include <lux/engine/editor/gui/DocumentPane.hpp>
#include <lux/engine/editor/gui/scene/SpatialViewport.hpp>
#include <lux/engine/editor/rendering/EditorRenderer.hpp>
#include <lux/engine/editor/scene/SceneEditor.hpp>
#include <lux/engine/ui/ViewportElement.hpp>

namespace lux::editor::gui
{
    class ScenePane final : public DocumentPane<ScenePane, scene::SceneEditor>
    {
      public:
        ScenePane(scene::SceneEditor &, rendering::EditorRenderer &, std::string,
                  std::span<const SpatialViewportRegistration> = {});
        void poll(PollBudget &) override;
        void requestClose() noexcept override;
        CloseStatus closeStatus() const override;
        void appendFrameImages(std::vector<rendering::ViewImage> &) const override;
        void releaseFrameImages() noexcept override;

      private:
        void draw(lux::ui::Frame &, lux::ui::PaneDrawContext &) override;
        void updateCamera(rendering::PixelExtent);
        void drawPlacement(lux::ui::Frame &);

        rendering::EditorRenderer &renderer_;
        std::unique_ptr<SpatialViewport> spatial_;
        scene::SceneEntityRef camera_;
        CameraMotion pending_motion_;
        bool navigation_pending_{};
        struct Pick final
        {
            scene::SceneInstanceId instance;
            lux::math::Ray3d ray;
            scene::SceneEntityRef camera;
            std::uint64_t selection_revision{};
        };
        struct Create final { Pick location; AssetReference asset; lux::partition::PartitionOrdinal partition; double plane; editing::StateId base; };
        std::variant<std::monostate, Pick, Create> action_;
        double work_plane_height_{};
        lux::ui::ViewportElement viewport_;
        using ViewOwner = std::variant<std::monostate, std::unique_ptr<rendering::RenderView>, scene::RunViewLease>;
        ViewOwner view_owner_;
        scene::RunId displayed_run_;
        EditorResult<void> view_result_;
        bool reopen_{};
        rendering::RenderView *view() noexcept;
        void remember(std::string_view, const rendering::RendererFailure &);
        rendering::ViewImage image_;
        std::string status_;
        rendering::PixelExtent camera_extent_;
        scene::ModelCreationId placement_;
        std::uint32_t partition_{};
        bool rotating_{}, panning_{}, closed_{};
    };
} // namespace lux::editor::gui
