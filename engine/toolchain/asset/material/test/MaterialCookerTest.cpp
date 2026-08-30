#include <lux/engine/resource/asset/AssetSerDeser.hpp>
#include <lux/engine/toolchain/asset/material/MaterialCooker.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace
{
    lux::asset::AssetId id(std::uint8_t value)
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes.back() = value;
        return lux::asset::AssetId{bytes};
    }

    lux::asset::AssetInfo info(std::uint8_t value)
    {
        lux::asset::AssetInfo result{};
        result.id = id(value);
        result.type = lux::asset::MaterialAsset::asset_type;
        constexpr char name[] = "material-cooker-test";
        std::memcpy(result.display_name.data(), name, sizeof(name) - 1U);
        return result;
    }
}

int main()
{
    lux::toolchain::ImportedMaterialDescription imported;
    imported.base_color = {0.25F, 0.5F, 0.75F};
    imported.metallic = 0.2F;
    imported.roughness = 0.7F;
    imported.base_color_texture = lux::toolchain::ImportedTextureReference{
        id(1U),
        lux::rdesc::ETextureColorSpace::SRGB
    };
    imported.alpha_mode = lux::rdesc::EAlphaMode::Mask;
    imported.alpha_cutoff = 0.4F;
    imported.double_sided = true;

    const auto cooked = lux::toolchain::cookImportedMaterial(info(2U), imported);
    if (!cooked)
        std::cerr << "material cook failed: " << cooked.error().detail << '\n';
    assert(cooked);
    assert((*cooked)->data().texture_slot_ids[0] == id(1U));
    assert((*cooked)->data().alpha_mode == lux::rdesc::EAlphaMode::Mask);
    assert((*cooked)->data().double_sided);
    assert((*cooked)->data().gbuffer_spirv.size() >= 5U);
    assert((*cooked)->data().forward_spirv.size() >= 5U);

    constexpr lux::asset::AssetEncodeLimits encode_limits{16U * 1024U * 1024U};
    const auto encoded = lux::asset::TAssetSerDeser<lux::asset::MaterialAsset>::encode(
        **cooked,
        encode_limits
    );
    assert(encoded);

    auto invalid = imported;
    invalid.base_color_texture->texture = {};
    assert(!lux::toolchain::cookImportedMaterial(info(3U), invalid));
    return 0;
}
