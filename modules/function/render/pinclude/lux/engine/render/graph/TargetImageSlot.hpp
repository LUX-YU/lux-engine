#pragma once

#include <NRI.h>

#include <lux/engine/render/graph/Ids.hpp>
#include <lux/engine/render/runtime/Target.hpp>
#include <array>
#include <string>

namespace lux::render::detail
{
    // Each entry names a local batch on a distinct physical queue. Aliased queue roles share an entry.
    struct QueueBatchSlots final
    {
        std::array<std::uint32_t, 3> batches{kGraphInvalidIndex, kGraphInvalidIndex, kGraphInvalidIndex};
    };

    struct TargetImageSlot final
    {
        std::string binding;
        rdesc::ETextureFormat format;
        std::uint32_t samples;
        GraphResourceId resource;
        struct QueueRead final
        {
            std::uint32_t first_batch = kGraphInvalidIndex;
            nri::StageBits stages = nri::StageBits::NONE;
        };
        std::array<QueueRead, 3> first_reads{};
    };
} // namespace lux::render::detail
