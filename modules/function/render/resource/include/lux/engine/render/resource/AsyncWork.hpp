#pragma once

#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/core/execution/WorkSchedule.hpp>

namespace lux::render
{
    inline constexpr auto kCpuStopped = error::errorId("lux.render.resource.cpu_stopped");
    inline constexpr auto kCpuScheduleFailure = error::errorId("lux.render.resource.cpu_schedule_failure");

    using CpuSender = execution::CpuSender<RenderError>;
    using CpuResume = execution::CpuResume<RenderError>;
    using CompletionPost = execution::CompletionPost<RenderError>;
    using CpuSchedule = execution::CpuSchedule<RenderError>;
}
