#pragma once

#include <lux/engine/resource/asset/Asset.hpp>
#include <lux/engine/resource/asset/AssetSerDeser.hpp>
#include <lux/engine/scene/SceneData.hpp>
#include <lux/engine/resource/asset/AssetCodec.hpp>
#include <lux/engine/error/Error.hpp>
#include <lux/engine/services/serialization/scene/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/memory/SharedBytes.hpp>

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace lux::scene
{
    inline constexpr std::string_view SceneAssetCanonicalName{"lux.scene.description"};
    inline constexpr std::uint32_t SceneAssetPrimaryMagic{0x4443534CU};

    class LUX_ENGINE_SCENE_ASSET_PUBLIC SceneAsset final : public lux::asset::TAsset<SceneData>
    {
    public:
        inline static constexpr std::string_view canonical_name = SceneAssetCanonicalName;
        inline static constexpr lux::asset::AssetTypeId asset_type = lux::asset::AssetTypeId::fromName(canonical_name);
        inline static constexpr std::uint32_t primary_magic = SceneAssetPrimaryMagic;
        inline static constexpr std::uint32_t legacy_type_tag = lux::asset::kNoLegacyAssetTypeTag;

        [[nodiscard]] static lux::cxx::expected<std::shared_ptr<const SceneAsset>, lux::asset::AssetDecodeFailure>
        create(
            lux::asset::AssetInfo info,
            std::shared_ptr<const SceneData> data,
            std::vector<lux::asset::AssetAuxiliaryPayload> auxiliary = {}
        ) noexcept;

    private:
        SceneAsset(
            lux::asset::AssetInfo info,
            std::shared_ptr<const SceneData> data,
            std::vector<lux::asset::AssetAuxiliaryPayload> auxiliary
        ) noexcept;
    };
    // Host installs this codec into Repository; the resource module never depends on Engine Scene types.
    [[nodiscard]] LUX_ENGINE_SCENE_ASSET_PUBLIC std::unique_ptr<asset::AssetCodec> createSceneCodec();

    // Logical construction inputs only: physical relocation and the embedded directory are excluded.
    [[nodiscard]] LUX_ENGINE_SCENE_ASSET_PUBLIC cxx::expected<cxx::algorithm::Sha256Digest, error::Error>
    sceneStreamingInputDigest(const SceneData& data) noexcept;
} // namespace lux::scene

namespace lux::asset
{
    template <> struct TAssetSerDeser<lux::scene::SceneAsset> final
    {
        [[nodiscard]] static LUX_ENGINE_SCENE_ASSET_PUBLIC lux::cxx::
            expected<std::shared_ptr<const lux::scene::SceneAsset>, AssetDecodeFailure>
            decode(AssetId requested, lux::cxx::SharedBytes<> cooked_image, const AssetDecodeLimits& limits) noexcept;

        [[nodiscard]] static LUX_ENGINE_SCENE_ASSET_PUBLIC lux::cxx::
            expected<std::vector<std::byte>, AssetEncodeFailure>
            encode(const lux::scene::SceneAsset& asset, const AssetEncodeLimits& limits) noexcept;

        // Synchronous encoding borrows the data; no temporary Asset or shared data owner is required.
        [[nodiscard]] static LUX_ENGINE_SCENE_ASSET_PUBLIC
        lux::cxx::expected<std::vector<std::byte>, AssetEncodeFailure>
        encode(
            const AssetInfo& info, const lux::scene::SceneData& data,
            std::span<const AssetAuxiliaryPayload> auxiliary, const AssetEncodeLimits& limits
        ) noexcept;
    };
} // namespace lux::asset
