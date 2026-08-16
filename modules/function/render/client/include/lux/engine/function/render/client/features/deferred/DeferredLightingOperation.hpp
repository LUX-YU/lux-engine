#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
#include <lux/engine/function/render/client/resources/lighting/EShadowTechnique.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <type_traits>
#include <string_view>

namespace lux::render
{
    struct FeatureFactory;

    // =========================================================================
    //  Default shader ASSET names for DeferredLightingFeature — what a client
    //  looks up in the asset system before submitting the SPIR-V through
    //  CompileShader (that op carries bytes, not names). A client that just
    //  wants the engine's own shader does not need these: leave the config
    //  field empty and the feature backfills it via
    //  createBuiltinShaderModule(EBuiltinShader::DEFERRED_LIGHTING_* / CLUSTER_*).
    //
    //  ⚠️ Unlike EBuiltinShader, these strings have NO build-time binding —
    //  nothing fails if an asset is renamed or split out from under them.
    //  Prefer EBuiltinShader when a typed handle will do.
    // =========================================================================
    inline constexpr std::string_view kDeferredLightingVertShaderName = "deferred_lighting.vert";

    /// VARIANT STEM — the fragment shader is packed once per (shadow technique ×
    /// GBuffer read mode), so the asset name is this stem plus a suffix:
    /// ".pcf" / ".evsm" (SAMPLED read) or ".pcf_lr" / ".evsm_lr" (local_read).
    /// The stem alone resolves to nothing.
    inline constexpr std::string_view kDeferredLightingFragShaderName = "deferred_lighting.frag";

    inline constexpr std::string_view kClusterBuildShaderName         = "cluster_build.comp";
    inline constexpr std::string_view kClusterCountShaderName         = "cluster_count.comp";
    inline constexpr std::string_view kClusterScanShaderName          = "cluster_scan.comp";
    inline constexpr std::string_view kClusterFillShaderName          = "cluster_fill.comp";
    inline constexpr std::string_view kClusterClearShaderName         = "clear_count_buffers.comp";

    inline constexpr uint32_t kDeferredLightingCommConfigVersion = 2u;  // v2: 删 ELightingReadMode::AUTO

    /// G-buffer 读路径。comm 层公开定义(uint8_t 底型,wire 稳定)——
    /// 客户端(orchestrator 等)用枚举名而非裸数字,server 侧
    /// DeferredLightingFeature::EReadMode 是它的别名。
    ///
    /// 只有两条真实路径。v1 曾有第三个取值 AUTO,含义是「跟随 DeviceCaps,设备不支持
    /// local-read 就降级 SAMPLED」—— 那是把「你替我选」写进了契约:客户端以为自己拿到
    /// 了 tile-local 快路,实际拿到什么要看跑在哪台机器上,而且无从得知。现在客户端先
    /// `queryDeviceCaps()` 再自己二选一;要 INPUT_ATTACHMENT 而设备不支持,装配直接报
    /// `err::lighting::LocalReadUnsupported`,不再无声换路。
    enum class ELightingReadMode : uint8_t
    {
        SAMPLED          = 0,  ///< 路径 A:独立 pass,texture() 采样。默认值。
        INPUT_ATTACHMENT = 1,  ///< 路径 B:local-read 合并作用域,subpassLoad()
    };

    /// Comm-layer config for DeferredLightingFeature.
    /// requires:G-buffer 是硬前提(addPasses 读它的附件);shadow_map 是**可选但
    /// 顺序敏感**的依赖(`?` 尾缀)——本特性 attach 时扫 scene.features() 做
    /// 阴影技术匹配校验,排在 ShadowMap 前面就扫不到、校验静默空转
    /// (DeferredLightingFeature.cpp 的注释自证)。
    struct LUX_COMM_CONFIG(prefix=DeferredLighting, id=lux.render.deferred_lighting.v1, display=DeferredLighting,
                           requires="lux.render.deferred_gbuffer.v1,lux.render.shadow_map.v1?",
                           custom_create=true)
    DeferredLightingCommConfig
    {
        uint32_t            comm_config_version{kDeferredLightingCommConfigVersion};
        ShaderHandle        vertex_shader{};     ///< fullscreen triangle
        ShaderHandle        fragment_shader{};   ///< deferred_lighting.frag (variant chosen by `technique` if empty)
        ELightingReadMode   read_mode{ELightingReadMode::SAMPLED};

        ShaderHandle        cluster_build_shader{};
        ShaderHandle        cluster_count_shader{};
        ShaderHandle        cluster_scan_shader{};
        ShaderHandle        cluster_fill_shader{};
        ShaderHandle        cluster_clear_shader{};
        uint32_t            enable_clustered{0};
        uint32_t            cluster_x{16};
        uint32_t            cluster_y{9};
        uint32_t            cluster_z{24};
        uint32_t            max_cluster_indices{1'048'576};
        /// Shadow technique whose lighting SPIR-V variant to bind. MUST match
        /// `ShadowMapCommConfig::default_technique` or lighting will sample
        /// the wrong atlas (PCF D32 vs EVSM RGBA16F).
        EShadowTechnique    technique{EShadowTechnique::PCF};
        uint8_t             _pad[7]{};
    };
    static_assert(std::is_trivially_copyable_v<DeferredLightingCommConfig>);

} // namespace lux::render
