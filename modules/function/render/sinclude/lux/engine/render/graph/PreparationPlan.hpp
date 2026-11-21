#pragma once

#include <lux/engine/render/extensions/execution/LinkedProducer.hpp>
#include <lux/engine/render/extensions/resource/StorageBinding.hpp>
#include <vector>

namespace lux::render::detail
{
    struct LinkedInlineBlock final
    {
        std::uint32_t pass;
        std::size_t bytes;
    };

    struct LinkedPreparationProducer final
    {
        LinkedProducer prototype;
        std::vector<std::uint32_t> inputs;
        std::vector<std::uint32_t> outputs;
        std::vector<LinkedInlineBlock> inline_blocks;
        std::vector<std::uint32_t> raster_streams;
        std::vector<std::uint32_t> buffers;
        std::vector<std::uint32_t> snapshots;
        // The Effect execution borrow protects this owner-side source through every Job.
        std::span<const std::byte> node_state;
        std::vector<std::uint32_t> program_snapshots;
    };

    struct PreparedBufferCopy final
    {
        std::uint32_t producer, buffer;
        ReplicaCopyRange ranges;
        std::size_t offset, size;
        std::uint64_t revision;
    };

    struct PreparationPlan final
    {
        struct RasterBindings final
        {
            std::uint32_t textures = kGraphInvalidIndex, meshes = kGraphInvalidIndex;
            std::uint32_t materials = kGraphInvalidIndex;
        };

        struct Buffer final
        {
            std::uint32_t resource;
            std::uint64_t minimum_bytes, maximum_bytes;
            bool upload;
            bool used;
            std::uint32_t producer;
        };

        struct Snapshot final
        {
            cxx::basic_type_info type;
            std::uint32_t binding;
        };

        std::vector<std::size_t> cpu_data_bytes;
        std::vector<Buffer> buffers;
        std::vector<Snapshot> snapshots;
        std::vector<RasterBindings> raster_bindings; // Final pass slot to canonical Job bindings.
        std::vector<LinkedPreparationProducer> producers;
    };
} // namespace lux::render::detail
