#pragma once

#include <cstddef>
#include <span>
#include <lux/engine/render/extensions/graph/GraphResource.hpp>
#include <lux/engine/render/graph/ShaderInterface.hpp>

namespace lux::render::detail
{
    struct DescriptorUse final
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

        bool operator==(const DescriptorUse&) const noexcept = default;
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
        std::span<const DescriptorUse> uses;
        std::span<const std::byte> initial_parameters;
        bool has_private_set;
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

        std::vector<FixedPassBindings> passes;
        std::vector<FixedDescriptorBinding> descriptor_fields;
        std::vector<DescriptorUse> descriptor_uses;
        std::vector<std::byte> initial_parameters;
        nri::DescriptorPoolDesc private_pool{};
    };
} // namespace lux::render::detail
