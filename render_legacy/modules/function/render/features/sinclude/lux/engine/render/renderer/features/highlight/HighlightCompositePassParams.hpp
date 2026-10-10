#pragma once
// =============================================================================
//  HighlightCompositePassParams.hpp — 双绑定 + 混合管线的 PassParams 作者头
// -----------------------------------------------------------------------------
//  生成机制与孪生关系同 HighlightBlurPassParams.hpp(那边的文件头是全套说明,
//  不重复)。本头额外验收的点:**多 read 字段的声明序 = glslh uniform 声明序
//  = 瞬态 DS 绑定序**三序同构 —— uBlur 必须先于 uMask(与
//  highlight_composite.frag.lglsl 原手写声明序一致,strip 后 SPIR-V 等值的
//  前提)。
// =============================================================================
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/function/render/graph/RGForwardDecls.hpp>

#include <vulkan/vulkan.h>

namespace lux::render
{
    /// 推送常量标量段([8,24)):光晕颜色与强度。
    struct LUX_PASS_SCALARS() HighlightCompositeScalars
    {
        float color_r{1.0f};   ///< 光晕颜色 R
        float color_g{0.6f};   ///< 光晕颜色 G
        float color_b{0.1f};   ///< 光晕颜色 B
        float intensity{1.0f}; ///< 光晕 alpha 倍率(着色器内 clamp 到 1)
    };

    /// 外光晕合成的资源面:模糊场 + 锐利遮罩两个采样输入,SceneColor 输出。
    /// 字段声明序即绑定序:uBlur=b0、uMask=b1。
    struct LUX_PASS_PARAMS() HighlightCompositePassParams
    {
        LUX_RESOURCE(role = read, glsl = uBlur) RGResourceHandle blur {};
        LUX_RESOURCE(role=sampler, for=blur) VkSampler        blur_sampler{VK_NULL_HANDLE};
        LUX_RESOURCE(role = read, glsl = uMask) RGResourceHandle mask {};
        LUX_RESOURCE(role=sampler, for=mask) VkSampler        mask_sampler{VK_NULL_HANDLE};
        LUX_RESOURCE(role = write) RGResourceHandle color_out {};

        HighlightCompositeScalars scalars{};
    };
} // namespace lux::render
