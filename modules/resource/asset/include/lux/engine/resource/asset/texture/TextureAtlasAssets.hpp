#pragma once

#include <lux/engine/description/TextureAtlas.hpp>
#include <lux/engine/resource/asset/Asset.hpp>
#include <lux/engine/resource/asset/AssetSerDeser.hpp>
#include <lux/engine/resource/asset/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/memory/SharedBytes.hpp>

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace lux::asset
{
    class LUX_ASSET_PUBLIC TextureAtlasAsset final : public TAsset<lux::rdesc::TextureAtlas>
    {
    public:
        inline static constexpr std::string_view canonical_name{"lux.texture-atlas"};
        inline static constexpr AssetTypeId asset_type = AssetTypeId::fromName(canonical_name);
        inline static constexpr std::uint32_t primary_magic = 0x0130914AU;
        inline static constexpr std::uint32_t legacy_type_tag = 11U;

        [[nodiscard]] static lux::cxx::expected<
            std::shared_ptr<const TextureAtlasAsset>,
            AssetDecodeFailure
        > create(
            AssetInfo info,
            std::shared_ptr<const lux::rdesc::TextureAtlas> data,
            std::vector<AssetAuxiliaryPayload> auxiliary = {}
        ) noexcept;

    private:
        TextureAtlasAsset(
            AssetInfo info,
            std::shared_ptr<const lux::rdesc::TextureAtlas> data,
            std::vector<AssetAuxiliaryPayload> auxiliary
        ) noexcept;
    };

    class LUX_ASSET_PUBLIC FlipbookClipAsset final : public TAsset<lux::rdesc::FlipbookClip>
    {
    public:
        inline static constexpr std::string_view canonical_name{"lux.flipbook-clip"};
        inline static constexpr AssetTypeId asset_type = AssetTypeId::fromName(canonical_name);
        inline static constexpr std::uint32_t primary_magic = 0x0130914BU;
        inline static constexpr std::uint32_t legacy_type_tag = 12U;

        [[nodiscard]] static lux::cxx::expected<
            std::shared_ptr<const FlipbookClipAsset>,
            AssetDecodeFailure
        > create(
            AssetInfo info,
            std::shared_ptr<const lux::rdesc::FlipbookClip> data,
            std::vector<AssetAuxiliaryPayload> auxiliary = {}
        ) noexcept;

    private:
        FlipbookClipAsset(
            AssetInfo info,
            std::shared_ptr<const lux::rdesc::FlipbookClip> data,
            std::vector<AssetAuxiliaryPayload> auxiliary
        ) noexcept;
    };

    template <>
    struct LUX_ASSET_PUBLIC TAssetSerDeser<TextureAtlasAsset> final
    {
        [[nodiscard]] static lux::cxx::expected<
            std::shared_ptr<const TextureAtlasAsset>,
            AssetDecodeFailure
        > decode(AssetId requested, lux::cxx::SharedBytes<> image, const AssetDecodeLimits& limits) noexcept;

        [[nodiscard]] static lux::cxx::expected<std::vector<std::byte>, AssetEncodeFailure>
        encode(const TextureAtlasAsset& asset, const AssetEncodeLimits& limits) noexcept;
    };

    template <>
    struct LUX_ASSET_PUBLIC TAssetSerDeser<FlipbookClipAsset> final
    {
        [[nodiscard]] static lux::cxx::expected<
            std::shared_ptr<const FlipbookClipAsset>,
            AssetDecodeFailure
        > decode(AssetId requested, lux::cxx::SharedBytes<> image, const AssetDecodeLimits& limits) noexcept;

        [[nodiscard]] static lux::cxx::expected<std::vector<std::byte>, AssetEncodeFailure>
        encode(const FlipbookClipAsset& asset, const AssetEncodeLimits& limits) noexcept;
    };
} // namespace lux::asset
