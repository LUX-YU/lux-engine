#pragma once
// ============================================================================
//  LinearDepthOperation.hpp — LinearDepth 生产者的线协议面
//
//  该特性把 SceneDepth 解算为眼空间线性深度,写入 TargetSlot::LINEAR_DEPTH
//  (sensors / SSAO / DoF 的公共输入)。装上即生效:它经
//  requiredTargetSlots() 声明 LinearDepth(补缺)与 SceneDepth(追加采样),
//  槽的物理背衬与图接入由引擎自动补齐 —— 客户端零额外配置。
// ============================================================================
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/function/render/client/protocol/FeatureFactory.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
#include <lux/engine/function/visibility.h>

#include <type_traits>

namespace lux::render
{
    /// Comm-layer config。两个句柄都空 = 全内置(替换阶梯第 2 级:
    /// 非空即换掉对应着色器,管线形状不变)。
    // No custom_create: the createFn this feature needs is the plain same-name
    // field copy the generator emits. It used to be hand-written, which meant a
    // new config field silently kept its default until someone remembered to add
    // the assignment by hand — the exact failure the generator exists to remove.
    struct LUX_COMM_CONFIG(
        prefix = LinearDepth,
        id = lux.render.linear_depth.v1,
        display = LinearDepth,
        feature = LinearDepthFeature,
        feature_header = lux / engine / render / renderer / features / postprocess /
                         LinearDepthFeature.hpp) LinearDepthCommConfig
    {
        ShaderHandle vertex_shader{};   ///< 空 = 内置全屏三角(TONEMAP_VERT)
        ShaderHandle fragment_shader{}; ///< 空 = 内置 LINEAR_DEPTH_FRAG
    };
    static_assert(std::is_trivially_copyable_v<LinearDepthCommConfig>);

    /// LinearDepth feature factory — registered at runtime via RegisterFeatureType.

} // namespace lux::render
