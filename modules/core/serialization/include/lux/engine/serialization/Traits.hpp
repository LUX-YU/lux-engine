#pragma once

#include <lux/engine/meta/TypeStaticInfo.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace lux::serialization
{
    inline constexpr std::size_t DynamicWireSize = static_cast<std::size_t>(-1);

    template <class T, class = void>
    struct FixedWireSize
    {
        static constexpr std::size_t value = DynamicWireSize;
    };

    template <class T>
        requires(std::is_arithmetic_v<T> || std::is_enum_v<T>)
    struct FixedWireSize<T>
    {
        static constexpr std::size_t value = sizeof(T);
    };

    template <class T, std::size_t Size>
    struct FixedWireSize<std::array<T, Size>>
    {
        static constexpr std::size_t element = FixedWireSize<T>::value;
        static constexpr std::size_t value =
            element == DynamicWireSize ? DynamicWireSize : element * Size;
    };

    template <class First, class Second>
    struct FixedWireSize<std::pair<First, Second>>
    {
        static constexpr std::size_t first = FixedWireSize<First>::value;
        static constexpr std::size_t second = FixedWireSize<Second>::value;
        static constexpr std::size_t value =
            first == DynamicWireSize || second == DynamicWireSize
                ? DynamicWireSize
                : first + second;
    };

    namespace detail
    {
        template <class T>
        consteval std::size_t typeStaticFixedWireSize()
        {
            std::size_t total{};
            bool dynamic{};
            std::apply(
                [&](const auto&... field)
                {
                    const auto include = [&](const auto& descriptor)
                    {
                        using Member = std::remove_cvref_t<decltype(
                            std::declval<T>().*descriptor.pointer
                        )>;
                        constexpr std::size_t size = FixedWireSize<Member>::value;
                        if constexpr (size == DynamicWireSize)
                        {
                            dynamic = true;
                        }
                        else
                        {
                            total += size;
                        }
                    };
                    (include(field), ...);
                },
                lux::meta::TypeStaticInfo<T>::fields
            );
            return dynamic ? DynamicWireSize : total;
        }
    } // namespace detail

    template <class T>
        requires lux::meta::HasTypeStaticInfo<T>
    struct FixedWireSize<T>
    {
        static constexpr std::size_t value =
            detail::typeStaticFixedWireSize<T>();
    };

    template <class T>
    inline constexpr std::size_t FixedWireSizeV =
        FixedWireSize<std::remove_cvref_t<T>>::value;
} // namespace lux::serialization
