#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <vector>
#include <lux/engine/render/extensions/graph/GraphResource.hpp>
#include <lux/engine/render/graph/ShaderInterface.hpp>

namespace lux::render::detail
{
    struct DescriptorAccess final
    {
        std::uint32_t resource;
        std::uint32_t set;
        std::uint32_t binding;
        EDescriptorKind kind;
        VGraphRange range;
        std::uint32_t field = 0;
        std::uint32_t element = 0;
        std::uint32_t sampler = kGraphInvalidIndex;
        bool whole_resource = false;
        std::uint32_t fallback = kGraphInvalidIndex;
        bool comparison_sampler = false;

        bool operator==(const DescriptorAccess&) const noexcept = default;
    };

    struct FixedDescriptorBinding final
    {
        nri::BufferView buffer_type;
        nri::Format buffer_format;
        std::uint32_t buffer_stride;
        bool image_arrayed;
        bool private_binding;

        [[nodiscard]] nri::BufferViewDesc
        bufferView(const nri::Buffer* buffer, std::uint64_t offset, std::uint64_t size) const noexcept
        {
            return {buffer, buffer_type, offset, size, buffer_format, buffer_stride};
        }
    };

    struct FixedBindingRange final
    {
        std::size_t first, count;
    };

    struct FixedPassBindings final
    {
        FixedBindingRange bindings, uses, initial_parameters;
        bool has_private_set;
    };

    struct FixedPassBindingView final
    {
        std::span<const FixedDescriptorBinding> bindings;
        std::span<const DescriptorAccess> uses;
        std::span<const std::byte> initial_parameters;
        bool has_private_set;
    };

    struct RecordingDependencies final
    {
        struct RasterList final
        {
            std::uint32_t batch, list;
        };

        struct ResourceIndex final
        {
            [[nodiscard]] std::span<const std::uint32_t> operator[](std::uint32_t resource) const noexcept
            {
                const auto first = offsets[resource];
                return std::span(slots).subspan(first, offsets[resource + 1] - first);
            }

            std::vector<std::size_t> offsets;
            std::vector<std::uint32_t> slots;
        };

        ResourceIndex resource_passes, resource_batches;
        std::vector<std::uint32_t> viewport_passes, dynamic_passes, producer_passes;
        std::array<std::vector<std::uint32_t>, 3> shared_batches;
        std::vector<RasterList> raster_lists;
    };

    struct GraphBindingPlan final
    {
        [[nodiscard]] FixedPassBindingView pass(std::size_t slot) const noexcept
        {
            const auto& entry = passes[slot];
            const auto slice = [](const auto& values, FixedBindingRange range) noexcept
            {
                return std::span(values).subspan(range.first, range.count);
            };
            return {
                slice(descriptor_fields, entry.bindings),
                slice(descriptor_uses, entry.uses),
                slice(initial_parameters, entry.initial_parameters),
                entry.has_private_set
            };
        }

        RecordingDependencies recordings;
        std::vector<FixedPassBindings> passes;
        std::vector<FixedDescriptorBinding> descriptor_fields;
        std::vector<DescriptorAccess> descriptor_uses;
        std::vector<std::byte> initial_parameters;
        nri::DescriptorPoolDesc private_pool{};
    };
} // namespace lux::render::detail
