#pragma once

#include <lux/engine/editor/gui/DocumentPane.hpp>
#include <lux/engine/editor/gui/scene/SpatialViewport.hpp>
#include <lux/engine/editor/scene/SceneEditor.hpp>
#include <lux/engine/render/RenderRuntime.hpp>
#include <lux/engine/ui/ViewportElement.hpp>

namespace lux::editor::gui
{
class ScenePane final : public DocumentPane<ScenePane, scene::SceneEditor>
{
  public:
    ScenePane(scene::SceneEditor &, lux::render::RenderRuntime &, std::string,
              std::span<const SpatialViewportRegistration> = {});
    void poll(PollBudget &) override;
    void requestClose() noexcept override;
    CloseStatus closeStatus() const override;
    void appendFrameImages(std::vector<lux::render::ViewImage> &) const override;
    void releaseFrameImages() noexcept override;

  private:
    void draw(lux::ui::Frame &, lux::ui::PaneDrawContext &) override;
    void updateCamera(lux::render::PixelExtent);
    void drawPlacement(lux::ui::Frame &);

    lux::render::RenderRuntime &renderer_;
    std::unique_ptr<SpatialViewport> spatial_;
    scene::SceneEntityRef camera_;
    CameraMotion pending_motion_;
    bool navigation_pending_{};
    struct Pick final
    {
        lux::scene::SceneInstanceId instance;
        lux::math::Ray3d ray;
        scene::SceneEntityRef camera;
        std::uint64_t selection_revision{};
    };
    struct Create final
    {
        Pick location;
        AssetReference asset;
        lux::partition::PartitionOrdinal partition;
        double plane;
        editing::StateId base;
    };
    std::variant<std::monostate, Pick, Create> action_;
    double work_plane_height_{};
    lux::ui::ViewportElement viewport_;
    std::unique_ptr<lux::render::RenderView> view_owner_;
    scene::RunId displayed_run_;
    EditorResult<void> view_result_;
    bool reopen_{};
    lux::render::RenderView *view() noexcept;
    void remember(std::string_view, const lux::render::RendererFailure &);
    lux::render::ViewImage image_;
    std::string status_;
    lux::render::PixelExtent camera_extent_;
    scene::ModelCreationId placement_;
    std::uint32_t partition_{};
    bool rotating_{}, panning_{}, closed_{};
};
} // namespace lux::editor::gui
