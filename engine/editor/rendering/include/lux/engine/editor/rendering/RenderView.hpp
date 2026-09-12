#pragma once
#include <lux/engine/editor/rendering/ViewImage.hpp>
namespace lux::editor::rendering
{
    struct RenderViewCloseStatus final
    {
        ViewStatus status;
        bool close_requested{}, layer_attached{}, target_owned{}, view_owned{};
        // Includes the View's own version reference; aliases of one image record share a version owner.
        long image_version_owners{};
        std::uint64_t last_submission{}, gpu_completed{};
        std::uint64_t create_view{}, create_target{}, resize{}, release_view{}, release_target{};
    };
    namespace detail
    {
        struct RendererTestAccess;
    }
    class LUX_EDITOR_RENDERING_PUBLIC RenderView final
    {
      public:
        ~RenderView() noexcept; // Requires COMPLETE; no drain/yield/wait in destructor.
        RenderView(const RenderView &) = delete;
        RenderView &operator=(const RenderView &) = delete;
        RenderView(RenderView &&) = delete;
        RenderView &operator=(RenderView &&) = delete;
        // Non-result queries require the owning thread; returned values do not extend this owner's lifetime.
        [[nodiscard]] RenderViewId id() const noexcept;
        [[nodiscard]] ViewStatus status() const noexcept;
        // Observes values only, including CPU ownership and actual GPU completion independently; never polls.
        [[nodiscard]] RenderResult<RenderViewCloseStatus> closeStatus() const noexcept;
        [[nodiscard]] RenderResult<void> requestExtent(PixelExtent) noexcept;
        [[nodiscard]] RenderResult<void> setCamera(const CameraFrame &) noexcept;
        [[nodiscard]] RenderResult<ViewImage> acquireImage() noexcept;
        [[nodiscard]] RenderResult<void> beginClose() noexcept;
        [[nodiscard]] RenderResult<ERenderClose> advanceClose() noexcept;

      private:
        friend class EditorRenderer;
        friend class detail::ViewResources;
        friend struct detail::RendererTestAccess;
        static RenderResult<std::unique_ptr<RenderView>> create(EditorRenderer &, lux::render::RenderControlSession &,
                                                                RenderViewId, lux::render::RenderSceneId,
                                                                ViewConfig) noexcept;
        RenderResult<void> pollResources() noexcept;
        std::optional<RendererFailure> takeFailure() noexcept;
        struct Impl;
        explicit RenderView(std::unique_ptr<Impl>) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor::rendering
