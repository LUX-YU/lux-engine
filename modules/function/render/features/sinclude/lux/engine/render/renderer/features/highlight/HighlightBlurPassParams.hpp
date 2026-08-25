#pragma once
// =============================================================================
//  HighlightBlurPassParams.hpp — 首个真实的 PassParams 作者头
// -----------------------------------------------------------------------------
//  这一个文件就是特性作者写的全部:资源角色 + 标量段。构建钟由此生成两份孪生
//  (engine_add_pass_params,模板见 modules/core/meta/template/):
//    pass_gen/HighlightBlurPassParams.pass.hpp   PC 布局断言 + 图 I/O +
//                                                瞬态 DS + 推送常量
//    pass_gen/HighlightBlurPassParams.lglslh     uBlurSrc 声明 + PC 块
//  后者经 emit --header 注入 set/binding 后,以
//  `#include "HighlightBlurPassParams.glsl"` 的名字被 highlight_blur.frag.lglsl
//  消费 —— C++/GLSL 两侧自此同源,原先三份手写布局副本(GLSL 块、recorder
//  匿名结构、管线 PC 尺寸字面量 20)全部退役。
//
//  约束(v0,模板 #error 强制):scalars 结构必须与本结构同头文件;一头一
//  params 结构;标量白名单 float/uint/int;read 字段必须有配对 sampler 字段。
//  不做:不接编辑器面板(模糊方向/半径由特性代码按 H/V 趟次写死,非用户
//  可调项；若未来要调，给 scalars 结构补 runtime TypeInfo 并入 module meta 列表）。
// =============================================================================
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/function/render/graph/RGForwardDecls.hpp>

#include <vulkan/vulkan.h>

namespace lux::render
{
    /// 推送常量标量段([8,20)):模糊轴与半径。字节布局即 GLSL PC 块尾段,
    /// 由生成的 .pass.hpp 逐偏移 static_assert 看住。
    struct LUX_PASS_SCALARS() HighlightBlurScalars
    {
        float dir_x{1.0f};    ///< 模糊轴 x —— H 趟 (1,0),V 趟 (0,1)
        float dir_y{0.0f};    ///< 模糊轴 y
        float radius{1.0f};   ///< 每 tap 步长倍率(texel)
    };

    /// 分离高斯模糊单趟的资源面:一进(采样)一出(色附件)。
    struct LUX_PASS_PARAMS() HighlightBlurPassParams
    {
        LUX_RESOURCE(role=read, glsl=uBlurSrc)   RGResourceHandle blur_src{};
        LUX_RESOURCE(role=sampler, for=blur_src) VkSampler        blur_src_sampler{VK_NULL_HANDLE};
        LUX_RESOURCE(role=write)                 RGResourceHandle color_out{};

        HighlightBlurScalars scalars{};
    };
} // namespace lux::render
