#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

namespace lux::asset
{
    class AssetTypeId final
    {
    public:
        constexpr AssetTypeId() = default;

        explicit constexpr AssetTypeId(std::uint64_t value) noexcept : value_(value)
        {
        }

        [[nodiscard]] static constexpr AssetTypeId fromName(std::string_view name) noexcept
        {
            std::uint64_t hash = 14695981039346656037ull;
            for (const unsigned char value : name)
            {
                hash ^= value;
                hash *= 1099511628211ull;
            }
            return AssetTypeId{hash};
        }

        [[nodiscard]] constexpr std::uint64_t value() const noexcept
        {
            return value_;
        }

        [[nodiscard]] constexpr explicit operator bool() const noexcept
        {
            return value_ != 0u;
        }

        friend constexpr bool operator==(AssetTypeId, AssetTypeId) noexcept = default;

        friend constexpr bool operator<(AssetTypeId left, AssetTypeId right) noexcept
        {
            return left.value_ < right.value_;
        }

    private:
        std::uint64_t value_{};
    };
} // namespace lux::asset

template <> struct std::hash<lux::asset::AssetTypeId>
{
    std::size_t operator()(lux::asset::AssetTypeId value) const noexcept
    {
        return static_cast<std::size_t>(value.value());
    }
};
