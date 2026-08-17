#include <lux/engine/ecs/render/RenderBridgeDiagnostics.hpp>

#include <lux/engine/function/render/client/core/RenderErrorRegistry.hpp>

#include <string>
#include <utility>

namespace lux::ecs
{
    namespace
    {
        RenderBridgeDiagnosticSink& diagnosticSink() noexcept
        {
            static RenderBridgeDiagnosticSink sink;
            return sink;
        }
    }

    void setRenderBridgeDiagnosticSink(RenderBridgeDiagnosticSink sink)
    {
        diagnosticSink() = std::move(sink);
    }

    bool renderBridgeDiagnosticsEnabled() noexcept
    {
        return static_cast<bool>(diagnosticSink());
    }

    void emitRenderBridgeDiagnostic(std::string_view message) noexcept
    {
        if (auto& sink = diagnosticSink())
            sink(message);
    }

    lux::render::ERecovery renderBridgeFailureRecovery(
        const lux::render::RenderError& error
    ) noexcept
    {
        if (error.ok())
            return lux::render::ERecovery::Permanent;
        const auto descriptor =
            lux::render::renderErrorRegistry().find(error.type);
        return descriptor
            ? descriptor->recovery
            : lux::render::ERecovery::Permanent;
    }

    lux::render::ERecovery reportRenderBridgeFailure(
        std::string_view source,
        std::string_view operation,
        const lux::render::RenderError& error
    ) noexcept
    {
        const auto recovery = renderBridgeFailureRecovery(error);
        if (!renderBridgeDiagnosticsEnabled())
            return recovery;

        if (error.ok())
        {
            diagnoseRenderBridge(
                "[{}] {} dispatch failed without a structured RenderError",
                source,
                operation
            );
            return recovery;
        }

        const auto text = lux::render::formatRenderError(
            lux::render::renderErrorRegistry(),
            error
        );
        diagnoseRenderBridge(
            "[{}] {} dispatch failed: {}",
            source,
            operation,
            text
        );
        return recovery;
    }
} // namespace lux::ecs
