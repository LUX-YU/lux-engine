#pragma once
// ============================================================================
//  SsaoOperation.hpp — Ssao 通信外观的【作者声明】(参考实现)
//  无特性局部命令、无创建参数 —— 空 tag 承载身份,整个 Operation 面
//  (含默认 Config 的 createFn)全生成。装配依赖:需 LinearDepthFeature
//  同装(本特性消费 LinearDepth 槽;缺席时首帧读到未写内容)。
// ============================================================================
#include <lux/engine/meta/MetaAnnotations.hpp>

#include <type_traits>

namespace lux::render
{
    /// Factory identity for SsaoFeature (shading-input reference impl).
    struct LUX_COMM_CONFIG(
        prefix = Ssao,
        id = lux.render.ssao.v1,
        display = Ssao,
        feature = SsaoFeature,
        feature_header = lux / engine / render / renderer / features / postprocess / SsaoFeature.hpp) SsaoCommTag
    {
    };
    static_assert(std::is_trivially_copyable_v<SsaoCommTag>);
}
