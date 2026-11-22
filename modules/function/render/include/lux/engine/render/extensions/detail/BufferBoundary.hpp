#pragma once

#include <lux/engine/render/extensions/graph/GraphResource.hpp>

namespace lux::render::detail
{
    // A valid physical queue proves every use preserves the declared whole-buffer boundary on that queue.
    struct BufferBoundary final
    {
        EGraphUse use = EGraphUse::UNSPECIFIED;
        std::uint32_t queue = kGraphInvalidIndex;

        [[nodiscard]] bool canShare(BufferBoundary other) const noexcept
        {
            return queue != kGraphInvalidIndex && queue == other.queue && use == other.use;
        }
    };
}
