#pragma once
/**
 * @file MaterialShaderKey.hpp
 * @brief Material-driven shader variant key for permutation selection.
 *
 * MaterialShaderKey captures only the features that change shader code paths.
 * Purely numeric parameters (roughness, metallic, etc.) go into UBOs/push
 * constants and do NOT participate in keying.
 *
 * The key is built from rdesc::Material at material submit time and converted
 * to a ShaderFeatureMask for the ShaderPermutationCompiler.
 */

#include <lux/engine/render/resources/material/MaterialFamily.hpp>
#include <lux/engine/render/gpu/pipeline/ShaderPermutation.hpp>
#include <lux/engine/render/core/Hash.hpp>

#include <functional>

namespace lux::render
{
    namespace detail
    {
        inline bool supportsLayers(ELightingTechnique family) noexcept
        {
            return family == ELightingTechnique::LegacyLit || family == ELightingTechnique::PbrMetallicRoughness;
        }

        inline bool supportsAlphaCutout(ELightingTechnique family) noexcept
        {
            return family == ELightingTechnique::PbrMetallicRoughness;
        }
    } // namespace detail

    /**
     * @brief Key that captures branch-level material features for shader selection.
     */
    struct MaterialShaderKey
    {
        ELightingTechnique family{ELightingTechnique::Unlit};
        rdesc::EDiffuseModel diffuse_model{rdesc::EDiffuseModel::Lambert}; // only LegacyLit
        rdesc::ESpecularModel specular_model{rdesc::ESpecularModel::None}; // only LegacyLit

        bool has_base_color_texture{false};
        bool has_normal_texture{false};
        bool has_emissive_texture{false};
        bool has_occlusion_texture{false};
        bool has_ramp_texture{false};               // Stylized only
        bool has_metallic_roughness_texture{false}; // PBR only

        bool has_fresnel_layer{false};
        bool has_clearcoat_layer{false};
        bool has_sheen_layer{false};

        rdesc::EAlphaMode alpha_mode{rdesc::EAlphaMode::Opaque};
        bool double_sided{false};

        bool operator==(const MaterialShaderKey&) const = default;
    };

    // Enforce one canonical key contract so family/closure/alpha map to
    // stable feature masks and cache keys regardless of call-site construction.
    inline MaterialShaderKey canonicalizeMaterialShaderKey(MaterialShaderKey key) noexcept
    {
        switch (key.family)
        {
        case ELightingTechnique::Unlit:
            key.diffuse_model = rdesc::EDiffuseModel::Lambert;
            key.specular_model = rdesc::ESpecularModel::None;
            key.has_normal_texture = false;
            key.has_occlusion_texture = false;
            key.has_ramp_texture = false;
            key.has_metallic_roughness_texture = false;
            key.has_fresnel_layer = false;
            key.has_clearcoat_layer = false;
            key.has_sheen_layer = false;
            key.alpha_mode = rdesc::EAlphaMode::Opaque;
            break;

        case ELightingTechnique::LegacyLit:
            key.has_occlusion_texture = false;
            key.has_ramp_texture = false;
            key.has_metallic_roughness_texture = false;
            key.alpha_mode = rdesc::EAlphaMode::Opaque;
            break;

        case ELightingTechnique::PbrMetallicRoughness:
            key.diffuse_model = rdesc::EDiffuseModel::Lambert;
            key.specular_model = rdesc::ESpecularModel::None;
            key.has_ramp_texture = false;
            break;

        case ELightingTechnique::Stylized:
            key.diffuse_model = rdesc::EDiffuseModel::Lambert;
            key.specular_model = rdesc::ESpecularModel::None;
            key.has_occlusion_texture = false;
            key.has_metallic_roughness_texture = false;
            key.has_fresnel_layer = false;
            key.has_clearcoat_layer = false;
            key.has_sheen_layer = false;
            key.alpha_mode = rdesc::EAlphaMode::Opaque;
            break;

        default:
            key = {};
            break;
        }

        if (!detail::supportsLayers(key.family))
        {
            key.has_fresnel_layer = false;
            key.has_clearcoat_layer = false;
            key.has_sheen_layer = false;
        }
        if (!detail::supportsAlphaCutout(key.family))
            key.alpha_mode = rdesc::EAlphaMode::Opaque;

        return key;
    }

