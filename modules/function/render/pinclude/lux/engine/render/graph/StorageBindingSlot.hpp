#pragma once

#include <lux/engine/render/extensions/detail/BufferBoundary.hpp>

namespace lux::render::detail
{
    struct StorageBindingSlot final
    {
        GraphResourceId resource;
        std::optional<GraphBufferRange> output_range;
        std::uint32_t first_use = 0;
        std::uint32_t use_count = 0;
        BufferBoundary boundary;
        std::uint32_t first_export = 0;
        std::uint32_t export_count = 0;
    };

    struct FinalBufferUse final
    {
        std::uint32_t pass;
        GraphBufferRange bytes;
        bool write;
    };

    struct FinalBufferExport final
    {
        GraphBufferRange bytes;
        std::optional<PassCondition> when;
    };
}
