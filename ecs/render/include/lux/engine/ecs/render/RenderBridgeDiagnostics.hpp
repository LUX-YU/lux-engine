#pragma once

#include <lux/cxx/core/Format.hpp>

#include <utility>

#include <cstddef>
#include <cstdio>
#include <functional>
#include <string_view>

#include <lux/engine/function/visibility.h>
#include <lux/engine/function/render/client/core/RenderError.hpp>

namespace lux::ecs
{
    using RenderBridgeDiagnosticSink = std::function<void(std::string_view)>;

    LUX_FUNCTION_PUBLIC void setRenderBridgeDiagnosticSink(
        RenderBridgeDiagnosticSink sink
    );

    LUX_FUNCTION_PUBLIC void emitRenderBridgeDiagnostic(
        std::string_view message
    ) noexcept;

    [[nodiscard]] LUX_FUNCTION_PUBLIC bool
    renderBridgeDiagnosticsEnabled() noexcept;

    [[nodiscard]] LUX_FUNCTION_PUBLIC lux::render::ERecovery
    reportRenderBridgeFailure(
        std::string_view source,
        std::string_view operation,
        const lux::render::RenderError& error
    ) noexcept;

    [[nodiscard]] LUX_FUNCTION_PUBLIC lux::render::ERecovery
    renderBridgeFailureRecovery(
        const lux::render::RenderError& error
    ) noexcept;

    template <class... Args>
    void diagnoseRenderBridge(lux::format_string<Args...> format, Args&&... args) noexcept
    {
        if (!renderBridgeDiagnosticsEnabled())
            return;

        try
        {
            const auto message = lux::format(format, std::forward<Args>(args)...);
            emitRenderBridgeDiagnostic(message);
        }
        catch (...)
        {
            // Diagnostics must never affect the render bridge operation.
        }
    }
} // namespace lux::ecs
