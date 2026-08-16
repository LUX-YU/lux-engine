#pragma once

#include <lux/engine/render/core/Hash.hpp>
#include <cstdint>
#include <functional>

namespace lux::render
{
    /**
     * @brief Specialization constant IDs reserved for non-feature controls.
     *
     * IDs 0..31 are used by ShaderFeatureMask bits. Keep these IDs outside
     * that range so template-level overrides don't collide with feature bits.
     */
    inline constexpr uint32_t kSpecConstMaterialFamily = 100u;
    inline constexpr uint32_t kSpecConstHZBMode = 102u;

    /**
     * @brief Shader feature flags for permutation-based pipeline variants.
     *
     * Each feature corresponds to a specialization constant ID in SPIR-V shaders.
     * The renderer uses these flags to select the correct shader variant at draw time
     * without recompiling shader modules — only specialization constant data changes.
     */
    enum class EShaderFeature : uint32_t
    {
        NONE           = 0,
        HAS_NORMAL_MAP = 1u << 0,   ///< Normal mapping enabled
        HAS_SKINNING   = 1u << 1,   ///< Skeletal animation
        HAS_EMISSION   = 1u << 2,   ///< Emissive channel
        ALPHA_CUTOUT   = 1u << 3,   ///< Alpha test / cutout
        DOUBLE_SIDED   = 1u << 4,   ///< Two-sided rendering (no backface cull)
        POINT_CLOUD    = 1u << 5,   ///< Point cloud rendering mode
        WIREFRAME      = 1u << 6,   ///< Wireframe overlay
        HAS_AO_MAP     = 1u << 7,   ///< Ambient occlusion map
        HAS_METALLIC_MAP = 1u << 8, ///< Metallic/roughness map
        VERTEX_COLOR   = 1u << 9,   ///< Per-vertex color attribute
        INSTANCED      = 1u << 10,  ///< GPU instancing path
        SHADOW_CASTER  = 1u << 11,  ///< Shadow map caster variant
        DEPTH_ONLY     = 1u << 12,  ///< Depth pre-pass (no color writes)
        HAS_RAMP_MAP   = 1u << 13,  ///< Toon/ramp lookup texture (Stylized)
        HAS_CLEARCOAT  = 1u << 14,  ///< Clearcoat layer enabled
        HAS_SHEEN      = 1u << 15,  ///< Sheen layer enabled
        HAS_FRESNEL_LAYER = 1u << 16, ///< Fresnel layer enabled
        // bits 17..31 reserved for future features
    };

    /// @brief Bitmask type for combining EShaderFeature flags
    using ShaderFeatureMask = uint32_t;

    /// @brief Bitwise OR for EShaderFeature
    inline constexpr ShaderFeatureMask operator|(EShaderFeature a, EShaderFeature b) noexcept
    {
        return static_cast<ShaderFeatureMask>(a) | static_cast<ShaderFeatureMask>(b);
    }

    /// @brief Bitwise OR for mask and feature
    inline constexpr ShaderFeatureMask operator|(ShaderFeatureMask a, EShaderFeature b) noexcept
    {
        return a | static_cast<ShaderFeatureMask>(b);
    }

    /// @brief Bitwise OR-assign for mask and feature
    inline constexpr ShaderFeatureMask& operator|=(ShaderFeatureMask& a, EShaderFeature b) noexcept
    {
        a = a | b;
        return a;
    }

    /// @brief Bitwise AND for testing feature presence
    inline constexpr ShaderFeatureMask operator&(ShaderFeatureMask a, EShaderFeature b) noexcept
    {
        return a & static_cast<ShaderFeatureMask>(b);
    }

    /// @brief Test if a specific feature is set in the mask
    inline constexpr bool hasFeature(ShaderFeatureMask mask, EShaderFeature feature) noexcept
    {
        return (mask & static_cast<ShaderFeatureMask>(feature)) != 0;
    }

    /// @brief Count the number of active features in the mask
    inline constexpr uint32_t featureCount(ShaderFeatureMask mask) noexcept
    {
        // Hamming weight (popcount)
        uint32_t n = mask;
        n = n - ((n >> 1) & 0x55555555u);
        n = (n & 0x33333333u) + ((n >> 2) & 0x33333333u);
        return (((n + (n >> 4)) & 0x0F0F0F0Fu) * 0x01010101u) >> 24;
    }

    /**
     * @brief Composite key identifying a specific shader permutation.
     *
     * Combines a base shader identifier (e.g. FNV-1a hash of shader path)
     * with a feature bitmask to uniquely identify a compiled variant.
     */
    struct ShaderPermutationKey
    {
        uint64_t          base_shader_id{ 0 }; ///< FNV-1a hash of the base shader path/name
        ShaderFeatureMask features{ 0 };        ///< Active feature combination

        bool operator==(const ShaderPermutationKey&) const = default;
    };

    /**
     * @brief Hash functor for ShaderPermutationKey.
     *
     * Uses the same hash_combine pattern as the rest of the engine.
     */
    struct ShaderPermutationKeyHash
    {
        std::size_t operator()(const ShaderPermutationKey& k) const noexcept
        {
            std::size_t h = std::hash<uint64_t>{}(k.base_shader_id);
            hash_combine(h, std::hash<uint32_t>{}(k.features));
            return h;
        }
    };

} // namespace lux::render
