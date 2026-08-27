#pragma once

#include <uuid.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace lux::asset
{
    class AssetId final
    {
    public:
        AssetId() = default;

        explicit AssetId(uuids::uuid value) noexcept : value_(value)
        {
        }

        explicit AssetId(const std::array<std::uint8_t, 16>& bytes) noexcept : value_(bytes)
        {
        }

        [[nodiscard]] bool isNull() const noexcept
        {
            return value_.is_nil();
        }

        [[nodiscard]] auto bytes() const noexcept
        {
            return value_.as_bytes();
        }

        [[nodiscard]] const uuids::uuid& uuid() const noexcept
        {
            return value_;
        }

        friend bool operator==(const AssetId&, const AssetId&) noexcept = default;

        friend bool operator<(const AssetId& left, const AssetId& right) noexcept
        {
            return left.value_ < right.value_;
        }

    private:
        uuids::uuid value_{};
    };

    inline const AssetId NullAssetId{};
} // namespace lux::asset

template <> struct std::hash<lux::asset::AssetId>
{
    std::size_t operator()(const lux::asset::AssetId& value) const noexcept
    {
        return std::hash<uuids::uuid>{}(value.uuid());
    }
};
