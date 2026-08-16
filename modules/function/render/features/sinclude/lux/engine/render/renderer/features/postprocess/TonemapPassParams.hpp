#pragma once
// =============================================================================
//  TonemapPassParams.hpp — Tonemap 的 PassParams 作者头(已投产)
// -----------------------------------------------------------------------------
//  历史:本文件曾是 L2-0「手写生成物原型」——后半段伪装成生成代码的
//  tonemap_pass_params_gen 命名空间,用来在写模板前证伪目标形状(f87be58,
//  四问三绿)。模板落地(147f5e2)并经 Highlight 验收(61722c3)后,那一段
//  连同手写的 tonemap_params.lglslh 一并退役:现在两份孪生由本头生成
//  (pass_gen/TonemapPassParams.pass.hpp + TonemapPassParams.lglslh),
//  注释预告的 static_assert 退休兑现 —— 同源生成后对账自动成立。
//
//  标量段直接复用编辑器反射结构 TonemapParams(它在自己的安装头里,
//  依赖保持轻;LUX_PASS_SCALARS 双标记加在那边)。跨头可见靠
//  PARSE_INCLUDED_MARKED(lux-cxx 选项:放行被 include 的标记声明)——
//  「一份声明,多处受益」不再要求挤进同一个文件。
// =============================================================================
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/function/render/graph/RGForwardDecls.hpp>
#include <lux/engine/function/render/client/features/postprocess/TonemapParams.hpp>

#include <vulkan/vulkan.h>

namespace lux::render
{
    /// 色调映射全屏 pass 的资源面:HDR 输入(采样)→ LDR 输出(色附件)。
    struct LUX_PASS_PARAMS() TonemapPassParams
    {
        LUX_RESOURCE(role=read, glsl=uHDRColor)   RGResourceHandle hdr_color{};
        LUX_RESOURCE(role=sampler, for=hdr_color) VkSampler        hdr_sampler{VK_NULL_HANDLE};
        LUX_RESOURCE(role=write)                  RGResourceHandle color_out{};

        /// 标量段 = 推送常量 [8,20);同一份结构喂编辑器面板与 comm 载荷。
        TonemapParams scalars{};
    };
} // namespace lux::render
