#pragma once
// =============================================================================
//  MaterialLowering.hpp  —  客户 A（材质）：rdesc::MaterialGraph -> ShaderIR
// -----------------------------------------------------------------------------
//  ShaderGen 的第一个客户。把作者材质图 lower 成纯表达式 ShaderIR：
//    · 7 个 surface 属性  -> 命名 outputs（"base_color"/"metallic"/...）
//    · 着色输入 (EMaterialInput) -> ShaderIR.inputs 槽
//    · 纹理/参数槽        -> ShaderIR.textures / params
//  着色模型 + PSO 渲染状态【不进 ShaderIR】——它们不是"表达式"，由本结果单独带出
//  给烘焙桥（见 LowerResult）。算法借鉴 lux::matgraph::lowerToIR 的迭代式 DFS。
// =============================================================================

#include <string>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/description/material_graph/MaterialGraph.hpp>  // rdesc::MaterialGraph
#include <lux/engine/shadergen/ShaderIR.hpp>
#include <lux/engine/function/visibility.h>

namespace lux::shadergen::material
{
    /// 材质 lowering 的产物：纯表达式 ShaderIR + 从 IR 剥离、由烘焙桥消费的着色
    /// 模型 + PSO 渲染状态（它们不属于"表达式"，故不进 ShaderIR）。
    struct LowerResult
    {
        ShaderIR ir;
        ::lux::rdesc::EMaterialShadingModel shading_model =
            ::lux::rdesc::EMaterialShadingModel::PbrMetallicRoughness;
        ::lux::rdesc::EAlphaMode alpha_mode = ::lux::rdesc::EAlphaMode::Opaque;
        float                    alpha_cutoff = 0.5f;
        bool                     double_sided = false;

        /// 最终 shader 缓存键 = ir.fingerprint 再组合 shading_model + alpha_mode +
        /// (Mask 时) alpha_cutoff —— 后两者改变发射的 SPIR-V（shading_model 选 BRDF /
        /// GBuffer 编码，alpha 决定 discard），但不在 ShaderIR 里。
        /// 【消费方/缓存层必须用此键，不可用裸 ir.fingerprint】，否则表达式相同、
        /// shading_model 或 alpha 不同的两图会错误复用同一编译产物。double_sided 是
        /// 纯 PSO 状态、不改 SPIR-V，故【不】进此键（由 render 侧进 bucket key）。
        uint64_t combined_fingerprint = 0;
    };

    /// 把作者材质图 lower 成纯表达式 ShaderIR + 带出的着色模型/PSO 状态。
    /// 成功返回 LowerResult；失败返回错误串（环 / 必需输出未绑定 / 类型不符）。
    LUX_FUNCTION_PUBLIC lux::cxx::expected<LowerResult, std::string>
    lowerMaterial(const ::lux::rdesc::MaterialGraph& graph);

} // namespace lux::shadergen::material
