#pragma once

#include <lux/engine/render/core/Error.hpp>

namespace lux::render
{
    inline constexpr auto kCommandCancelled = error::errorId("lux.render.runtime.command_cancelled");
    inline constexpr auto kRuntimeState = error::errorId("lux.render.runtime.state");
    inline constexpr auto kRuntimeCapacity = error::errorId("lux.render.runtime.capacity");
    inline constexpr auto kRuntimeIdentity = error::errorId("lux.render.runtime.identity");
    inline constexpr auto kRuntimeUnsupported = error::errorId("lux.render.runtime.unsupported");
    inline constexpr auto kFrameSource = error::errorId("lux.render.frame.source");
    inline constexpr auto kFrameBoundary = error::errorId("lux.render.frame.boundary");
    inline constexpr auto kFrameResynchronization = error::errorId("lux.render.frame.resynchronization");

    [[nodiscard]] inline std::span<const error::ErrorDescriptor> renderRuntimeErrorDescriptors() noexcept
    {
        static constexpr error::ErrorDescriptor descriptors[]{
            {"lux.render.runtime.command_cancelled", "Command stopped before commitment"},
            {"lux.render.runtime.state", "Invalid runtime state"},
            {"lux.render.runtime.capacity", "Runtime capacity exhausted"},
            {"lux.render.runtime.identity", "Stale or foreign runtime identity"},
            {"lux.render.runtime.unsupported", "Unsupported runtime command"},
            {"lux.render.frame.source", "Frame submitted by a different IPC source"},
            {"lux.render.frame.boundary", "Command requires a sealed frame or a frame boundary"},
            {"lux.render.frame.resynchronization", "Frame state requires explicit Client resynchronization"}
        };
        return descriptors;
    }
} // namespace lux::render
