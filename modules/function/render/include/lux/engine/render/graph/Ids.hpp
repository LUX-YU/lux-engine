#pragma once

#include <cstdint>
#include <limits>

namespace lux::render
{
    inline constexpr auto kGraphInvalidIndex = std::numeric_limits<std::uint32_t>::max();

    // Definition-local indices. Equal numbers in different definitions are unrelated.
    struct GraphStorageSnapshotId final
    {
        std::uint32_t index = kGraphInvalidIndex;
        bool operator==(const GraphStorageSnapshotId&) const noexcept = default;
    };

    struct GraphPassId final
    {
        std::uint32_t index = kGraphInvalidIndex;
        bool operator==(const GraphPassId&) const noexcept = default;
    };

    struct GraphResourceId final
    {
        std::uint32_t index = kGraphInvalidIndex;
        bool operator==(const GraphResourceId&) const noexcept = default;
    };
} // namespace lux::render
