#pragma once

#include <lux/engine/render/core/Error.hpp>

namespace lux::render
{
    inline constexpr auto kJobState = error::errorId("lux.render.job.invalid_state");
    inline constexpr auto kJobBusy = error::errorId("lux.render.job.busy");
    inline constexpr auto kJobIdentity = error::errorId("lux.render.job.identity");
    inline constexpr auto kJobCycle = error::errorId("lux.render.job.cycle");
    inline constexpr auto kJobStopped = error::errorId("lux.render.job.stopped");
    inline constexpr auto kTargetInUse = error::errorId("lux.render.target.in_use");

    inline constexpr auto kCommandCancelled = error::errorId("lux.render.runtime.command_cancelled");
    inline constexpr auto kRuntimeState = error::errorId("lux.render.runtime.state");
    inline constexpr auto kRuntimeCapacity = error::errorId("lux.render.runtime.capacity");
    inline constexpr auto kRuntimeIdentity = error::errorId("lux.render.runtime.identity");
    inline constexpr auto kRuntimeUnsupported = error::errorId("lux.render.runtime.unsupported");
    inline constexpr auto kFrameContract = error::errorId("lux.render.frame.contract");

    [[nodiscard]] inline std::span<const error::ErrorDescriptor> renderRuntimeErrorDescriptors() noexcept
    {
        static constexpr error::ErrorDescriptor descriptors[]{
            {"lux.render.job.invalid_state", "Invalid Job state"},
            {"lux.render.job.busy", "Job has an active operation"},
            {"lux.render.job.identity", "Stale or foreign Job identity"},
            {"lux.render.job.cycle", "Cyclic Job dependencies"},
            {"lux.render.job.stopped", "Job admission stopped"},
            {"lux.render.target.in_use", "Target writer already reserved"},
            {"lux.render.runtime.command_cancelled", "Command stopped before commitment"},
            {"lux.render.runtime.state", "Invalid runtime state"},
            {"lux.render.runtime.capacity", "Runtime capacity exhausted"},
            {"lux.render.runtime.identity", "Stale or foreign runtime identity"},
            {"lux.render.runtime.unsupported", "Unsupported runtime command"},
            {"lux.render.frame.contract", "Invalid frame data contract"},
        };
        return descriptors;
    }
} // namespace lux::render
