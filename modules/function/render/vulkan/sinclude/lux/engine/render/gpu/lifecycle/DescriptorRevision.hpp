#pragma once

#include <lux/engine/function/render/client/core/RenderTypes.hpp>
#include <cstdint>

namespace lux::render
{
    /// Per-FIF descriptor revision tracker.  Bump the revision when the
    /// underlying VkBuffer/VkImage handle changes; at upload time, compare
    /// per-slot written revisions to decide whether to re-write.
    struct DescriptorRevision
    {
        uint64_t revision{0};
        uint64_t written[kMaxFramesInFlight]{};

        void bump() noexcept
        {
            ++revision;
        }

        /// Returns true if slot `fi` has not yet been written at the
        /// current revision.  Automatically marks it written.
        bool needsWrite(uint32_t fi) noexcept
        {
            if (written[fi] != revision)
            {
                written[fi] = revision;
                return true;
            }
            return false;
        }
    };

}
