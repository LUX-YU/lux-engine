#pragma once

#include <lux/engine/description/Texture.hpp>
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
    class LUX_ASSET_PUBLIC TextureAsset final : public TAsset<lux::rdesc::Texture>
    {
    public:
        inline static constexpr std::string_view canonical_name{"lux.texture"};
        inline static constexpr AssetTypeId asset_type = AssetTypeId::fromName(canonical_name);
        inline static constexpr std::uint32_t primary_magic = 0x01309143U;
        inline static constexpr std::uint32_t legacy_type_tag = 0U;

        [[nodiscard]] static lux::cxx::expected<std::shared_ptr<const TextureAsset>, AssetDecodeFailure>
        create(
            AssetInfo info,
            std::shared_ptr<const lux::rdesc::Texture> data,
            std::vector<AssetAuxiliaryPayload> auxiliary = {}
        ) noexcept;

    private:
        TextureAsset(
            AssetInfo info,
            std::shared_ptr<const lux::rdesc::Texture> data,
            std::vector<AssetAuxiliaryPayload> auxiliary
        ) noexcept;
    };

    template <>
    struct LUX_ASSET_PUBLIC TAssetSerDeser<TextureAsset> final
    {
        [[nodiscard]] static lux::cxx::expected<std::shared_ptr<const TextureAsset>, AssetDecodeFailure>
        decode(
            AssetId requested,
            lux::cxx::SharedBytes<> cooked_image,
            const AssetDecodeLimits& limits
        ) noexcept;

        [[nodiscard]] static lux::cxx::expected<std::vector<std::byte>, AssetEncodeFailure>
        encode(const TextureAsset& asset, const AssetEncodeLimits& limits) noexcept;
    };
} // namespace lux::asset
