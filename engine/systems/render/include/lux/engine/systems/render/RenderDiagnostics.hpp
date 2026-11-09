#pragma once

#include <lux/engine/systems/render/visibility.h>

#include <lux/engine/render/core/RenderErrorSink.hpp>

namespace lux::engine::render
{
    // Application initialization. Definitions go into the existing process error directory once;
    // failures preserve its previously published definitions.
    [[nodiscard]] LUX_ENGINE_SYSTEM_RENDER_PUBLIC lux::render::RenderResult<void> registerRenderErrors() noexcept;

    // One thread-safe bridge to the application's selected lux::log output. No captured business state.
    [[nodiscard]] LUX_ENGINE_SYSTEM_RENDER_PUBLIC lux::render::RenderErrorSink renderDiagnostics() noexcept;

    // For a caller that has decided to report an otherwise unhandled Command/Frame failure.
    LUX_ENGINE_SYSTEM_RENDER_PUBLIC void
    reportRenderFailure(lux::render::RenderError error, std::string_view operation) noexcept;
} // namespace lux::engine::render
