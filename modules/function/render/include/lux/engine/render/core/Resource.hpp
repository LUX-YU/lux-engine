#pragma once

#include <lux/cxx/core/Schema.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/cxx/compile_time/type_info.hpp>
#include <array>
#include <functional>
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
} // namespace lux::render
