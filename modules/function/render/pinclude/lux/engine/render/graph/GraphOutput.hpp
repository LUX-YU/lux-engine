#pragma once

#include <lux/engine/render/graph/GraphResource.hpp>

namespace lux::render
{
    [[nodiscard]] RenderResult<void> validateGraphOutput(const GraphOutputRequirements& output) noexcept;
}