    /**
     * @brief Full shader cache query key — material + pass + geometry features.
     */
    struct DrawShaderKey
    {
        MaterialShaderKey material;

        bool has_tangent{false};
        bool skinned{false};
        bool instanced{false};

        bool operator==(const DrawShaderKey&) const = default;
    };

    // (buildMaterialShaderKey(const rdesc::Material&) retired in W5a — the builtin
    //  closure-material upload path that consumed it was removed. Graph materials get
    //  their PSO identity from their baked frag shader handle, not a feature key.)

    // ===== Convert to ShaderFeatureMask for specialization constants =====
    inline ShaderFeatureMask toFeatureMask(const MaterialShaderKey& key)
    {
        const MaterialShaderKey k = canonicalizeMaterialShaderKey(key);
        ShaderFeatureMask mask = 0;
        if (k.has_normal_texture)
            mask |= EShaderFeature::HAS_NORMAL_MAP;
        if (k.has_emissive_texture)
            mask |= EShaderFeature::HAS_EMISSION;
        // Two-sided materials get their own variant bucket so the mesh features
        // can route them to a no-cull pipeline (rasterizer cull mode). The bit is
        // inert in shaders (no spec constant declared at this id) — it only acts
        // as a bucket/pipeline-cache discriminator.
        if (k.double_sided)
            mask |= EShaderFeature::DOUBLE_SIDED;

        if (k.family == ELightingTechnique::PbrMetallicRoughness)
        {
            if (k.alpha_mode == rdesc::EAlphaMode::Mask)
                mask |= EShaderFeature::ALPHA_CUTOUT;
            if (k.has_occlusion_texture)
                mask |= EShaderFeature::HAS_AO_MAP;
            if (k.has_metallic_roughness_texture)
                mask |= EShaderFeature::HAS_METALLIC_MAP;
        }
        if (k.family == ELightingTechnique::Stylized)
        {
            if (k.has_ramp_texture)
                mask |= EShaderFeature::HAS_RAMP_MAP;
        }
        if (detail::supportsLayers(k.family))
        {
            if (k.has_clearcoat_layer)
                mask |= EShaderFeature::HAS_CLEARCOAT;
            if (k.has_sheen_layer)
                mask |= EShaderFeature::HAS_SHEEN;
            if (k.has_fresnel_layer)
                mask |= EShaderFeature::HAS_FRESNEL_LAYER;
        }
        return mask;
    }

    inline ShaderFeatureMask toFeatureMask(const DrawShaderKey& key)
    {
        ShaderFeatureMask mask = toFeatureMask(key.material);
        if (key.skinned)
            mask |= EShaderFeature::HAS_SKINNING;
        if (key.instanced)
            mask |= EShaderFeature::INSTANCED;
        return mask;
    }

    // ===== Hash =====

    struct MaterialShaderKeyHash
    {
        std::size_t operator()(const MaterialShaderKey& k) const noexcept
        {
            const MaterialShaderKey key = canonicalizeMaterialShaderKey(k);
            std::size_t h = std::hash<uint8_t>{}(static_cast<uint8_t>(key.family));
            hash_combine(h, std::hash<uint8_t>{}(static_cast<uint8_t>(key.diffuse_model)));
            hash_combine(h, std::hash<uint8_t>{}(static_cast<uint8_t>(key.specular_model)));
            hash_combine(h, std::hash<uint8_t>{}(static_cast<uint8_t>(key.alpha_mode)));
            uint16_t bools = (key.has_base_color_texture << 0) | (key.has_normal_texture << 1) |
                             (key.has_emissive_texture << 2) | (key.has_occlusion_texture << 3) |
                             (key.has_ramp_texture << 4) | (key.has_metallic_roughness_texture << 5) |
                             (key.has_fresnel_layer << 6) | (key.has_clearcoat_layer << 7) |
                             (key.has_sheen_layer << 8) | (key.double_sided << 9);
            hash_combine(h, std::hash<uint16_t>{}(bools));
            return h;
        }
    };

    struct DrawShaderKeyHash
    {
        std::size_t operator()(const DrawShaderKey& k) const noexcept
        {
            std::size_t h = MaterialShaderKeyHash{}(k.material);
            uint8_t geom = (k.has_tangent << 0) | (k.skinned << 1) | (k.instanced << 2);
            hash_combine(h, std::hash<uint8_t>{}(geom));
            return h;
        }
    };

} // namespace lux::render
