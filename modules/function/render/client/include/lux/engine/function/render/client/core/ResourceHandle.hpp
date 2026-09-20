#pragma once
/// Generic resource handle building blocks.
#include <lux/cxx/container/SlotMap.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>

namespace lux::render
{
    /**
     * @brief A strongly-typed handle wrapping a uint32_t index.
     *
     * Two handles with different Tag types are distinct types and cannot
     * be implicitly converted or compared.
     */
    template <typename Tag> struct TypedHandle
    {
        uint32_t index{std::numeric_limits<uint32_t>::max()};

        constexpr TypedHandle() noexcept = default;
        constexpr explicit TypedHandle(uint32_t idx) noexcept : index(idx)
        {
        }

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return index != std::numeric_limits<uint32_t>::max();
        }

        constexpr auto operator<=>(const TypedHandle&) const noexcept = default;

        struct Hash
        {
            std::size_t operator()(TypedHandle h) const noexcept
            {
                return std::hash<uint32_t>{}(h.index);
            }
        };
    };

    /**
     * @brief A strongly-typed generational handle alias.
     */
    template <typename Tag> using TypedSlotHandle = lux::cxx::SlotKey<Tag>;

    struct TextureHandleTag {};
    struct ShaderHandleTag {};

    using TextureHandle = lux::cxx::SlotKey<TextureHandleTag>;
    using ShaderHandle = lux::cxx::SlotKey<ShaderHandleTag>;
}
