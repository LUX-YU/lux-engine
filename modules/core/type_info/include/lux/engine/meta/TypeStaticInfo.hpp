#pragma once

#include <string_view>
#include <tuple>
#include <type_traits>

namespace lux::meta
{
    template <auto Member> struct TTypeStaticField final
    {
        static constexpr auto pointer = Member;
        std::string_view name;
    };

    template <auto Member>
    [[nodiscard]] consteval TTypeStaticField<Member> typeStaticField(std::string_view name) noexcept
    {
        return {name};
    }

    template <class T> struct TTypeStaticInfo
    {
        static constexpr bool available = false;
    };

    template <class T>
    concept HasTypeStaticInfo = TTypeStaticInfo<std::remove_cvref_t<T>>::available;
} // namespace lux::meta
