#pragma once
// =============================================================================
//  ShaderIR.hpp  —  backend-neutral shader IR (the core boundary of the ShaderGen subsystem)
// -----------------------------------------------------------------------------
//  [Architectural invariant] A plain data structure -- not an mlir::Operation,
//  not tied to any backend. Clients (materials / GBuffer layout / lighting
//  models) each lower their own declarations into ShaderIR; the backend in
//  engine/ (lux::shadergen::glsl) consumes ShaderIR to generate GLSL -> SPIR-V.
//  modules/ has no dependency on shaderc.
//
//  [Pure expressions] ShaderIR only carries the data flow of "how to compute
//  named outputs from inputs" -- math, sampling, inputs/params, construction,
//  escape hatches. It does NOT contain shading_model (which BRDF) or
//  render_state (PSO state such as alpha/double_sided) -- those aren't
//  "expressions"; they belong to each client's own declaration and are
//  carried separately by the baking bridge (see LowerResult in
//  material/MaterialLowering.hpp).
//
//  [Generalization pivot] Output is a generic named binding ("base_color" /
//  "g_normal" / "radiance") that makes no assumption about whether it's a
//  material property, a GBuffer channel, or a color -- so the same IR/Emitter
//  can be reused across all three clients. See
//  .internal/plan/shadergen-design.md for the design and rationale.
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <lux/engine/description/MaterialGraphContract.hpp>  // reuses the neutral scalar/vector types

namespace lux::shadergen
{
    /// Scalar/vector type. Reuses description's neutral enum (the "Mat" in the
    /// name is historical; the semantics are generic).
    /// shadergen code always uses the semantically-neutral alias EValueType,
    /// never the rdesc name directly.
    using EValueType = ::lux::rdesc::EMatValueType;

    /// Backend-neutral SSA op set. Pure expressions: math + sampling +
    /// inputs/params + construction + escape hatches.
    /// **APPEND-ONLY** (participates in the cache fingerprint): only ever
    /// append at the end -- never reorder or change the meaning of an entry.
    enum class EOp : uint16_t
    {
        Constant,      ///< a literal (constant[])
        Input,         ///< read a named input slot (slot -> inputs[])
        SampleTexture, ///< sample a texture slot (slot -> textures[], operands[0]=uv)
        Param,         ///< read a param slot (slot -> params[])
        Mul, Add, Sub, Div, Lerp, Saturate, Dot, Min, Max,
        Swizzle,       ///< component reshuffle (swizzle[])
        Construct,     ///< vecN(...)
        DecodeNormal,  ///< normal-map rgb -> tangent-space normal
        Pow, Step, Mod, Cross, Reflect, OneMinus, Abs, Sqrt, Floor, Fract,
        Sin, Cos, Normalize, Length, TbnNormal,
        RawExpr,       ///< escape hatch: a backend-specific raw shader fragment (slot -> raw_blocks[], see §6.5)
    };

    inline constexpr uint32_t kNoValue = ~0u;

    /// A single SSA value. operands are indices into values[] (kNoValue = unused).
    struct ShaderIRValue
    {
        EOp        op;
        EValueType type;
        uint32_t   operands[4] = { kNoValue, kNoValue, kNoValue, kNoValue };

        // payload (interpreted according to op):
        float    constant[4] = { 0, 0, 0, 0 };  ///< Constant
        uint32_t slot        = 0;               ///< the slot index for Input/SampleTexture/Param/RawExpr
        uint8_t  swizzle[4]  = { 0, 1, 2, 3 };  ///< Swizzle
    };

    /// Interpolation qualifier for a shading-stage input.
    enum class EInterpolation : uint8_t { Smooth, Flat };

    /// A named input slot. Its semantics are defined by the client (for
    /// materials: uv0/world_normal/...); shadergen itself makes no assumption
    /// about what an input means. **Self-contained**: carries everything the
    /// emitter needs to declare `layout(location=N) in <type> <name>`
    /// (location + interpolation qualifier), so the emitter never has to look
    /// back at the client's own enum.
    /// location == -1 means unspecified (left for ShellTemplate to decide).
    struct InputSlot
    {
        std::string    name;
        EValueType     type          = EValueType::Float;
        int32_t        location      = -1;
        EInterpolation interpolation = EInterpolation::Smooth;
    };

    /// A set2 bindless sampler slot.
    struct TextureSlot
    {
        std::string name;
    };

    /// A set4 per-material SSBO parameter slot.
    struct ParamSlot
    {
        std::string name;
        EValueType  type = EValueType::Float;
        float       dflt[4] = { 0, 0, 0, 0 };
    };

    /// A backend-specific fragment for the RawExpr escape hatch (see §6.5).
    /// The emitter inlines it only when `language` matches the current
    /// backend; otherwise the graph explicitly errors out on that backend
    /// (never silently misbehaves).
    ///
    /// Wiring convention (locked down to avoid M3 breaking ABI): the
    /// ShaderIRValue that carries a RawExpr reuses existing fields --
    ///   - inputs = ShaderIRValue.operands[0..3]; the code references their
    ///     GLSL expressions as `$0`..`$3`;
    ///   - output type = ShaderIRValue.type.
    /// So RawBlock only stores the language + text; all the wiring lives on
    /// ShaderIRValue, with no extra structural fields needed.
    struct RawBlock
    {
        std::string language;  ///< "glsl" / "hlsl" / ...
        std::string code;      ///< the fragment text, $0..$3 = the operands' expressions
    };

    /// A named output binding. Generic -- makes no assumption about whether
    /// it's a material property / GBuffer channel / color. The pivot that
    /// lets the same IR/Emitter be reused across all three clients.
    /// **Addressed by name -- the order of outputs[] carries no meaning.**
    /// value_id == kNoValue => use dflt (the default is self-contained in the
    /// IR, so a consumer can restore it without looking back at any client
    /// contract).
    struct Output
    {
        std::string name;
        uint32_t    value_id = kNoValue;
        EValueType  type     = EValueType::Float;
        float       dflt[4]  = { 0, 0, 0, 0 };  ///< the default used when value_id==kNoValue (goes straight into a SPIR-V literal)
    };

    /// Backend-neutral IR for one synthesized piece of shading logic. Pure
    /// expressions (no shading_model / render_state).
    struct ShaderIR
    {
        std::vector<ShaderIRValue> values;     ///< SSA values in topological order
        std::vector<Output>        outputs;    ///< named output bindings
        std::vector<InputSlot>     inputs;     ///< named input slots
        std::vector<TextureSlot>   textures;   ///< set2 bindless sampler slots
        std::vector<ParamSlot>     params;     ///< set4 SSBO parameter slots
        std::vector<RawBlock>      raw_blocks; ///< RawExpr text

        /// The codegen cache key is data derived from computeFingerprint(*this).
        /// Designed from scratch, decoupled from rdesc's
        /// EMaterialAttribute/EMatIROp ABI. Note: this fingerprint alone only
        /// fingerprints the expressions themselves, not shading_model /
        /// render_state (they aren't part of the IR); the final shader cache
        /// key is assembled by the client combining in those shell parameters
        /// (for materials, see material::LowerResult::combined_fingerprint).
        uint64_t fingerprint = 0;
    };

    /// A pure-function fingerprint of a ShaderIR's contents (a single rule
    /// reused by every client, to avoid drift). lowering calls this at the
    /// end and writes the result back into ir.fingerprint. If you construct
    /// an IR by hand, you must call this too, or fingerprint is left at its
    /// uncomputed 0.
    [[nodiscard]] uint64_t computeFingerprint(const ShaderIR& ir) noexcept;

} // namespace lux::shadergen
