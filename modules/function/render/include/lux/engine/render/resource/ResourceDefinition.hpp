#pragma once

#include <lux/engine/render/visibility.h>

#include <array>
#include <algorithm>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/cxx/core/Schema.hpp>
#include <functional>
#include <lux/engine/render/resource/ResourceIdentity.hpp>
#include <tuple>

namespace lux::render
{
    struct ResourceNameTag;
    using ResourceName = cxx::SchemaId<ResourceNameTag>;
    using ResourceNameView = cxx::SchemaIdView<ResourceNameTag>;

    enum class ESnapshotScope : std::uint8_t
    {
        ANY_ON_DEVICE,
        JOB_LOCAL
    };

    template <class T> struct RevisionedValue final
    {
        T value;
        std::uint64_t revision;
    };

    template <class T> using SnapshotValue = RevisionedValue<std::reference_wrapper<const T>>;
    template <class... T> using SnapshotValues = std::tuple<SnapshotValue<T>...>;

    namespace extension_detail
    {
        template <class... T>
        inline constexpr std::array<cxx::basic_type_info, sizeof...(T)> snapshot_types{
            cxx::make_basic_type_info<T>()...
        };

        // Called by the registered adapter after its input signature and Resource bindings have been qualified.
        template <class... T, class Select, std::size_t... I>
        SnapshotValues<T...> snapshotValues(Select select, std::index_sequence<I...>) noexcept
        {
            const auto typed = []<class Value>(RevisionedValue<const void*> selected) noexcept
            {
                return RevisionedValue{std::cref(*static_cast<const Value*>(selected.value)), selected.revision};
            };
            return {typed.template operator()<T>(select(I))...};
        }
    }

    enum class EStorageCapability : std::uint32_t
    {
        NONE = 0,
        SAMPLED_IMAGE_ARRAY = 1,
        BLOCK_COMPRESSION_BC = 2,
        STORAGE_BUFFER_ARRAY = 4
    };

    enum class EResourceDomain : std::uint8_t
    {
        DEVICE,
        SCENE,
        VIEW
    };
    template <class... Inputs>
    [[nodiscard]] std::span<const cxx::basic_type_info> resourceFrameTypes(std::tuple<Inputs...>*) noexcept
    {
        static const std::array<cxx::basic_type_info, sizeof...(Inputs)> types{cxx::make_basic_type_info<Inputs>()...};
        return types;
    }

    struct ResourceRequirement final
    {
        ResourceName type;
        // GPU image imports can require a concurrent read-only backing in the declared boundary layout.
        bool concurrent_image_read = false;
        bool operator==(const ResourceRequirement&) const noexcept = default;
    };

    // Static metadata. A descriptor is not an instance, a resource budget or a GPU owner.
    // Config/Ops qualification belongs to the typed Factory, not a manual version or encoded config schema.
    struct ResourceDefinition final
    {
        ResourceNameView    type;
        std::uint32_t       required_capabilities = 0;
        std::uint32_t       optional_capabilities = 0;
        cxx::basic_type_info binding_type;
        std::string_view description;
        EResourceDomain domain = EResourceDomain::DEVICE;
        std::span<const cxx::basic_type_info> frame_inputs;

        [[nodiscard]] bool supports(std::uint32_t capabilities) const noexcept
        {
            return (required_capabilities & ~capabilities) == 0;
        }

        [[nodiscard]] bool hasSameContract(const ResourceDefinition& other) const noexcept
        {
            const bool same_policy =
                type == other.type &&
                required_capabilities == other.required_capabilities &&
                optional_capabilities == other.optional_capabilities &&
                binding_type == other.binding_type &&
                domain == other.domain;
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
