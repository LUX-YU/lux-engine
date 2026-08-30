#pragma once

#include <lux/engine/resource/asset/Asset.hpp>
#include <lux/engine/resource/asset/AssetSerDeser.hpp>
#include <lux/engine/resource/asset/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/memory/SharedBytes.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace lux::asset
{
    namespace detail
    {
        struct CookedAssetImageAccess;
    }

    inline constexpr std::uint32_t kCookedAssetVersionV1 = 20250128U;
    inline constexpr std::uint32_t kCookedAssetVersionV2 = 20260606U;

    struct CookedAssetMetadata final
    {
        AssetId id;
        std::uint32_t legacy_type_tag{kNoLegacyAssetTypeTag};
        std::uint64_t date{};
        std::array<char, 64U> display_name{};
        std::array<char, 256U> source_path{};
        std::uint64_t source_mtime{};
    };

    class CookedAssetImage;

    [[nodiscard]] LUX_ASSET_PUBLIC lux::cxx::expected<CookedAssetImage, AssetDecodeFailure>
    inspectCookedAssetImage(
        lux::cxx::SharedBytes<> image,
        const AssetDecodeLimits& limits
    ) noexcept;

    [[nodiscard]] LUX_ASSET_PUBLIC lux::cxx::expected<CookedAssetImage, AssetDecodeFailure>
    inspectCookedAssetImage(
        AssetId requested,
        lux::cxx::SharedBytes<> image,
        const AssetDecodeLimits& limits
    ) noexcept;

    class LUX_ASSET_PUBLIC CookedAssetImage final
    {
    public:
        CookedAssetImage() noexcept = default;

        [[nodiscard]] std::uint32_t magic() const noexcept;
        [[nodiscard]] std::uint32_t version() const noexcept;
        [[nodiscard]] const CookedAssetMetadata& metadata() const noexcept;
        [[nodiscard]] const lux::cxx::SharedBytes<>& image() const noexcept;
        [[nodiscard]] const lux::cxx::SharedBytes<>& information() const noexcept;
        [[nodiscard]] const lux::cxx::SharedBytes<>& data() const noexcept;
        [[nodiscard]] std::span<const AssetAuxiliaryPayload> auxiliaryPayloads() const noexcept;

    private:
        std::uint32_t magic_{};
        std::uint32_t version_{};
        CookedAssetMetadata metadata_;
        lux::cxx::SharedBytes<> image_;
        lux::cxx::SharedBytes<> information_;
        lux::cxx::SharedBytes<> data_;
        std::vector<AssetAuxiliaryPayload> auxiliary_;

        friend struct detail::CookedAssetImageAccess;
    };

} // namespace lux::asset
