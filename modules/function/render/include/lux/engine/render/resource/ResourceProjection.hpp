#pragma once

#include <lux/engine/render/graph/GraphResource.hpp>

namespace lux::render
{
    // Logical shape and boundary state for one GPU projection, independent of its current native backing.
    struct ResourceProjection final
    {
        VGraphResourceDesc description;
        std::vector<VGraphRange> initial_ranges;
        EGraphUse initial_use = EGraphUse::UNSPECIFIED;
        // When set, the graph may only read the projection in this use.
        std::optional<EGraphUse> read_use{};
        // The owner can grant an exclusive writer for bytes within this Buffer projection.
        bool final_buffer_output = false;
        // The owner supplies concurrent native images in their declared read layout and physical queue frontiers.
        bool concurrent_image_read = false;
        // Installation must select this resource from the Job's own scope.
        bool job_local = false;
        [[nodiscard]] static ResourceProjection bufferOutput(std::uint64_t bytes, EGraphUse boundary) noexcept
        {
            return {GraphBufferDesc{bytes}, {}, boundary, {}, true};
        }
        bool operator==(const ResourceProjection&) const noexcept = default;
    };
}
