#pragma once
#include <lux/engine/render/RenderRuntime.hpp>
#include <lux/engine/render/detail/ViewImageLifetime.hpp>
#include <thread>

namespace lux::render::detail
{
class ViewResources final
{
  public:
    ViewResources(RenderRuntime &, lux::render::RenderControlSession &, RenderViewId, RenderSceneLease, ViewConfig);
    ~ViewResources() noexcept;
    RenderResult<void> check() const noexcept;
    RenderResult<void> prepareControlStep(std::size_t &budget);
    RenderResult<void> requestExtent(PixelExtent) noexcept;
    RenderResult<void> beginClose() noexcept;
    bool drained() const noexcept;
    void acceptReplies() noexcept;
    void rememberFailure(lux::render::RenderError, std::uint64_t,
                         std::optional<std::uint32_t> backend_status = {}) noexcept;

    const std::thread::id owner{std::this_thread::get_id()};
    RenderRuntime &renderer;
    lux::render::RenderControlSession &control;
    ViewStatus status;
    std::variant<SampledOutput, NativeSurfaceOutput> output;
    std::optional<RendererFailure> pending_failure;
    lux::render::RenderSceneId scene;
    RenderSceneLease scene_use;
    ViewStamp desired;
    bool scene_enabled{};
    bool linked{}, view_requested{}, target_requested{}, close_requested{};
    std::uint64_t in_flight_sequence{};
    PixelExtent in_flight_extent;
    std::shared_ptr<ImageVersion> version;
    lux::render::RenderRequest<lux::render::ViewCreatedReply> create_view;
    lux::render::RenderRequest<lux::render::TargetReadyReply> create_target;
    lux::render::RenderRequest<lux::render::TargetResizedReply> resize;
    lux::render::RenderRequest<lux::render::GenericOkReply> release_view;
    lux::render::RenderRequest<lux::render::TargetReleasedReply> release_target;
    lux::render::ViewHandle view;
    lux::render::RenderTargetId target;
};
} // namespace lux::render::detail
