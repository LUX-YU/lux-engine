#pragma once

#include <lux/engine/description/AnimationClip.hpp>
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
    class LUX_ASSET_PUBLIC AnimationClipAsset final : public TAsset<lux::rdesc::AnimationClip>
    {
    public:
        inline static constexpr std::string_view canonical_name{"lux.animation-clip"};
        inline static constexpr AssetTypeId asset_type = AssetTypeId::fromName(canonical_name);
        inline static constexpr std::uint32_t primary_magic = 0x01309147U;
        inline static constexpr std::uint32_t legacy_type_tag = 8U;

        [[nodiscard]] static lux::cxx::expected<
            std::shared_ptr<const AnimationClipAsset>,
            AssetDecodeFailure
        > create(
            AssetInfo info,
            std::shared_ptr<const lux::rdesc::AnimationClip> data,
            std::vector<AssetAuxiliaryPayload> auxiliary = {}
        ) noexcept;

    private:
        AnimationClipAsset(
            AssetInfo info,
            std::shared_ptr<const lux::rdesc::AnimationClip> data,
            std::vector<AssetAuxiliaryPayload> auxiliary
        ) noexcept;
    };

    template <>
    struct LUX_ASSET_PUBLIC TAssetSerDeser<AnimationClipAsset> final
    {
        [[nodiscard]] static lux::cxx::expected<
            std::shared_ptr<const AnimationClipAsset>,
            AssetDecodeFailure
        > decode(AssetId requested, lux::cxx::SharedBytes<> image, const AssetDecodeLimits& limits) noexcept;

        [[nodiscard]] static lux::cxx::expected<std::vector<std::byte>, AssetEncodeFailure>
        encode(const AnimationClipAsset& asset, const AssetEncodeLimits& limits) noexcept;
    };
} // namespace lux::asset
