#pragma once
// =============================================================================
//  ShaderIR.hpp  —  后端中性着色器 IR（ShaderGen 子系统的核心边界）
// -----------------------------------------------------------------------------
//  【架构不变量】普通数据结构，不是 mlir::Operation、不绑任何后端。客户（材质 /
//  GBuffer 布局 / 光照模型）各自把声明 lower 成 ShaderIR；engine/ 的后端
//  (lux::shadergen::glsl) 消费 ShaderIR 生成 GLSL→SPIR-V。modules/ 不依赖 shaderc。
//
//  【纯表达式】ShaderIR 只承载"如何从输入算出命名输出"的数据流——数学、采样、
//  输入/参数、结构、逃生舱。它【不含】shading_model（哪种 BRDF）、【不含】
//  render_state（alpha/double_sided 等 PSO 状态）——那些不是"表达式"，归各自的
//  声明，由烘焙桥另行携带（见 material/MaterialLowering.hpp 的 LowerResult）。
//
//  【泛化支点】Output 是通用命名绑定（"base_color" / "g_normal" / "radiance"），
//  不预设是材质属性还是 GBuffer 通道还是颜色——同一台 IR/Emitter 因此能复用给
//  三个客户。设计与决策见 .internal/plan/shadergen-design.md。
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <lux/engine/description/MaterialGraphContract.hpp>  // 复用中性标量/向量类型
#include <lux/engine/function/visibility.h>

namespace lux::shadergen
{
    /// 标量/向量类型。复用 description 的中性枚举（名字带 Mat 是历史，语义通用）。
    /// shadergen 代码一律用 EValueType 这个语义中性别名，不直呼 rdesc 名。
    using EValueType = ::lux::rdesc::EMatValueType;

    /// 后端中性 SSA 操作集。纯表达式：数学 + 采样 + 输入/参数 + 结构 + 逃生舱。
    /// **APPEND-ONLY**（参与缓存指纹）：只许在末尾追加，绝不重排/改义。
    enum class EOp : uint16_t
    {
        Constant,      ///< 字面量（constant[]）
        Input,         ///< 读具名输入槽（slot -> inputs[]）
        SampleTexture, ///< 采样纹理槽（slot -> textures[]，operands[0]=uv）
        Param,         ///< 读参数槽（slot -> params[]）
        Mul, Add, Sub, Div, Lerp, Saturate, Dot, Min, Max,
        Swizzle,       ///< 分量重排（swizzle[]）
        Construct,     ///< vecN(...)
        DecodeNormal,  ///< 法线贴图 rgb -> 切线空间法线
        Pow, Step, Mod, Cross, Reflect, OneMinus, Abs, Sqrt, Floor, Fract,
        Sin, Cos, Normalize, Length, TbnNormal,
        RawExpr,       ///< 逃生舱：后端特定原始着色器片段（slot -> raw_blocks[]，见 §6.5）
    };

    inline constexpr uint32_t kNoValue = ~0u;

    /// 一个 SSA 值。operands 是 values[] 的下标（kNoValue = 未用）。
    struct ShaderIRValue
    {
        EOp        op;
        EValueType type;
        uint32_t   operands[4] = { kNoValue, kNoValue, kNoValue, kNoValue };

        // payload（按 op 解释）：
        float    constant[4] = { 0, 0, 0, 0 };  ///< Constant
        uint32_t slot        = 0;               ///< Input/SampleTexture/Param/RawExpr 的槽下标
        uint8_t  swizzle[4]  = { 0, 1, 2, 3 };  ///< Swizzle
    };

    /// 着色阶段输入的插值限定。
    enum class EInterpolation : uint8_t { Smooth, Flat };

    /// 具名输入槽。客户定义其语义（材质 = uv0/world_normal/...）；shadergen 本身不
    /// 预设输入含义。**自包含**：携带 emitter 声明 `layout(location=N) in <type>
    /// <name>` 所需的全部信息（location + 插值限定），emitter 不必回查客户的枚举。
    /// location == -1 表示未指定（由 ShellTemplate 决定）。
    struct InputSlot
    {
        std::string    name;
        EValueType     type          = EValueType::Float;
        int32_t        location      = -1;
        EInterpolation interpolation = EInterpolation::Smooth;
    };

    /// set2 bindless 采样器槽。
    struct TextureSlot
    {
        std::string name;
    };

    /// set4 per-material SSBO 参数槽。
    struct ParamSlot
    {
        std::string name;
        EValueType  type = EValueType::Float;
        float       dflt[4] = { 0, 0, 0, 0 };
    };

    /// RawExpr 逃生舱的后端特定片段（见 §6.5）。emitter 仅在 language 匹配当前后端
    /// 时内联，否则该图在该后端明确报错（不静默出错）。
    ///
    /// 接线约定（钉死，避免 M3 破 ABI）：承载 RawExpr 的 ShaderIRValue 复用既有字段——
    ///   · 输入 = ShaderIRValue.operands[0..3]，code 内以 `$0`..`$3` 引用其 GLSL 表达式；
    ///   · 输出类型 = ShaderIRValue.type。
    /// 故 RawBlock 只存语言 + 文本，接线全在 ShaderIRValue 上，无需额外结构字段。
    struct RawBlock
    {
        std::string language;  ///< "glsl" / "hlsl" / ...
        std::string code;      ///< 片段文本，$0..$3 = operands 的表达式
    };

    /// 命名输出绑定。通用——不预设是材质属性 / GBuffer 通道 / 颜色。把同一台
    /// IR/Emitter 复用给三个客户的支点。**按 name 寻址，outputs[] 的顺序无语义。**
    /// value_id == kNoValue => 用 dflt（默认值自包含于 IR，消费方无需回查任何客户
    /// 契约即可还原默认）。
    struct Output
    {
        std::string name;
        uint32_t    value_id = kNoValue;
        EValueType  type     = EValueType::Float;
        float       dflt[4]  = { 0, 0, 0, 0 };  ///< value_id==kNoValue 时的默认（直入 SPIR-V 字面量）
    };

    /// 一段被合成的着色逻辑的后端中性 IR。纯表达式（无 shading_model / render_state）。
    struct ShaderIR
    {
        std::vector<ShaderIRValue> values;     ///< 拓扑序 SSA
        std::vector<Output>        outputs;    ///< 命名输出绑定
        std::vector<InputSlot>     inputs;     ///< 具名输入槽
        std::vector<TextureSlot>   textures;   ///< set2 bindless 采样槽
        std::vector<ParamSlot>     params;     ///< set4 SSBO 参数槽
        std::vector<RawBlock>      raw_blocks; ///< RawExpr 文本

        /// codegen 缓存键 = computeFingerprint(*this) 的派生数据。从零设计，与 rdesc 的
        /// EMaterialAttribute/EMatIROp ABI 解耦。注意：单独的 fingerprint 只指纹表达式
        /// 本身，不含 shading_model / render_state（它们不在 IR）；最终 shader 缓存键由
        /// 客户组合那些外壳参数（材质见 material::LowerResult::combined_fingerprint）。
        uint64_t fingerprint = 0;
    };

    /// ShaderIR 内容的纯函数指纹（单点规则，所有客户复用，避免漂移）。lowering 末尾
    /// 调它写回 ir.fingerprint。手工构造 IR 后也须调它，否则 fingerprint 是未算的 0。
    [[nodiscard]] LUX_FUNCTION_PUBLIC uint64_t computeFingerprint(const ShaderIR& ir) noexcept;

} // namespace lux::shadergen
