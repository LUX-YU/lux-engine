#pragma once

#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/core/execution/CompletionPost.hpp>

namespace lux::render
{
    inline constexpr auto kCpuStopped = error::errorId("lux.render.resource.cpu_stopped");
    inline constexpr auto kCpuScheduleFailure = error::errorId("lux.render.resource.cpu_schedule_failure");

    using CpuResume = execution::CpuResume<RenderError>;
    using CompletionPost = execution::CompletionPost<RenderError>;
}
