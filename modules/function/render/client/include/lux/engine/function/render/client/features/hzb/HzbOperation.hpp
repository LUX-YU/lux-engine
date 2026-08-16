#pragma once
// ============================================================================
//  HzbOperation.hpp — Hzb 通信外观的【作者声明】(A+ 扫尾)
//  无特性局部命令、无创建参数 —— 空 tag 承载身份,整个 Operation 面
//  (含默认 Config 的 createFn)全生成。
// ============================================================================
#include <lux/engine/meta/MetaAnnotations.hpp>

#include <type_traits>

namespace lux::render
{
    /// Factory identity for HzbFeature (P2 HZB Stage A).
    struct LUX_COMM_CONFIG(
        prefix=Hzb, id=lux.render.hzb.v1, display=Hzb,
        feature=HzbFeature,
        feature_header=lux/engine/render/renderer/features/hzb/HzbFeature.hpp)
    HzbCommTag
    {
    };
    static_assert(std::is_trivially_copyable_v<HzbCommTag>);
}
