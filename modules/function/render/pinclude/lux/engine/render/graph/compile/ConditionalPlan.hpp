#pragma once

#include <lux/engine/render/graph/RenderPlan.hpp>

namespace lux::render::detail
{
    void planConditions(const RenderGraph&, RecordingProgram&) noexcept;
}
