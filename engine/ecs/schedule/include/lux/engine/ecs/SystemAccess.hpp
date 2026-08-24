#pragma once

#include <lux/engine/ecs/Query.hpp>

#include <lux/cxx/compile_time/TypeToken.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <tuple>
#include <type_traits>

namespace lux::ecs
{
    enum class EAccessMode : std::uint8_t
    {
        READ,
        WRITE,
    };

    struct ComponentAccess final
    {
        lux::cxx::TypeToken type;
        EAccessMode mode{EAccessMode::READ};
    };

    struct ExternalAccess final
    {
        lux::cxx::TypeToken type;
        EAccessMode mode{EAccessMode::READ};
    };

    template <class External>
    struct ExternalRead final
    {
        using external_type = External;
    };

    template <class External>
    struct ExternalWrite final
    {
        using external_type = External;
    };

    struct SystemAccess final
    {
        std::span<const ComponentAccess> components;
        std::span<const ExternalAccess> external;
        bool complete{};
    };

    namespace detail
    {
        template <class Access>
        struct ExternalAccessTraits;

        template <class External>
        struct ExternalAccessTraits<ExternalRead<External>> final
        {
            using ExternalType = External;
            static constexpr EAccessMode kMode = EAccessMode::READ;
        };

        template <class External>
        struct ExternalAccessTraits<ExternalWrite<External>> final
        {
            using ExternalType = External;
            static constexpr EAccessMode kMode = EAccessMode::WRITE;
        };

        template <class Access>
        concept ExternalAccessSpec = requires
        {
            typename ExternalAccessTraits<Access>::ExternalType;
        };

        template <class... Values>
        [[nodiscard]] consteval bool uniqueTypes() noexcept
        {
            return []<std::size_t... Index>(std::index_sequence<Index...>)
            {
                using Types = std::tuple<Values...>;
                return (([]<std::size_t Current, class Tuple>()
                {
                    using Value = std::tuple_element_t<Current, Tuple>;
                    return []<std::size_t... Previous>(
                        std::index_sequence<Previous...>)
                    {
                        return (!std::same_as<
                            Value,
                            std::tuple_element_t<Previous, Tuple>> && ...);
                    }(std::make_index_sequence<Current>{});
                }.template operator()<Index, Types>()) && ...);
            }(std::index_sequence_for<Values...>{});
        }

        template <class Query, class... External>
        struct SystemAccessStorage;

        template <class... Component, class... External>
        struct SystemAccessStorage<QuerySpec<Component...>, External...> final
        {
            static_assert((ComponentAccessSpec<Component> && ...));
            static_assert((ExternalAccessSpec<External> && ...));
            static_assert(uniqueComponents<Component...>());
            static_assert(uniqueTypes<
                typename ExternalAccessTraits<External>::ExternalType...>());

            inline static constexpr std::array<
                ComponentAccess,
                sizeof...(Component)> component{
                ComponentAccess{
                    lux::cxx::typeToken<
                        typename AccessTraits<Component>::ComponentType>(),
                    AccessTraits<Component>::kWrite
                        ? EAccessMode::WRITE
                        : EAccessMode::READ}...};

            inline static constexpr std::array<
                ExternalAccess,
                sizeof...(External)> external{
                ExternalAccess{
                    lux::cxx::typeToken<
                        typename ExternalAccessTraits<External>::ExternalType>(),
                    ExternalAccessTraits<External>::kMode}...};
        };
    } // namespace detail

    template <class... Component, class... External>
        requires (detail::ExternalAccessSpec<External> && ...)
    [[nodiscard]] constexpr SystemAccess access(
        QuerySpec<Component...>,
        External...) noexcept
    {
        using Storage = detail::SystemAccessStorage<
            QuerySpec<Component...>, External...>;
        return SystemAccess{Storage::component, Storage::external, true};
    }
} // namespace lux::ecs
