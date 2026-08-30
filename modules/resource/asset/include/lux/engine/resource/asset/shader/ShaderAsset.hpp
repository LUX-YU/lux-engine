#pragma once

#include <lux/engine/description/Shader.hpp>
#include <lux/engine/description/ShaderInfo.hpp>
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
    struct ShaderAssetData final
    {
        lux::rdesc::Shader shader;
        lux::rdesc::ShaderInfo info;
    };

    class LUX_ASSET_PUBLIC ShaderAsset final : public TAsset<ShaderAssetData>
    {
    public:
        inline static constexpr std::string_view canonical_name{"lux.shader"};
        inline static constexpr AssetTypeId asset_type = AssetTypeId::fromName(canonical_name);
        inline static constexpr std::uint32_t primary_magic = 0x01309144U;
        inline static constexpr std::uint32_t legacy_type_tag = 2U;

        [[nodiscard]] static lux::cxx::expected<std::shared_ptr<const ShaderAsset>, AssetDecodeFailure>
        create(
            AssetInfo info,
            std::shared_ptr<const ShaderAssetData> data,
            std::vector<AssetAuxiliaryPayload> auxiliary = {}
        ) noexcept;

    private:
        ShaderAsset(
            AssetInfo info,
            std::shared_ptr<const ShaderAssetData> data,
            std::vector<AssetAuxiliaryPayload> auxiliary
        ) noexcept;
    };

    template <>
    struct LUX_ASSET_PUBLIC TAssetSerDeser<ShaderAsset> final
    {
        [[nodiscard]] static lux::cxx::expected<std::shared_ptr<const ShaderAsset>, AssetDecodeFailure>
        decode(
            AssetId requested,
            lux::cxx::SharedBytes<> cooked_image,
            const AssetDecodeLimits& limits
        ) noexcept;

        [[nodiscard]] static lux::cxx::expected<std::vector<std::byte>, AssetEncodeFailure>
        encode(const ShaderAsset& asset, const AssetEncodeLimits& limits) noexcept;
    };
} // namespace lux::asset
