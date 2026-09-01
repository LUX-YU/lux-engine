#pragma once

#include <lux/engine/description/MaterialEnums.hpp>
#include <lux/engine/description/Texture.hpp>
#include <lux/engine/resource/identity/AssetId.hpp>

#include <Eigen/Core>

#include <optional>

namespace lux::material
{
    struct ImportedTextureReference final
    {
        lux::asset::AssetId texture;
        lux::rdesc::ETextureColorSpace color_space{lux::rdesc::ETextureColorSpace::SRGB};
    };

    struct ImportedMaterialDescription final
    {
        Eigen::Vector3f base_color{1.0F, 1.0F, 1.0F};
        float opacity{1.0F};
        float metallic{1.0F};
        float roughness{1.0F};
        float normal_scale{1.0F};
        float occlusion_strength{1.0F};
        Eigen::Vector3f emissive{0.0F, 0.0F, 0.0F};
        float emissive_intensity{1.0F};

        std::optional<ImportedTextureReference> base_color_texture;
        std::optional<ImportedTextureReference> normal_texture;
        std::optional<ImportedTextureReference> metallic_roughness_texture;
        std::optional<ImportedTextureReference> occlusion_texture;
        std::optional<ImportedTextureReference> emissive_texture;

        lux::rdesc::EAlphaMode alpha_mode{lux::rdesc::EAlphaMode::Opaque};
        float alpha_cutoff{0.5F};
        bool double_sided{};
        bool legacy_lit{};
    };
} // namespace lux::material
