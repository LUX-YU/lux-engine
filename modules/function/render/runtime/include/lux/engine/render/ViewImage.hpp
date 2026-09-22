#pragma once
#include <lux/engine/render/RendererConfig.hpp>

namespace lux::render
{
namespace detail
{
struct ViewImageAccess;
}
class RenderRuntime;
class RenderView;

class LUX_RENDER_RUNTIME_PUBLIC ViewImageLease final
{
  public:
    ViewImageLease() noexcept;
    ~ViewImageLease() noexcept;
    ViewImageLease(const ViewImageLease &) noexcept;
    ViewImageLease &operator=(const ViewImageLease &) noexcept;
    ViewImageLease(ViewImageLease &&) noexcept;
    ViewImageLease &operator=(ViewImageLease &&) noexcept;
    [[nodiscard]] bool valid() const noexcept;

  private:
    friend class RenderRuntime;
    friend class RenderView;
    friend struct detail::ViewImageAccess;
    struct Record;
    std::shared_ptr<const Record> record_;
};

struct ViewImage final
{
    std::uint64_t texture{}; // Opaque sampled-image token; independent of any UI toolkit.
    PixelExtent extent;
    RenderViewId view;
    ImageContentStamp content;
    ViewImageLease lease;
};

struct ViewStatus final
{
    EViewState state{};
    RenderViewId view;
    PixelExtent requested_extent{}, ready_extent{};
    std::uint64_t request_sequence{}, acknowledged_sequence{};
    std::optional<RendererFailure> failure;
};
} // namespace lux::render
