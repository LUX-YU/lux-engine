#pragma once

#include <lux/engine/meta/TypeStaticInfo.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <tuple>
#include <type_traits>
#include <utility>

namespace lux::serialization
{
    enum class EWireExtent : std::uint8_t
    {
        TAG,
        FIXED,
        VARIABLE,
    };

    template <class T>
    struct Serializer;

    template <class T>
    concept HasSerializerDefinition = requires
    {
        sizeof(Serializer<std::remove_cvref_t<T>>);
    };

    namespace detail
    {
        struct WireShape final
        {
            EWireExtent extent{EWireExtent::VARIABLE};
            std::size_t fixed_size{};
        };

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
        consteval WireShape wireShape();

        [[nodiscard]] consteval WireShape combine(
            WireShape left,
            WireShape right
        ) noexcept
        {
            if (left.extent == EWireExtent::VARIABLE ||
                right.extent == EWireExtent::VARIABLE)
            {
                return {EWireExtent::VARIABLE, 0U};
            }
            if (left.fixed_size >
                std::numeric_limits<std::size_t>::max() - right.fixed_size)
            {
                return {EWireExtent::VARIABLE, 0U};
            }
            const std::size_t size = left.fixed_size + right.fixed_size;
            return {
                size == 0U ? EWireExtent::TAG : EWireExtent::FIXED,
                size
            };
        }

        [[nodiscard]] consteval WireShape repeat(
            WireShape value,
            std::size_t count
        ) noexcept
        {
            if (value.extent == EWireExtent::VARIABLE)
                return value;
            if (count != 0U && value.fixed_size >
                std::numeric_limits<std::size_t>::max() / count)
            {
                return {EWireExtent::VARIABLE, 0U};
            }
            const std::size_t size = value.fixed_size * count;
            return {
                size == 0U ? EWireExtent::TAG : EWireExtent::FIXED,
                size
            };
        }

        template <class T, std::size_t Index = 0U>
        consteval WireShape typeStaticWireShape()
        {
            using Fields = std::remove_cvref_t<decltype(
                lux::meta::TypeStaticInfo<T>::fields
            )>;
            if constexpr (Index == std::tuple_size_v<Fields>)
            {
                return {EWireExtent::TAG, 0U};
            }
            else
            {
                using Descriptor = std::tuple_element_t<Index, Fields>;
                using Member = std::remove_cvref_t<decltype(
                    std::declval<T>().*Descriptor::pointer
                )>;
                return combine(
                    wireShape<Member>(),
                    typeStaticWireShape<T, Index + 1U>()
                );
            }
        }

        template <class Tuple, std::size_t Index = 0U>
        consteval WireShape tupleWireShape()
        {
            if constexpr (Index == std::tuple_size_v<Tuple>)
            {
                return {EWireExtent::TAG, 0U};
            }
            else
            {
                return combine(
                    wireShape<std::tuple_element_t<Index, Tuple>>(),
                    tupleWireShape<Tuple, Index + 1U>()
                );
            }
        }

        template <class T>
        consteval WireShape wireShape()
        {
            using U = std::remove_cvref_t<T>;
            if constexpr (HasSerializerDefinition<U>)
            {
                if constexpr (requires { Serializer<U>::wire_extent; })
                {
                    constexpr auto extent = Serializer<U>::wire_extent;
                    if constexpr (extent == EWireExtent::TAG)
                        return {extent, 0U};
                    else if constexpr (extent == EWireExtent::FIXED)
                    {
                        static_assert(requires {
                            Serializer<U>::fixed_wire_size;
                        });
                        static_assert(Serializer<U>::fixed_wire_size != 0U);
                        return {extent, Serializer<U>::fixed_wire_size};
                    }
                    else
                        return {EWireExtent::VARIABLE, 0U};
                }
                else
                    return {EWireExtent::VARIABLE, 0U};
            }
            else if constexpr (std::same_as<U, bool>)
                return {EWireExtent::FIXED, 1U};
            else if constexpr (std::is_arithmetic_v<U>)
                return {EWireExtent::FIXED, sizeof(U)};
            else if constexpr (std::is_enum_v<U>)
                return wireShape<std::underlying_type_t<U>>();
            else if constexpr (WireIsArray<U>::value)
                return repeat(
                    wireShape<typename U::value_type>(),
                    std::tuple_size_v<U>
                );
            else if constexpr (WireIsPair<U>::value)
                return combine(
                    wireShape<typename U::first_type>(),
                    wireShape<typename U::second_type>()
                );
            else if constexpr (lux::meta::HasTypeStaticInfo<U>)
                return typeStaticWireShape<U>();
            else if constexpr (WireIsTupleLike<U>::value)
                return tupleWireShape<U>();
            else
                return {EWireExtent::VARIABLE, 0U};
        }
    }

    template <class T>
    struct WireTraits
    {
      private:
        inline static constexpr auto kShape = detail::wireShape<T>();

      public:
        inline static constexpr EWireExtent extent = kShape.extent;
        inline static constexpr std::size_t fixed_size = kShape.fixed_size;
    };
}
