#pragma once

#include <lux/engine/resource/asset/Asset.hpp>
#include <lux/engine/resource/asset/AssetSerDeser.hpp>
#include <lux/engine/world/WorldDescription.hpp>
#include <lux/engine/world/asset/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/memory/SharedBytes.hpp>

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace lux::world
{
    inline constexpr std::string_view WorldAssetCanonicalName{"lux.world.description"};
    inline constexpr std::uint32_t WorldAssetPrimaryMagic{0x4457584CU};

    class LUX_ENGINE_WORLD_ASSET_PUBLIC WorldAsset final
        : public lux::asset::TAsset<WorldDescription>
    {
    public:
        inline static constexpr std::string_view canonical_name = WorldAssetCanonicalName;
        inline static constexpr lux::asset::AssetTypeId asset_type =
            lux::asset::AssetTypeId::fromName(canonical_name);
        inline static constexpr std::uint32_t primary_magic = WorldAssetPrimaryMagic;
        inline static constexpr std::uint32_t legacy_type_tag = lux::asset::kNoLegacyAssetTypeTag;

        [[nodiscard]] static lux::cxx::expected<
            std::shared_ptr<const WorldAsset>,
            lux::asset::AssetDecodeFailure
        > create(
            lux::asset::AssetInfo info,
            std::shared_ptr<const WorldDescription> data,
            std::vector<lux::asset::AssetAuxiliaryPayload> auxiliary = {}
        ) noexcept;

    private:
        WorldAsset(
            lux::asset::AssetInfo info,
            std::shared_ptr<const WorldDescription> data,
            std::vector<lux::asset::AssetAuxiliaryPayload> auxiliary
        ) noexcept;
    };
} // namespace lux::world

namespace lux::asset
{
    template <>
    struct TAssetSerDeser<lux::world::WorldAsset> final
    {
        [[nodiscard]] static LUX_ENGINE_WORLD_ASSET_PUBLIC lux::cxx::expected<
            std::shared_ptr<const lux::world::WorldAsset>,
            AssetDecodeFailure
        > decode(
            AssetId requested,
            lux::cxx::SharedBytes<> cooked_image,
            const AssetDecodeLimits& limits
        ) noexcept;

        [[nodiscard]] static LUX_ENGINE_WORLD_ASSET_PUBLIC lux::cxx::expected<
            std::vector<std::byte>,
            AssetEncodeFailure
        > encode(
            const lux::world::WorldAsset& asset,
            const AssetEncodeLimits& limits
        ) noexcept;
    };
} // namespace lux::asset
