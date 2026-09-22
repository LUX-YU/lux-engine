#pragma once

#include <lux/engine/render/ViewImage.hpp>
#include <lux/engine/ui/Context.hpp>
#include <lux/engine/ui/UiFrameSnapshot.hpp>
#include <lux/engine/ui/rendering/genops/UiRenderOperation.ops.hpp>
#include <lux/engine/ui/rendering/visibility.h>

namespace lux::ui
{
// The producer may reuse a slot only after every Program/Feature borrow returns.
// Scene use is retained separately by the Program, avoiding a Feature/Scene cycle.
struct UiRenderFrame final
{
    UiFrameSnapshot snapshot;
    std::vector<render::ViewImage> images;
    std::uint64_t sequence{};
};

[[nodiscard]] LUX_UI_RENDERING_PUBLIC lux::cxx::expected<std::vector<std::byte>, EUiInitError>
makeUiRenderConfiguration(Context &context);

// Reject before changing the builder. A failed submission retains this same
// captured input; callers must not rebuild UI while retrying the Program.
[[nodiscard]] LUX_UI_RENDERING_PUBLIC render::Expected<void> appendUiFrame(
    render::RenderProgramSession::Builder &builder, const render::UiRenderOperationIds &operations,
    const render::RenderSceneLease &scene, render::FeatureHandle feature,
    const std::shared_ptr<const UiRenderFrame> &input);
[[nodiscard]] LUX_UI_RENDERING_PUBLIC render::Expected<void> appendUiClear(render::RenderProgramSession::Builder &,
                                                                           const render::UiRenderOperationIds &,
                                                                           const render::RenderSceneLease &,
                                                                           render::FeatureHandle);

} // namespace lux::ui
