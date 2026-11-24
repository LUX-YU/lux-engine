#pragma once

#include <lux/engine/render/visibility.h>
#include <lux/engine/render/device/GpuResourceFeatures.hpp>

#include <array>
#include <algorithm>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/resource/ResourceRequirement.hpp>
#include <functional>
#include <lux/engine/render/resource/ResourceIdentity.hpp>
#include <tuple>

namespace lux::render
{
    template <class T> struct RevisionedValue final
    {
        T value;
        std::uint64_t revision;
    };

    template <class T> using ResourceViewValue = RevisionedValue<std::reference_wrapper<const T>>;

    template <class... Inputs>
    [[nodiscard]] std::span<const cxx::basic_type_info> resourceFrameTypes(std::tuple<Inputs...>*) noexcept
    {
        static const std::array<cxx::basic_type_info, sizeof...(Inputs)> types{cxx::make_basic_type_info<Inputs>()...};
        return types;
    }

    // Static metadata. A descriptor is not an instance, a resource budget or a GPU owner.
    // Config/Ops qualification belongs to the typed Factory, not a manual version or encoded config schema.
    struct ResourceDefinition final
    {
        ResourceNameView    type;
        GpuResourceFeatures required_features;
        GpuResourceFeatures optional_features;
        cxx::basic_type_info binding_type;
        std::string_view description;
        EResourceDomain domain = EResourceDomain::DEVICE;
        std::span<const cxx::basic_type_info> frame_inputs;
        EResourceContentLifetime lifetime = EResourceContentLifetime::PERSISTENT;

        [[nodiscard]] bool supports(GpuResourceFeatures available) const noexcept
        {
            return available.containsAll(required_features);
        }

        [[nodiscard]] bool hasSameContract(const ResourceDefinition& other) const noexcept
        {
            const bool same_policy =
                type == other.type &&
                required_features == other.required_features &&
                optional_features == other.optional_features &&
                binding_type == other.binding_type &&
                domain == other.domain && lifetime == other.lifetime;
            return same_policy && std::ranges::equal(frame_inputs, other.frame_inputs);
        }
    };

    [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<void>
    validateStorage(const ResourceDefinition& descriptor) noexcept;

    [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<void>
    compatibleStorage(const ResourceDefinition& expected, const ResourceDefinition& actual) noexcept;

    [[nodiscard]] LUX_RENDER_PUBLIC std::span<const error::ErrorDescriptor>
    renderResourceErrorDescriptors() noexcept;
} // namespace lux::render
