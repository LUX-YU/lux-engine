#pragma once

#include <cstdint>
#include <lux/engine/render/visibility.h>
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
} // namespace lux::render
