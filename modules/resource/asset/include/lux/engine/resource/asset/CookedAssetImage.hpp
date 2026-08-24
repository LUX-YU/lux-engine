#pragma once

#include <lux/engine/resource/asset/AssetId.hpp>
#include <lux/engine/resource/asset/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace lux::asset
{
    inline constexpr std::uint32_t kCookedAssetVersionV1 = 20250128u;
    inline constexpr std::uint32_t kCookedAssetVersionV2 = 20260606u;

    enum class ECookedAssetImageError : std::uint8_t
    {
        TRUNCATED,
        UNSUPPORTED_VERSION,
        INVALID_LAYOUT,
        LIMIT_EXCEEDED,
    };

    struct CookedAssetMetadata final
    {
        AssetId id;
        std::uint32_t legacy_type_tag{};
        std::uint64_t date{};
        std::array<char, 64> display_name{};
        std::array<char, 256> source_path{};
        std::uint64_t source_mtime{};
    };

    struct CookedAssetImageView final
    {
        std::uint32_t magic{};
        std::uint32_t version{};
        CookedAssetMetadata metadata;
        std::span<const std::byte> info;
        std::span<const std::byte> data;
        std::span<const std::byte> auxiliary_payloads;
    };

    [[nodiscard]] LUX_ASSET_PUBLIC lux::cxx::expected<
        CookedAssetImageView,
        ECookedAssetImageError>
    inspectCookedAssetImage(std::span<const std::byte> image) noexcept;
} // namespace lux::asset
