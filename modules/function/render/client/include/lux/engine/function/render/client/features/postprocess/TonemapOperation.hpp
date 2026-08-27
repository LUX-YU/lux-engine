#pragma once
// ============================================================================
//  TonemapOperation.hpp — Tonemap 通信外观的【作者声明】(A+)
//  单 op 特性:通用反射 setParams(param_op 键)。设置面板经共享
//  FeatureParamsProxy 推 TonemapParams 反射块,无专属载荷/Proxy(INC-B 统一)。
//  Operation 面由 engine_add_comm_ops 生成;本特性 handler 语义为零
//  (Param op 走共享 registerFeatureParamsOp),连 Handlers.cpp 都只剩
//  createFn 同名抄写 —— 已由生成物接管,手写文件退役删除。
// ============================================================================
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>

#include <cstdint>
#include <string_view>
#include <type_traits>

namespace lux::render
{
    // =========================================================================
    //  Default shader name constants for TonemapFeature
    // =========================================================================
    inline constexpr std::string_view kTonemapVertShaderName = "tonemap.vert";
    inline constexpr std::string_view kTonemapFragShaderName = "tonemap.frag";

    /// 色调映射算子。comm 层公开定义(uint8_t 底型,wire 稳定)——客户端用
    /// 枚举名而非裸数字;server 侧 EToneMapOperator 是它的别名。
    enum class ETonemapOperator : uint8_t
    {
        REINHARD = 0,
        ACES_FILMIC = 1,
        UNCHARTED2 = 2,
        NONE = 3, ///< pass-through (debug: view raw HDR values)
    };

    /// Comm-layer config for TonemapFeature.
    struct LUX_COMM_CONFIG(
        prefix = Tonemap,
        id = lux.render.tonemap.v1,
        display = Tonemap,
        feature = TonemapFeature,
        feature_header = lux / engine / render / renderer / features / postprocess / TonemapFeature.hpp,
        param_op = TonemapParams,
        param_lane = frame) TonemapCommConfig
    {
        ShaderHandle vertex_shader{};   ///< fullscreen triangle
        ShaderHandle fragment_shader{}; ///< tonemap.frag
        ETonemapOperator tone_map_op{ETonemapOperator::ACES_FILMIC};
        float exposure{1.0f};
        float gamma{2.2f};
    };
    static_assert(std::is_trivially_copyable_v<TonemapCommConfig>);

} // namespace lux::render
