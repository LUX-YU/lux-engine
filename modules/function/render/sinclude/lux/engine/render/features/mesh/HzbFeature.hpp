#pragma once

#include <lux/engine/render/extensions/graph/GraphResource.hpp>

#include <lux/engine/render/extensions/features/FeatureGraphOps.hpp>
#include <lux/engine/render/visibility.h>

namespace lux::render
{
    // Forward ZO depth: near=0, far/clear=1. A pyramid texel stores the farthest covered depth.
    struct HzbFeatureConfig final
    {
        GraphImageDesc depth;
        std::uint32_t mip_count = 1;
    };

} // namespace lux::render
