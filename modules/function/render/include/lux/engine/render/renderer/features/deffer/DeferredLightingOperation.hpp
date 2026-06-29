#pragma once
#include <lux/engine/render/comm/RenderCommTypes.hpp>
#include <lux/engine/render/core/ResourceHandle.hpp>
#include <lux/engine/render/core/EShadowTechnique.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <type_traits>
#include <string_view>

namespace lux::render
{
    struct FeatureFactory;

    // =========================================================================
    //  Default shader name constants for DeferredLightingFeature
    // =========================================================================
    inline constexpr std::string_view kDeferredLightingVertShaderName = "deferred_lighting.vert";
    inline constexpr std::string_view kDeferredLightingFragShaderName = "deferred_lighting.frag";
    inline constexpr std::string_view kClusterBuildShaderName         = "cluster_build.comp";
    inline constexpr std::string_view kClusterCountShaderName         = "cluster_count.comp";
    inline constexpr std::string_view kClusterScanShaderName          = "cluster_scan.comp";
    inline constexpr std::string_view kClusterFillShaderName          = "cluster_fill.comp";
    inline constexpr std::string_view kClusterClearShaderName         = "clear_count_buffers.comp";

    /// Comm-layer config for DeferredLightingFeature.
    struct DeferredLightingCommConfig
    {
        ShaderHandle vertex_shader{};     ///< fullscreen triangle
        ShaderHandle fragment_shader{};   ///< deferred_lighting.frag (variant chosen by `technique` if empty)
        uint8_t  read_mode{0};         ///< 0 = SAMPLED, 1 = INPUT_ATTACHMENT

        ShaderHandle cluster_build_shader{};
        ShaderHandle cluster_count_shader{};
        ShaderHandle cluster_scan_shader{};
        ShaderHandle cluster_fill_shader{};
        ShaderHandle cluster_clear_shader{};
        uint32_t enable_clustered{0};
        uint32_t cluster_x{16};
        uint32_t cluster_y{9};
        uint32_t cluster_z{24};
        uint32_t max_cluster_indices{1'048'576};
        /// Shadow technique whose lighting SPIR-V variant to bind. MUST match
        /// `ShadowMapCommConfig::default_technique` or lighting will sample
        /// the wrong atlas (PCF D32 vs EVSM RGBA16F).
        EShadowTechnique technique{EShadowTechnique::PCF};
        uint8_t _pad[7]{};
    };
    static_assert(std::is_trivially_copyable_v<DeferredLightingCommConfig>);

    /// DeferredLighting feature factory — registered at runtime via RegisterFeatureType.
    extern LUX_FUNCTION_PUBLIC const FeatureFactory kDeferredLightingFeatureFactory;

} // namespace lux::render
