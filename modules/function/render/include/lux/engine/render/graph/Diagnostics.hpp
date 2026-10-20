#pragma once

#include <lux/engine/render/graph/RenderGraphAnalysis.hpp>
#include <string_view>

namespace lux::render
{
    enum class EGraphDumpFormat : std::uint8_t
    {
        JSON,
        DOT
    };

    // Synchronous borrowed output. The caller owns opening, flushing and closing its destination.
    struct DumpSink final
    {
        void* destination = nullptr;
        bool (*write)(void*, std::string_view) noexcept = nullptr;
    };

    [[nodiscard]] LUX_RENDER_PUBLIC bool dumpGraph(
        const RenderGraph&,
        const RenderGraphAnalysis&,
        EGraphDumpFormat,
        DumpSink
    ) noexcept;
} // namespace lux::render
