#include <lux/engine/rendering/RenderDiagnostics.hpp>

#include <lux/engine/error/ErrorRegistry.hpp>
#include <lux/engine/log/Log.hpp>
#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/commands/Error.hpp>

namespace lux::rendering
{
    namespace
    {
        void report(void*, const render::RenderDiagnostic& diagnostic) noexcept
        {
            auto level = log::ELevel::LOG_INFO;
            if (diagnostic.severity == render::ERenderDiagnosticSeverity::WARNING)
            {
                level = log::ELevel::LOG_WARN;
            }
            else if (diagnostic.severity == render::ERenderDiagnosticSeverity::ERROR_MESSAGE)
            {
                level = log::ELevel::LOG_ERROR;
            }
            const auto error = error::format(diagnostic.error);
            log::logf(level, "Render", "{} {}:{}", error, diagnostic.file, diagnostic.line);
            // Backend diagnostics can exceed the fixed LogRecord argument capacity. Split borrowed
            // text at this bridge so the chosen application outlet receives every part.
            constexpr auto maximum = log::kLogArgCapacity - sizeof(std::uint16_t);
            for (std::size_t offset = 0; offset < diagnostic.message.size(); offset += maximum)
            {
                log::logf(level, "Render", "{}", diagnostic.message.substr(offset, maximum));
            }
        }
    } // namespace

    render::RenderResult<void> registerRenderErrors() noexcept
    {
        for (const auto table : render::renderErrorDescriptorTables())
        {
            if (auto registered = error::ErrorRegistry::instance().registerTypes(table); !registered)
            {
                return cxx::unexpected(registered.error());
            }
        }
        return {};
    }

    render::RenderErrorSink renderDiagnostics() noexcept
    {
        return {nullptr, report};
    }

    void reportRenderFailure(render::RenderError failure, std::string_view operation) noexcept
    {
        const auto text = error::format(failure);
        log::error("Render", "{}: {}", operation, text);
    }
} // namespace lux::rendering
