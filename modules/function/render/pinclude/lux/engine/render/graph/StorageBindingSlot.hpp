#pragma once

#include <lux/engine/render/extensions/graph/GraphResource.hpp>

namespace lux::render::detail
{
    struct StorageBindingSlot final
    {
        GraphResourceId resource;
        std::optional<GraphBufferRange> output_range;
        std::uint32_t first_use = 0;
        std::uint32_t use_count = 0;
        std::uint32_t canonical_queue = kGraphInvalidIndex;
    };
}
