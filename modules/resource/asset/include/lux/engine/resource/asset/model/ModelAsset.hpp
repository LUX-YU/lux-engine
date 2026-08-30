#pragma once

#include <lux/engine/description/Model.hpp>
#include <lux/engine/resource/asset/Asset.hpp>
#include <lux/engine/resource/asset/AssetSerDeser.hpp>
#include <lux/engine/resource/asset/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/memory/SharedBytes.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace lux::asset
{
    struct ModelAssetData final
    {
        std::shared_ptr<const lux::rdesc::ModelNode> node_tree;
        std::vector<AssetId> mesh_assets;
        std::vector<AssetId> material_assets;
        std::optional<AssetId> skeleton_asset;
        std::vector<AssetId> animation_assets;
    };

    class LUX_ASSET_PUBLIC ModelAsset final : public TAsset<ModelAssetData>
    {
    public:
        inline static constexpr std::string_view canonical_name{"lux.model"};
        inline static constexpr AssetTypeId asset_type = AssetTypeId::fromName(canonical_name);
        inline static constexpr std::uint32_t primary_magic = 0x01309142U;
        inline static constexpr std::uint32_t legacy_type_tag = 1U;

        [[nodiscard]] static lux::cxx::expected<std::shared_ptr<const ModelAsset>, AssetDecodeFailure>
        create(
            AssetInfo info,
            std::shared_ptr<const ModelAssetData> data,
            std::vector<AssetAuxiliaryPayload> auxiliary = {}
        ) noexcept;

    private:
        ModelAsset(
            AssetInfo info,
            std::shared_ptr<const ModelAssetData> data,
            std::vector<AssetAuxiliaryPayload> auxiliary
        ) noexcept;
    };

    template <>
    struct LUX_ASSET_PUBLIC TAssetSerDeser<ModelAsset> final
    {
        [[nodiscard]] static lux::cxx::expected<std::shared_ptr<const ModelAsset>, AssetDecodeFailure>
        decode(AssetId requested, lux::cxx::SharedBytes<> image, const AssetDecodeLimits& limits) noexcept;

        [[nodiscard]] static lux::cxx::expected<std::vector<std::byte>, AssetEncodeFailure>
        encode(const ModelAsset& asset, const AssetEncodeLimits& limits) noexcept;
    };
} // namespace lux::asset
