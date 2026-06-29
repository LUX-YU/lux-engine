#pragma once
#include <lux/engine/render/comm/RenderCommTypes.hpp>
#include <lux/engine/render/core/ResourceHandle.hpp>
#include <lux/engine/render/renderer/features/postprocess/TonemapParams.hpp>
#include <lux/engine/render/renderer/features/FeatureParamsOperation.hpp>   // SetFeatureParamsPayload
#include <lux/engine/render/renderer/features/FeatureOps.hpp>               // EOpKind / FeatureOpDesc
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <type_traits>
#include <string_view>

namespace lux::render
{
    struct FeatureFactory;

    // =========================================================================
    //  Default shader name constants for TonemapFeature
    // =========================================================================
    inline constexpr std::string_view kTonemapVertShaderName = "tonemap.vert";
    inline constexpr std::string_view kTonemapFragShaderName = "tonemap.frag";

    /// Comm-layer config for TonemapFeature.
    struct TonemapCommConfig
    {
        ShaderHandle vertex_shader{};     ///< fullscreen triangle
        ShaderHandle fragment_shader{};   ///< tonemap.frag
        uint8_t  tone_map_op{1};       ///< 0=Reinhard, 1=ACES, 2=Uncharted2, 3=None
        float    exposure{1.0f};
        float    gamma{2.2f};
    };
    static_assert(std::is_trivially_copyable_v<TonemapCommConfig>);

    /// Tonemap's single op: the GENERIC reflected setParams (EOpKind::Param). The
    /// typed-op registrar registers it via the shared registerFeatureParamsOp and
    /// auto-derives FeatureFactory.param_set_op_index from its position — no per-feature
    /// register_ops_fn, no hand-counted index.
    struct TonemapParamsOp
    {
        using Payload = SetFeatureParamsPayload;
        static constexpr EOpKind kind = EOpKind::Param;
        static constexpr const char* name = "TonemapParams";
    };

    /// Tonemap feature factory. Registers the one TonemapParamsOp so the settings panel
    /// pushes a reflected TonemapParams blob via the shared FeatureParamsProxy. No
    /// Tonemap-specific payload/proxy (the INC-B unification).
    extern LUX_FUNCTION_PUBLIC const FeatureFactory kTonemapFeatureFactory;

} // namespace lux::render
