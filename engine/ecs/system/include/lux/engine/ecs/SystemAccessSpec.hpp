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
    enum class ESystemAccessMode : std::uint8_t
    {
        READ,
        WRITE
    };

    struct SystemComponentAccess final
    {
        lux::cxx::TypeToken type;
        std::uint64_t storage{};
        ESystemAccessMode mode{ESystemAccessMode::READ};
    };

    struct SystemExternalAccess final
    {
        lux::cxx::TypeToken type;
        ESystemAccessMode mode{ESystemAccessMode::READ};
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

    struct SystemAccessSpec final
    {
        std::span<const SystemComponentAccess> components;
        std::span<const SystemExternalAccess> external;
    };

    namespace detail
    {
        template <class Access>
        struct ExternalSystemAccessTraits;

        template <class External>
        struct ExternalSystemAccessTraits<ExternalRead<External>> final
        {
            using ExternalType = External;
            static constexpr ESystemAccessMode kMode = ESystemAccessMode::READ;
        };

        template <class External>
        struct ExternalSystemAccessTraits<ExternalWrite<External>> final
        {
            using ExternalType = External;
            static constexpr ESystemAccessMode kMode = ESystemAccessMode::WRITE;
        };

        template <class Access>
        concept ExternalSystemAccess = requires
        {
            typename ExternalSystemAccessTraits<Access>::ExternalType;
        };

        template <class... Values>
        [[nodiscard]] consteval bool uniqueSystemTypes() noexcept
        {
            return []<std::size_t... Index>(std::index_sequence<Index...>)
            {
                using Types = std::tuple<Values...>;
                return (([]<std::size_t Current, class TypeTuple>()
                {
                    using Value = std::tuple_element_t<Current, TypeTuple>;
                    return []<std::size_t... Previous>(
                        std::index_sequence<Previous...>
                    )
                    {
                        return (!std::same_as<
                            Value,
                            std::tuple_element_t<Previous, TypeTuple>> && ...);
                    }(std::make_index_sequence<Current>{});
                }.template operator()<Index, Types>()) && ...);
            }(std::index_sequence_for<Values...>{});
        }

        template <class ComponentTuple, class ExternalTuple>
        struct StaticSystemAccessStorage;

        template <class... Component, class... External>
        struct StaticSystemAccessStorage<
            std::tuple<Component...>,
            std::tuple<External...>
        > final
        {
            static_assert((ComponentAccessSpec<Component> && ...));
            static_assert((ExternalSystemAccess<External> && ...));
            static_assert(uniqueComponents<Component...>());
            static_assert(uniqueSystemTypes<
                typename ExternalSystemAccessTraits<External>::ExternalType...
            >());

            inline static constexpr std::array<
                SystemComponentAccess,
                sizeof...(Component)
            > components{
                SystemComponentAccess{
                    lux::cxx::typeToken<
                        typename AccessTraits<Component>::ComponentType>(),
                    entt::type_hash<
                        typename AccessTraits<Component>::ComponentType>::value(),
                    AccessTraits<Component>::kWrite
                        ? ESystemAccessMode::WRITE
                        : ESystemAccessMode::READ
                }...
            };

            inline static constexpr std::array<
                SystemExternalAccess,
                sizeof...(External)
            > external{
                SystemExternalAccess{
                    lux::cxx::typeToken<
                        typename ExternalSystemAccessTraits<External>::ExternalType>(),
                    ExternalSystemAccessTraits<External>::kMode
                }...
            };
        };

        template <class Access>
        inline constexpr bool kSystemAccessElement =
            ComponentAccessSpec<Access> || ExternalSystemAccess<Access>;

        template <class... Access>
        inline constexpr std::size_t kComponentAccessCount =
            (std::size_t{ComponentAccessSpec<Access>} + ... + 0U);

        template <class... Access>
        inline constexpr std::size_t kExternalAccessCount =
            (std::size_t{ExternalSystemAccess<Access>} + ... + 0U);

        template <class... Access>
        [[nodiscard]] consteval auto systemComponentAccesses() noexcept
        {
            std::array<
                SystemComponentAccess,
                kComponentAccessCount<Access...>
            > result{};
            std::size_t index{};
            ([&]
            {
                if constexpr (ComponentAccessSpec<Access>)
                {
                    using Component = typename AccessTraits<Access>::ComponentType;
                    result[index++] = SystemComponentAccess{
                        lux::cxx::typeToken<Component>(),
                        entt::type_hash<Component>::value(),
                        AccessTraits<Access>::kWrite
                            ? ESystemAccessMode::WRITE
                            : ESystemAccessMode::READ
                    };
                }
            }(), ...);
            return result;
        }

        template <class... Access>
        [[nodiscard]] consteval auto systemExternalAccesses() noexcept
        {
            std::array<
                SystemExternalAccess,
                kExternalAccessCount<Access...>
            > result{};
            std::size_t index{};
            ([&]
            {
                if constexpr (ExternalSystemAccess<Access>)
                {
                    using External = typename
                        ExternalSystemAccessTraits<Access>::ExternalType;
                    result[index++] = SystemExternalAccess{
                        lux::cxx::typeToken<External>(),
                        ExternalSystemAccessTraits<Access>::kMode
                    };
                }
            }(), ...);
            return result;
        }

        template <class Value, std::size_t Size>
        [[nodiscard]] consteval bool uniqueSystemAccesses(
            const std::array<Value, Size>& values
        ) noexcept
        {
            for (std::size_t current = 0U; current < values.size(); ++current)
            {
                for (std::size_t previous = 0U; previous < current; ++previous)
                {
                    if (values[current].type == values[previous].type)
                        return false;
                }
            }
            return true;
        }
    }

    template <class... Component, class... External>
        requires (detail::ExternalSystemAccess<External> && ...)
    [[nodiscard]] consteval SystemAccessSpec makeSystemAccessSpec(
        QuerySpec<Component...>,
        External...
    ) noexcept
    {
        using Storage = detail::StaticSystemAccessStorage<
            std::tuple<Component...>,
            std::tuple<External...>
        >;
        return SystemAccessSpec{Storage::components, Storage::external};
    }

    template <class... AccessValue>
        requires (detail::kSystemAccessElement<AccessValue> && ...)
    struct StaticSystemAccess
    {
    private:
        inline static constexpr auto kComponents =
            detail::systemComponentAccesses<AccessValue...>();
        inline static constexpr auto kExternal =
            detail::systemExternalAccesses<AccessValue...>();

    public:
        static_assert(detail::uniqueSystemAccesses(kComponents));
        static_assert(detail::uniqueSystemAccesses(kExternal));

        inline static constexpr SystemAccessSpec Access{
            kComponents,
            kExternal
        };
    };
}
