#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstring>
#include <lux/engine/description/PassContract.hpp>
#include <lux/engine/render/graph/Definition.hpp>
#include <optional>
#include <type_traits>

namespace lux::render
{
    template <typename T> struct PassSchema;

    namespace detail
    {
        struct CapturedParameters
        {
            // Parallel to uses during capture only; never part of Definition/topology identity.
            std::vector<std::uint64_t> authoring_scopes;
            std::vector<GraphResourceUse> uses;
            std::vector<std::byte> scalars;
            std::vector<CapturedFieldBinding> bindings;
            std::optional<RenderError> error;
        };

        template <typename T>
        void captureScalar(CapturedParameters& output, std::uint32_t offset, const T& value) noexcept
        {
            static_assert(std::same_as<T, float> || std::same_as<T, int> || std::same_as<T, unsigned int>);
            std::memcpy(output.scalars.data() + offset, &value, sizeof(T));
        }

        inline CapturedFieldBinding captureField(const rdesc::PassResourceField& field, std::uint32_t element) noexcept
        {
            CapturedFieldBinding result;
            result.path = field.path;
            result.array_element = element;
            result.shader_name = field.shader_name;
            result.array_count = field.array_count;
            result.element_stride = field.element_stride;
            result.descriptor_array = field.descriptor_array;
            result.stages = field.stages;
            result.role = field.role;
            result.owner = field.owner;
            result.frequency = field.frequency;
            result.required = field.required;
            result.semantic = field.semantic;
            result.paired_texture = field.paired_texture;
            result.dimension = field.dimension;
            result.image_format = field.image_format;
            return result;
        }

        [[nodiscard]] EGraphAccess resourceAccess(rdesc::EPassFieldRole role) noexcept;
        [[nodiscard]] EGraphUsage resourceUsage(rdesc::EPassFieldRole role) noexcept;

        template <typename T>
            requires requires(const T& value) {
                { value.texture } -> std::convertible_to<GraphTexture>;
            }
        void captureResource(
            CapturedParameters& output,
            const T& value,
            const rdesc::PassResourceField& field,
            std::uint32_t element
        ) noexcept
        {
            const auto role = field.role;
            const auto id = !value.texture.isValid() && !field.required ? value.fallback : value.texture;
            if (!id.isValid())
            {
                output.error = RenderError{kGraphInvalidResource, {0}};
                return;
            }
            auto binding = captureField(field, element);
            binding.resource = GraphResourceId{id.value()};
            auto access = resourceAccess(role);
            if constexpr (std::same_as<T, Attachment>)
            {
                binding.load = value.load;
                binding.store = value.store;
                std::copy(std::begin(value.clear), std::end(value.clear), binding.clear.begin());
                if (value.load == ELoadOp::LOAD && role != rdesc::EPassFieldRole::RESOLVE)
                {
                    access = EGraphAccess::READ_WRITE;
                }
            }
            if constexpr (std::same_as<T, DepthStencilAttachment>)
            {
                binding.load = value.load;
                binding.store = value.store;
                binding.stencil_load = value.stencil_load;
                binding.stencil_store = value.stencil_store;
                binding.clear_depth = value.clear_depth;
                binding.clear_stencil = value.clear_stencil;
                if (value.load == ELoadOp::LOAD || value.stencil_load == ELoadOp::LOAD)
                {
                    access = EGraphAccess::READ_WRITE;
                }
            }
            binding.resource_kind = EGraphResourceKind::IMAGE;
            output.authoring_scopes.push_back(id.authoringScope());
            binding.image_range = value.range;
            const auto field_index = static_cast<std::uint32_t>(output.bindings.size());
            output.bindings.push_back(std::move(binding));
            output.uses.push_back(
                {GraphResourceId{id.value()},
                 access,
                 resourceUsage(role),
                 value.range,
                 field.stages,
                 0,
                 1,
                 0,
                 field_index}
            );
        }

