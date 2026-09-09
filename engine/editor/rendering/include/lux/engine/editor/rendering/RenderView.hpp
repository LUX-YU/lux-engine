#pragma once
#include <lux/engine/editor/rendering/ViewImage.hpp>
namespace lux::editor::rendering
{
    class LUX_EDITOR_RENDERING_PUBLIC RenderView final
    {
      public:
        ~RenderView() noexcept; // Requires COMPLETE; no drain/yield/wait in destructor.
        RenderView(const RenderView &) = delete;
        RenderView &operator=(const RenderView &) = delete;
        RenderView(RenderView &&) = delete;
        RenderView &operator=(RenderView &&) = delete;
        [[nodiscard]] RenderViewId id() const noexcept;
        [[nodiscard]] ViewStatus status() const noexcept;
        [[nodiscard]] RenderResult<void> requestExtent(PixelExtent) noexcept;
        [[nodiscard]] RenderResult<void> setCamera(const CameraFrame &) noexcept;
        [[nodiscard]] RenderResult<ViewImage> acquireImage() noexcept;
        [[nodiscard]] RenderResult<void> beginClose() noexcept;
        [[nodiscard]] RenderResult<ERenderClose> advanceClose() noexcept;

      private:
        friend class EditorRenderer;
        friend class detail::ViewResources;
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
