#pragma once

#include <lux/engine/meta/TypeStaticInfo.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <limits>
#include <tuple>
#include <type_traits>
#include <utility>

namespace lux::serialization
{
    inline constexpr std::size_t DynamicWireSize =
        std::numeric_limits<std::size_t>::max();

    template <class T>
    struct Serializer;

    template <class T>
    concept HasSerializerDefinition = requires
    {
        sizeof(Serializer<std::remove_cvref_t<T>>);
    };

    namespace detail
    {
        template <class T>
        struct WireIsArray : std::false_type {};

        template <class T, std::size_t Size>
        struct WireIsArray<std::array<T, Size>> : std::true_type {};

        template <class T>
        struct WireIsPair : std::false_type {};

        template <class First, class Second>
        struct WireIsPair<std::pair<First, Second>> : std::true_type {};

        template <class T, class = void>
        struct WireIsTupleLike : std::false_type {};

        template <class T>
        struct WireIsTupleLike<
            T,
            std::void_t<decltype(std::tuple_size<T>::value)>>
            : std::true_type {};

        template <class T>
        consteval std::size_t wireSize();

        [[nodiscard]] constexpr std::size_t addWireSize(
            std::size_t left,
            std::size_t right
        )
        {
            return left == DynamicWireSize || right == DynamicWireSize ||
                    left > DynamicWireSize - right
                ? DynamicWireSize
                : left + right;
        }

        [[nodiscard]] constexpr std::size_t multiplyWireSize(
            std::size_t value,
            std::size_t count
        )
        {
            return value == DynamicWireSize ||
                    (count != 0U && value > DynamicWireSize / count)
                ? DynamicWireSize
                : value * count;
        }

        template <class T, std::size_t Index = 0U>
        consteval std::size_t typeStaticWireSize()
        {
            using Fields = std::remove_cvref_t<decltype(
                lux::meta::TypeStaticInfo<T>::fields
            )>;
            if constexpr (Index == std::tuple_size_v<Fields>)
            {
                return 0U;
            }
            else
            {
                using Descriptor = std::tuple_element_t<Index, Fields>;
                using Member = std::remove_cvref_t<decltype(
                    std::declval<T>().*Descriptor::pointer
                )>;
                return addWireSize(
                    wireSize<Member>(),
                    typeStaticWireSize<T, Index + 1U>()
                );
            }
        }

        template <class Tuple, std::size_t Index = 0U>
        consteval std::size_t tupleWireSize()
        {
            if constexpr (Index == std::tuple_size_v<Tuple>)
            {
                return 0U;
            }
            else
            {
                return addWireSize(
                    wireSize<std::tuple_element_t<Index, Tuple>>(),
                    tupleWireSize<Tuple, Index + 1U>()
                );
            }
        }

        template <class T>
        consteval std::size_t wireSize()
        {
            using U = std::remove_cvref_t<T>;
            if constexpr (HasSerializerDefinition<U>)
            {
                if constexpr (requires { Serializer<U>::fixed_wire_size; })
                    return Serializer<U>::fixed_wire_size;
                else
                    return DynamicWireSize;
            }
            else if constexpr (std::same_as<U, bool>)
            {
                return 1U;
            }
            else if constexpr (std::is_arithmetic_v<U>)
            {
                return sizeof(U);
            }
            else if constexpr (std::is_enum_v<U>)
            {
                return wireSize<std::underlying_type_t<U>>();
            }
            else if constexpr (WireIsArray<U>::value)
            {
                return multiplyWireSize(
                    wireSize<typename U::value_type>(),
                    std::tuple_size_v<U>
                );
            }
            else if constexpr (WireIsPair<U>::value)
            {
                return addWireSize(
                    wireSize<typename U::first_type>(),
                    wireSize<typename U::second_type>()
                );
            }
            else if constexpr (lux::meta::HasTypeStaticInfo<U>)
            {
                return typeStaticWireSize<U>();
            }
            else if constexpr (WireIsTupleLike<U>::value)
            {
                return tupleWireSize<U>();
            }
            else
            {
                return DynamicWireSize;
            }
        }
    } // namespace detail

    template <class T>
    struct WireTraits
    {
        static constexpr std::size_t fixed_size = detail::wireSize<T>();
    };

    template <class T, std::size_t Size>
    struct WireTraits<std::array<T, Size>>
    {
        static constexpr std::size_t fixed_size =
            detail::multiplyWireSize(WireTraits<T>::fixed_size, Size);
    };

    template <class First, class Second>
    struct WireTraits<std::pair<First, Second>>
    {
        static constexpr std::size_t fixed_size = detail::addWireSize(
            WireTraits<First>::fixed_size,
            WireTraits<Second>::fixed_size
        );
    };

    template <class T>
    inline constexpr std::size_t WireSizeV =
        WireTraits<std::remove_cvref_t<T>>::fixed_size;
} // namespace lux::serialization
