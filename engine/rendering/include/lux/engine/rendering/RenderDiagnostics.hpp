#pragma once

#include <lux/engine/render/core/RenderErrorSink.hpp>

namespace lux::rendering
{
    // Application initialization. Definitions go into the existing process error directory once;
    // failures preserve its previously published definitions.
    [[nodiscard]] render::RenderResult<void> registerRenderErrors() noexcept;

    // One thread-safe bridge to the application's selected lux::log output. No captured business state.
    [[nodiscard]] render::RenderErrorSink renderDiagnostics() noexcept;

    // For a caller that has decided to report an otherwise unhandled Command/Frame failure.
    void reportRenderFailure(render::RenderError error, std::string_view operation) noexcept;
} // namespace lux::rendering
