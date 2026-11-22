#pragma once

#include <lux/engine/render/extensions/graph/GraphResource.hpp>

namespace lux::render
{
    struct FinalBufferVersion final
    {
        RenderJobId writer;
        std::uint64_t frame_index;
        std::uint32_t slot_index;
        GraphBufferRange range;
        bool operator==(const FinalBufferVersion&) const noexcept = default;
    };
}
