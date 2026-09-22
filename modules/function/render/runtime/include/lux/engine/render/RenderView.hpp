#pragma once
#include <lux/engine/render/ViewImage.hpp>

namespace lux::render
{
namespace detail
{
class ViewResources;
}

// A value observation does not extend the View's use or borrow its public owner.
struct ViewObservation final
{
    RenderSceneId scene;
    ViewHandle handle;
    ViewStatus status;
};

class LUX_RENDER_RUNTIME_PUBLIC RenderView final
{
  public:
    ~RenderView() noexcept;
    RenderView(const RenderView &) = delete;
    RenderView &operator=(const RenderView &) = delete;

    [[nodiscard]] RenderViewId id() const noexcept;
    [[nodiscard]] ViewStatus status() const noexcept;
    [[nodiscard]] ViewHandle handle() const noexcept;
    [[nodiscard]] RenderResult<void> requestExtent(PixelExtent) noexcept;
    [[nodiscard]] RenderResult<void> setOutput(ViewStamp, bool scene_enabled) noexcept;
    [[nodiscard]] RenderResult<ViewImage> acquireImage();
    [[nodiscard]] RenderResult<void> beginClose() noexcept;
    [[nodiscard]] RenderResult<ERenderClose> advanceClose() const noexcept;

  private:
    friend class RenderRuntime;
    explicit RenderView(std::shared_ptr<detail::ViewResources>) noexcept;
    // Runtime retains the already-admitted record after this handle exits.
    // No asynchronous cleanup obligation is left in this object's destructor.
    std::shared_ptr<detail::ViewResources> resources_;
};
} // namespace lux::render
