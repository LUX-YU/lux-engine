#include <lux/engine/material/Cooker.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <utility>

int main()
{
    std::array<std::uint8_t, 16U> id_bytes{};
    id_bytes.back() = 1U;
    lux::asset::AssetInfo info{};
    info.id = lux::asset::AssetId{id_bytes};
    info.type = lux::asset::MaterialAsset::asset_type;
    constexpr char name[] = "installed-material";
    std::memcpy(info.display_name.data(), name, sizeof(name) - 1U);

    lux::material::ImportedMaterialDescription imported;
    imported.base_color = {0.25F, 0.5F, 0.75F};
    const auto cooked = lux::material::cookImportedMaterial(std::move(info), imported);
    return cooked && !(*cooked)->data().gbuffer_spirv.empty() ? 0 : 1;
}
