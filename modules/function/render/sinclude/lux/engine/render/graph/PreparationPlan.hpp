#pragma once

#include <lux/engine/render/extensions/execution/LinkedProducer.hpp>
#include <lux/engine/render/extensions/resource/StorageBinding.hpp>
#include <type_traits>
#include <vector>

namespace lux::render::detail
{
    struct InputSlot final
    {
        std::uint32_t index;
    };

    struct CompiledPassSlot final
    {
        std::uint32_t index = kGraphInvalidIndex;
    };

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

        struct EffectInput final
        {
            std::uint32_t source;
            InputSlot destination;
            std::span<const std::byte> (*bind)(const void*) noexcept = nullptr;
        };

        struct ConstantCopy final
        {
            InputSlot input;
            CompiledPassSlot pass;
        };

        static_assert(std::is_standard_layout_v<ConstantCopy> && std::is_trivially_copyable_v<ConstantCopy>);

        std::vector<ConstantCopy> constant_copies;
        std::vector<EffectInput> effect_inputs;
        std::vector<std::size_t> cpu_data_bytes;
        std::vector<Buffer> buffers;
        std::vector<Snapshot> snapshots;
        std::vector<RasterBindings> raster_bindings; // Final pass slot to canonical Job bindings.
        std::vector<LinkedPreparationProducer> producers;
    };
} // namespace lux::render::detail
