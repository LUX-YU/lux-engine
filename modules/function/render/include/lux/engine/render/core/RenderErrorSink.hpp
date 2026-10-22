#pragma once

#include <lux/engine/render/core/Error.hpp>
#include <string_view>

namespace lux::render
{
    enum class ERenderDiagnosticSeverity : std::uint8_t
    {
        INFORMATION,
        WARNING,
        ERROR_MESSAGE
    };

    struct RenderDiagnostic final
    {
        ERenderDiagnosticSeverity severity;
        RenderError error;
        std::string_view message;
        std::string_view file;
        std::uint32_t line = 0;
    };

    // Borrowed until RenderServer destruction, including device initialization and teardown.
    // Calls may originate on compilation workers. The sink must be thread-safe, must not reenter
    // RenderServer, and must copy any text it retains beyond the callback. No output destination is implied.
    struct RenderErrorSink final
    {
        void* context = nullptr;
        void (*report)(void*, const RenderDiagnostic&) noexcept = nullptr;
    };
} // namespace lux::render
