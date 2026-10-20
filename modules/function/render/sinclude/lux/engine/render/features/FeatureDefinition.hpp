#pragma once

#include <lux/engine/render/features/FeatureAssembly.hpp>

namespace lux::render
{
    // Cold graph contribution functions; no execution state.
    struct FeatureDefinition final
    {
        const FeatureDescriptor* descriptor;
        const FeatureGraphOps* graph;
    };
} // namespace lux::render
