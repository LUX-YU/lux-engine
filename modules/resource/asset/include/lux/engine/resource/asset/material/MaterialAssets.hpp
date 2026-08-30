#pragma once

#include <lux/engine/description/ShaderInfo.hpp>
#include <lux/engine/resource/asset/Asset.hpp>
#include <lux/engine/resource/asset/AssetSerDeser.hpp>
#include <lux/engine/resource/asset/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/memory/SharedBytes.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace lux::asset
{
    struct MaterialAssetData final
    {
        inline static constexpr std::uint32_t kMaxParams = 16U;
        inline static constexpr std::uint32_t kMaxTextures = 8U;

        std::array<std::array<float, 4U>, kMaxParams> parameter_defaults{};
        std::uint32_t parameter_count{};
        std::uint32_t alpha_mode{};
        bool double_sided{};
        std::vector<std::uint32_t> gbuffer_spirv;
        lux::rdesc::ShaderInfo gbuffer_info;
        std::vector<std::uint32_t> forward_spirv;
        lux::rdesc::ShaderInfo forward_info;
        std::array<AssetId, kMaxTextures> texture_slot_ids{};
    };

    struct MaterialInstanceAssetData final
    {
        inline static constexpr std::uint32_t kMaxParams = MaterialAssetData::kMaxParams;
        inline static constexpr std::uint32_t kMaxTextures = MaterialAssetData::kMaxTextures;

        AssetId parent_material_id;
        std::uint32_t param_override_mask{};
        std::array<std::array<float, 4U>, kMaxParams> params{};
        std::uint32_t tex_override_mask{};
        std::array<AssetId, kMaxTextures> texture_slot_ids{};
        std::uint32_t render_state_override{};
        std::uint32_t alpha_mode{};
        bool double_sided{};
    };

    class LUX_ASSET_PUBLIC MaterialAsset final : public TAsset<MaterialAssetData>
    {
    public:
        inline static constexpr std::string_view canonical_name{"lux.material"};
        inline static constexpr AssetTypeId asset_type = AssetTypeId::fromName(canonical_name);
        inline static constexpr std::uint32_t primary_magic = 0x01309148U;
        inline static constexpr std::uint32_t legacy_type_tag = 9U;

        [[nodiscard]] static lux::cxx::expected<std::shared_ptr<const MaterialAsset>, AssetDecodeFailure>
        create(
            AssetInfo info,
            std::shared_ptr<const MaterialAssetData> data,
            std::vector<AssetAuxiliaryPayload> auxiliary = {}
        ) noexcept;

    private:
        MaterialAsset(
            AssetInfo info,
            std::shared_ptr<const MaterialAssetData> data,
            std::vector<AssetAuxiliaryPayload> auxiliary
        ) noexcept;
    };

    class LUX_ASSET_PUBLIC MaterialInstanceAsset final : public TAsset<MaterialInstanceAssetData>
    {
    public:
        inline static constexpr std::string_view canonical_name{"lux.material-instance"};
        inline static constexpr AssetTypeId asset_type = AssetTypeId::fromName(canonical_name);
        inline static constexpr std::uint32_t primary_magic = 0x01309149U;
        inline static constexpr std::uint32_t legacy_type_tag = 10U;

        [[nodiscard]] static lux::cxx::expected<
            std::shared_ptr<const MaterialInstanceAsset>,
            AssetDecodeFailure
        > create(
            AssetInfo info,
            std::shared_ptr<const MaterialInstanceAssetData> data,
            std::vector<AssetAuxiliaryPayload> auxiliary = {}
        ) noexcept;

    private:
        MaterialInstanceAsset(
            AssetInfo info,
            std::shared_ptr<const MaterialInstanceAssetData> data,
            std::vector<AssetAuxiliaryPayload> auxiliary
        ) noexcept;
    };

    template <>
    struct LUX_ASSET_PUBLIC TAssetSerDeser<MaterialAsset> final
    {
        [[nodiscard]] static lux::cxx::expected<std::shared_ptr<const MaterialAsset>, AssetDecodeFailure>
        decode(AssetId requested, lux::cxx::SharedBytes<> image, const AssetDecodeLimits& limits) noexcept;

        [[nodiscard]] static lux::cxx::expected<std::vector<std::byte>, AssetEncodeFailure>
        encode(const MaterialAsset& asset, const AssetEncodeLimits& limits) noexcept;
    };

    template <>
    struct LUX_ASSET_PUBLIC TAssetSerDeser<MaterialInstanceAsset> final
    {
        [[nodiscard]] static lux::cxx::expected<
            std::shared_ptr<const MaterialInstanceAsset>,
            AssetDecodeFailure
        > decode(AssetId requested, lux::cxx::SharedBytes<> image, const AssetDecodeLimits& limits) noexcept;

        [[nodiscard]] static lux::cxx::expected<std::vector<std::byte>, AssetEncodeFailure>
        encode(const MaterialInstanceAsset& asset, const AssetEncodeLimits& limits) noexcept;
    };
} // namespace lux::asset
