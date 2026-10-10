#pragma once

#include <lux/engine/function/render/client/RenderProgramSession.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/ui/DrawData.hpp>
#include <lux/engine/ui/rendering/genops/UiRenderOperation.ops.hpp>
#include <lux/engine/ui/rendering/visibility.h>

namespace lux::ui
{
    // The producer may reuse a slot only after every Program/Feature borrow returns.
    // The host retains the destination Scene separately in the Program. Installed
    // UI content keeps only passive resource submissions, never its Scene owner.
    struct RenderFrame final
    {
        DrawData draw_data;
        std::vector<render::RenderSubmissionState> resources;
        std::uint64_t sequence{};
        // Written/observed by the producer only. Backend content owns a counted
        // RenderSubmissionState independently of this passive slot observation.
        mutable render::RenderSubmissionState::Observer submission;
    };

    [[nodiscard]] LUX_UI_RENDERING_PUBLIC lux::cxx::expected<std::vector<std::byte>, EInitError>
    makeRenderConfiguration(const Root& root);

    // Reject before changing the builder. A failed submission retains this same
    // captured input; callers must not rebuild UI while retrying the Program.
    // The host fixes every DrawData texture's lifetime in resources, and retains
    // the destination Scene until command adoption. Render resolves texture kinds
    // and checks actual target dependencies before recording a GPU frame.
    [[nodiscard]] LUX_UI_RENDERING_PUBLIC render::Expected<void> appendFrame(
        render::RenderProgramSession::Builder& builder,
        const render::UiRenderOperationIds& operations,
        render::RenderSceneId scene,
        render::FeatureHandle feature,
        const std::shared_ptr<const RenderFrame>& input
    );
    [[nodiscard]] LUX_UI_RENDERING_PUBLIC render::Expected<void> appendClear(
        render::RenderProgramSession::Builder&,
        const render::UiRenderOperationIds&,
        render::RenderSceneId,
        render::FeatureHandle
    );

} // namespace lux::ui
