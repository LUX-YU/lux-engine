#pragma once

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
    void diagnoseRenderBridge(const char* format, Args... args) noexcept
    {
        if (!renderBridgeDiagnosticsEnabled())
            return;

        char buffer[320];
#if defined(__clang__) || defined(__GNUC__)
#   pragma GCC diagnostic push
#   pragma GCC diagnostic ignored "-Wformat-security"
#   pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
        const int count = std::snprintf(
            buffer,
            sizeof(buffer),
            format,
            args...
        );
#if defined(__clang__) || defined(__GNUC__)
#   pragma GCC diagnostic pop
#endif
        if (count <= 0)
            return;

        const auto length = static_cast<std::size_t>(count) < sizeof(buffer)
            ? static_cast<std::size_t>(count)
            : sizeof(buffer) - 1;
        emitRenderBridgeDiagnostic({buffer, length});
    }
} // namespace lux::ecs
