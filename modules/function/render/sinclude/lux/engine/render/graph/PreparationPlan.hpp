#pragma once

#include <lux/engine/render/extensions/execution/GraphPreparation.hpp>
#include <lux/engine/render/resource/RasterProgramBuild.hpp>
#include <lux/engine/render/extensions/resource/StorageBinding.hpp>
#include <type_traits>
#include <vector>

namespace lux::render::detail
{
    struct BoundDrawResource final
    {
        ResourceInstanceId id;
        const void* storage = nullptr;
        cxx::basic_type_info implementation;
        std::uint32_t capabilities = 0;
        bool indexed_geometry = false;
    };

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
        std::uint32_t alternate = kGraphInvalidIndex;
    };

    struct PreparationRange final
    {
        std::uint32_t first = 0, count = 0;

        [[nodiscard]] bool contains(std::uint32_t slot) const noexcept
        {
            return slot >= first && slot - first < count;
        }
    };

    struct LinkedPreparationProducer final
    {
        extension_detail::PreparationCall call;
        RasterProgramBuilder::Build build_programs = nullptr;
        PreparationRange inputs, outputs, inline_blocks;
        std::vector<std::uint32_t> raster_streams;
        std::vector<std::uint32_t> buffers;
        std::vector<std::uint32_t> snapshots;
        std::vector<std::uint32_t> program_snapshots;

        [[nodiscard]] bool linkPrograms(const RasterProgramBuilder& builder) noexcept
        {
            build_programs = builder.link(call.inputs.type(), call.methods->mapping_type);
            return !builder || build_programs;
        }
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
        std::vector<InputSlot> cpu_reads;
        std::vector<LinkedInlineBlock> inline_blocks;
        std::vector<Buffer> buffers;
        std::vector<Snapshot> snapshots;
        struct DrawSourceRange final
        {
            std::uint32_t first = 0, count = 0;
            std::uint32_t producer = kGraphInvalidIndex, local_stream = kGraphInvalidIndex;
        };

        std::vector<DrawSourceRange> draw_resource_ranges;
        struct DrawBinding final
        {
            std::uint32_t binding;
            std::uint32_t capabilities;
            bool indexed_geometry;
        };

        std::vector<DrawBinding> draw_resource_bindings;
        std::vector<LinkedPreparationProducer> producers;
    };
} // namespace lux::render::detail
