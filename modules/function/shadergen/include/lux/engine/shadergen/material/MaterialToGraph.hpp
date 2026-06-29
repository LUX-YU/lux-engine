#pragma once
// =============================================================================
//  MaterialToGraph.hpp  —  把导入的扁平材质描述转成 MaterialGraph
// -----------------------------------------------------------------------------
//  统一材质系统的转换器（W1/W5b）：让外部导入的内建材质(PBR/Unlit/...)变成一张材质
//  图，从而能被材质图编辑器打开、并走统一的 Graph 渲染路径。
//
//  纯数据：只依赖 description（rdesc::ImportedMaterialDesc / MaterialGraph）。**不
//  依赖** asset/编译器 —— 纹理 UUID 的绑定是调用方(editor)在烘焙时按 texture_index
//  1:1 映射的事。ShaderGen 子系统的材质客户辅助（从退役的 material_graph 中性核迁入）。
// =============================================================================

#include <string>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/description/material_graph/MaterialGraph.hpp>  // expected<MaterialGraph> 需完整类型
#include <lux/engine/function/visibility.h>

namespace lux::rdesc { struct ImportedMaterialDesc; }

namespace lux::shadergen::material
{
    /// 把导入器产出的扁平 ImportedMaterialDesc POD 转成 MaterialGraph（PBR 图）。
    /// 图纹理槽 index == desc 的 `ImportedTextureRef::texture_index`（调用方按该序号
    /// 映射到 ModelAsset 的每材质纹理 UUID 列表再烘焙）。失败返回错误串（当前实现恒
    /// 成功；用 expected 统一非热路径的错误模式 + 为未来可失败的转换留出口）。
    LUX_FUNCTION_PUBLIC lux::cxx::expected<::lux::rdesc::MaterialGraph, std::string>
    materialToGraph(const ::lux::rdesc::ImportedMaterialDesc& desc);
} // namespace lux::shadergen::material
