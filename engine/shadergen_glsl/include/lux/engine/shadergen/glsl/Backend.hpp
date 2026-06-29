#pragma once
// =============================================================================
//  Backend.hpp  —  ShaderGen GLSL 后端：ShaderIR -> GLSL 源 -> SPIR-V (libshaderc)
// -----------------------------------------------------------------------------
//  editor/cook 侧（engine 层，LUX_ENABLE_SHADERGEN 门控）。公开 API 不泄漏任何
//  shaderc/spirv-cross 类型：吃后端中性的 ShaderIR，吐 GLSL 文本 / SPIR-V words。
//  runtime render 永不链接它——继续只消费 SPIR-V。
//
//  外壳参数（EmitParams）= shading_model / pass / alpha 这些【不在 ShaderIR】的东西
//  （它们是外壳，不是表达式）；调用方从 material::LowerResult 取来填。
//
//  M1 范围：GBuffer pass（PBR / Unlit）+ Forward Unlit。Forward 光照（GGX/Toon）、
//  textures/params、TbnNormal、RawExpr 在后续里程碑接入（emitGlsl 对它们明确报错）。
// =============================================================================

#include <cstdint>
#include <string>
#include <vector>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/shadergen/ShaderIR.hpp>
#include <lux/engine/description/MaterialGraphContract.hpp>  // rdesc::EMaterialPass / EMaterialShadingModel
#include <lux/engine/description/MaterialEnums.hpp>          // rdesc::EAlphaMode
#include <lux/engine/description/ShaderInfo.hpp>             // CompiledShader 含 ShaderInfo 值
#include <lux/engine/editor/visibility.h>

namespace lux::shadergen::glsl
{
    /// 外壳参数：决定生成哪种 pass 外壳 / 着色模型分支 / alpha 行为。来自材质
    /// lowering 的 LowerResult（这些不在 ShaderIR，是外壳而非表达式）。
    struct EmitParams
    {
        ::lux::rdesc::EMaterialPass          pass          = ::lux::rdesc::EMaterialPass::GBuffer;
        ::lux::rdesc::EMaterialShadingModel  shading_model = ::lux::rdesc::EMaterialShadingModel::PbrMetallicRoughness;
        ::lux::rdesc::EAlphaMode             alpha_mode    = ::lux::rdesc::EAlphaMode::Opaque;
        float                                alpha_cutoff  = 0.5f;
    };

    /// 编译产物：SPIR-V words + 该 shader 的 ShaderInfo（描述符布局 set2/set4）。
    struct CompiledShader
    {
        std::vector<uint32_t>    spirv;
        ::lux::rdesc::ShaderInfo info;
    };

    /// 发射 GLSL fragment 源（纯：无 shaderc、无设备，可单测）。成功返回源串；失败
    /// 返回错误串（如 Shadow/VisBuffer pass）。
    LUX_EDITOR_PUBLIC lux::cxx::expected<std::string, std::string>
    emitGlsl(const ShaderIR& ir, const EmitParams& params);

    /// emitGlsl + libshaderc 编译成 SPIR-V（target env vulkan1.2）。配 IncluderInterface：
    /// 外壳里的 #include "gbuffer_encode.glsl" 等由 `include_dirs` 解析（调用方传入，
    /// 通常含 render 的 assets/shaders 目录）——本组件因此不依赖 render。成功返回
    /// CompiledShader（SPIR-V + ShaderInfo：textures→set2、tex|param→set4）；失败返回错误串。
    LUX_EDITOR_PUBLIC lux::cxx::expected<CompiledShader, std::string>
    compileToSpirv(const ShaderIR&                 ir,
                   const EmitParams&               params,
                   const std::vector<std::string>& include_dirs);

} // namespace lux::shadergen::glsl
