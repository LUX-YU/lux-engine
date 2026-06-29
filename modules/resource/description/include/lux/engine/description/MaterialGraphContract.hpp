#pragma once
// =============================================================================
//  MaterialGraphContract.hpp  —  M0 契约（提案，待评审 2026-06-07）
// -----------------------------------------------------------------------------
//  材质图系统的【单一真相源】。定义三件事：
//    (a) 表面属性 (MaterialAttributes) —— 图的 OutputSurface 节点产出的内容；
//    (b) 着色输入 (MaterialInputs)     —— 通道骨架喂给 evalSurface() 的插值/几何量；
//    (c) 着色模型选择器 (ShadingModel) —— 复用 description 的 ELightingTechnique。
//
//  material_graph（数据模型）与 material_graph_compiler（MLIR）都引用本头；
//  GLSL 的 `MaterialAttributes` struct 与各通道骨架由 codegen 据此生成 / 保持同步。
//
//  放在 description 层（在 render 之下）：material_graph / compiler 因此无需依赖
//  render-runtime；render-runtime 也只通过 ShadingModelRegistry 消费烘焙产物。
//
//  详见 .internal/plan/material-graph-mlir-implementation-guide.md（§3.1 / §11）。
// =============================================================================

#include "MaterialEnums.hpp"   // 复用 ELightingTechnique 作为着色模型选择器（删 Material.hpp 后仍存）
#include <cstddef>
#include <cstdint>

namespace lux::rdesc
{
    /**
     * @brief 图中一个值 / 属性 / 输入的标量-向量类型。
     *        M0 先支持这四种；矩阵 / 整数等按需在末尾追加。
     */
    enum class EMatValueType : uint8_t
    {
        Float,
        Vec2,
        Vec3,
        Vec4
    };

    /**
     * @brief 规范表面输出（PBR 超集；覆盖 Unlit / PBR / Stylized，LegacyLit 映射其上）。
     *
     * 扩展规则（硬约束）：**只许在 COUNT 前追加 + 提供默认值**；
     * 绝不重排 / 改义 —— 它同时是 codegen 缓存键与生成结构的 ABI。
     */
    enum class EMaterialAttribute : uint8_t
    {
        BaseColor = 0,     ///< Vec3  反照率 / unlit 颜色      默认 (1,1,1)
        Opacity,           ///< Float alpha                    默认 1
        Metallic,          ///< Float                          默认 0
        Roughness,         ///< Float                          默认 1
        NormalTS,          ///< Vec3  切线空间法线             默认 (0,0,1)
        Emissive,          ///< Vec3  自发光                   默认 (0,0,0)
        AmbientOcclusion,  ///< Float 环境光遮蔽               默认 1
        COUNT
    };

    struct MaterialAttributeDesc
    {
        EMaterialAttribute attribute;
        const char*        glsl_name;  ///< 生成的 MaterialAttributes struct 中的字段名
        EMatValueType      type;
        float              dflt[4];    ///< 默认常量（未用分量填 0）
    };

    inline constexpr MaterialAttributeDesc kMaterialAttributes[] = {
        { EMaterialAttribute::BaseColor,        "base_color", EMatValueType::Vec3,  { 1, 1, 1, 0 } },
        { EMaterialAttribute::Opacity,          "opacity",    EMatValueType::Float, { 1, 0, 0, 0 } },
        { EMaterialAttribute::Metallic,         "metallic",   EMatValueType::Float, { 0, 0, 0, 0 } },
        { EMaterialAttribute::Roughness,        "roughness",  EMatValueType::Float, { 1, 0, 0, 0 } },
        { EMaterialAttribute::NormalTS,         "normal_ts",  EMatValueType::Vec3,  { 0, 0, 1, 0 } },
        { EMaterialAttribute::Emissive,         "emissive",   EMatValueType::Vec3,  { 0, 0, 0, 0 } },
        { EMaterialAttribute::AmbientOcclusion, "ao",         EMatValueType::Float, { 1, 0, 0, 0 } },
    };

    static_assert(
        sizeof(kMaterialAttributes) / sizeof(kMaterialAttributes[0])
            == static_cast<size_t>(EMaterialAttribute::COUNT),
        "kMaterialAttributes table must stay in sync with EMaterialAttribute"
    );

    /**
     * @brief 通道骨架提供给图 evalSurface() 的着色输入。
     *
     * 资源访问（set 2 bindless 纹理、set 4 材质参数）走专用节点，不在此列举。
     * 扩展同样【只许末尾追加】（如 UV1 / ViewDir）。
     */
    enum class EMaterialInput : uint8_t
    {
        UV0 = 0,        ///< Vec2
        WorldPosition,  ///< Vec3
        WorldNormal,    ///< Vec3
        WorldTangent,   ///< Vec4 (xyz + 手性 w)
        VertexColor,    ///< Vec4
        COUNT
    };

    struct MaterialInputDesc
    {
        EMaterialInput input;
        const char*    glsl_name;
        EMatValueType  type;
    };

    inline constexpr MaterialInputDesc kMaterialInputs[] = {
        { EMaterialInput::UV0,           "uv0",            EMatValueType::Vec2 },
        { EMaterialInput::WorldPosition, "world_position", EMatValueType::Vec3 },
        { EMaterialInput::WorldNormal,   "world_normal",   EMatValueType::Vec3 },
        { EMaterialInput::WorldTangent,  "world_tangent",  EMatValueType::Vec4 },
        { EMaterialInput::VertexColor,   "vertex_color",   EMatValueType::Vec4 },
    };

    static_assert(
        sizeof(kMaterialInputs) / sizeof(kMaterialInputs[0])
            == static_cast<size_t>(EMaterialInput::COUNT),
        "kMaterialInputs table must stay in sync with EMaterialInput"
    );

    /**
     * @brief codegen 把 evalSurface() 包进的通道骨架。
     *        Forward / GBuffer 先做（M3）；Shadow 平凡；
     *        VisBuffer 是 Nanite(T2) 交汇点 —— 见 nanite-implementation-guide.md。
     */
    enum class EMaterialPass : uint8_t
    {
        Forward = 0,
        GBuffer,
        Shadow,
        VisBuffer,
        COUNT
    };

    /// 着色模型选择器 = 复用 description 既有的 ELightingTechnique
    /// (Unlit / LegacyLit / PbrMetallicRoughness / Stylized)。
    using EMaterialShadingModel = ELightingTechnique;

    /// 便捷查询。
    inline constexpr EMatValueType attributeType(EMaterialAttribute a) noexcept
    {
        return kMaterialAttributes[static_cast<size_t>(a)].type;
    }

    inline constexpr EMatValueType inputType(EMaterialInput i) noexcept
    {
        return kMaterialInputs[static_cast<size_t>(i)].type;
    }

} // namespace lux::rdesc
