#pragma once

#include <lux/engine/render/core/Error.hpp>

namespace lux::render
{
    inline constexpr auto kOutcomeUnknown = error::errorId("lux.render.runtime.outcome_unknown");
    inline constexpr auto kCommandCancelled = error::errorId("lux.render.runtime.command_cancelled");
    inline constexpr auto kRuntimeState = error::errorId("lux.render.runtime.state");
    inline constexpr auto kRuntimeCapacity = error::errorId("lux.render.runtime.capacity");
    inline constexpr auto kRuntimeIdentity = error::errorId("lux.render.runtime.identity");
    inline constexpr auto kRuntimeUnsupported = error::errorId("lux.render.runtime.unsupported");

    [[nodiscard]] inline std::span<const error::ErrorDescriptor> renderRuntimeErrorDescriptors() noexcept
    {
        static constexpr error::ErrorDescriptor descriptors[]{
            {"lux.render.runtime.outcome_unknown", "Accepted side effects have an unknown outcome"},
            {"lux.render.runtime.command_cancelled", "Command stopped before commitment"},
            {"lux.render.runtime.state", "Invalid runtime state"},
            {"lux.render.runtime.capacity", "Runtime capacity exhausted"},
            {"lux.render.runtime.identity", "Stale or foreign runtime identity"},
            {"lux.render.runtime.unsupported", "Unsupported runtime command"}
        };
        return descriptors;
    }
} // namespace lux::render