        template <typename T>
            requires requires(const T& value) {
                { value.buffer } -> std::convertible_to<GraphBuffer>;
            }
        void captureResource(
            CapturedParameters& output,
            const T& value,
            const rdesc::PassResourceField& field,
            std::uint32_t element
        ) noexcept
        {
            const auto role = field.role;
            const auto id = !value.buffer.isValid() && !field.required ? value.fallback : value.buffer;
            if (!id.isValid())
            {
                output.error = RenderError{kGraphInvalidResource, {0}};
                return;
            }
            auto binding = captureField(field, element);
            binding.resource = GraphResourceId{id.value()};
            binding.resource_kind = EGraphResourceKind::BUFFER;
            output.authoring_scopes.push_back(id.authoringScope());
            binding.buffer_range = value.range;
            const auto field_index = static_cast<std::uint32_t>(output.bindings.size());
            output.bindings.push_back(std::move(binding));
            output.uses.push_back(
                {GraphResourceId{id.value()},
                 resourceAccess(role),
                 resourceUsage(role),
                 value.range,
                 field.stages,
                 field.element_stride,
                 field.element_alignment,
                 role == rdesc::EPassFieldRole::UNIFORM_READ ? 0u : field.element_stride,
                 field_index}
            );
        }

        inline void captureResource(
            CapturedParameters& output,
            const SamplerHandle& value,
            const rdesc::PassResourceField& field,
            std::uint32_t element
        ) noexcept
        {
            auto binding = captureField(field, element);
            binding.sampler = value.sampler;
            output.bindings.push_back(std::move(binding));
            if (!value.sampler.isValid())
            {
                output.error = RenderError{kGraphInvalidResource, {0}};
            }
        }
    } // namespace detail

    consteval bool validPassContract(rdesc::PassShaderContract contract)
    {
        if (contract.canonical_name.empty() || contract.parameter_size == 0 || contract.parameter_alignment == 0)
        {
            return false;
        }
        for (std::size_t i = 0; i < contract.resources.size(); ++i)
        {
            const auto& field = contract.resources[i];
            const bool is_invalid_identity = field.path.empty() || field.shader_name.empty();
            const bool is_invalid_shape =
                field.element_alignment == 0 || (field.element_alignment & (field.element_alignment - 1)) != 0 ||
                field.array_count == 0 || (!field.descriptor_array && field.array_count != 1) || field.stages == 0 ||
                (field.stages & ~7u) != 0;
            const bool is_invalid_role = field.role > rdesc::EPassFieldRole::INPUT_ATTACHMENT;
            const bool is_invalid_owner =
                field.owner > rdesc::EFieldOwner::PASS_LOCAL || field.frequency > rdesc::EUpdateFrequency::DRAW;
            if (is_invalid_identity || is_invalid_shape || is_invalid_role || is_invalid_owner)
            {
                return false;
            }
            for (std::size_t j = 0; j < i; ++j)
            {
                if (field.path == contract.resources[j].path || field.shader_name == contract.resources[j].shader_name)
                {
                    return false;
                }
            }
        }
        for (const auto& field : contract.scalars)
        {
            const bool is_invalid_scalar =
                field.path.empty() || field.size != 4 || field.kind > rdesc::EScalarKind::UINT ||
                field.array_count == 0 || field.owner > rdesc::EFieldOwner::PASS_LOCAL ||
                field.frequency > rdesc::EUpdateFrequency::DRAW || field.stages == 0 || (field.stages & ~7u) != 0;
            if (is_invalid_scalar)
            {
                return false;
            }
        }
        return true;
    }

    template <typename T>
    concept GraphPassParameters = std::is_standard_layout_v<T> && requires(const T& value) {
        requires PassSchema<T>::version == 1;
        requires validPassContract(PassSchema<T>::contract());
        { PassSchema<T>::contract() } noexcept -> std::same_as<rdesc::PassShaderContract>;
        { PassSchema<T>::capture(value) } noexcept -> std::same_as<detail::CapturedParameters>;
        requires PassSchema<T>::contract().parameter_size == sizeof(T);
        requires PassSchema<T>::contract().parameter_alignment == alignof(T);
    };
} // namespace lux::render
