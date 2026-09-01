#pragma once
// =============================================================================
//  Backend.hpp  --  ShaderGen GLSL backend: ShaderIR -> GLSL source -> SPIR-V (libshaderc)
// -----------------------------------------------------------------------------
//  Toolchain side (gated by LUX_ENABLE_SHADERGEN). The public
//  API leaks no shaderc/spirv-cross types: it consumes the backend-neutral
//  ShaderIR and produces GLSL text / SPIR-V words.
//  The runtime render module never links it -- it continues to consume only SPIR-V.
//
//  Shell parameters (EmitParams) = shading_model / pass / alpha -- things that
//  live outside the ShaderIR (they're shell concerns, not expressions); the
//  caller pulls these from material::LowerResult to fill it in.
//
//  Currently supported: the GBuffer pass (PBR / Unlit) and Forward Unlit.
//  Forward lighting (GGX/Toon), textures/params, TbnNormal, and RawExpr are
//  not yet implemented and will be added later (emitGlsl fails explicitly for them).
// =============================================================================

#include <cstdint>
#include <string>
#include <vector>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/toolchain/shader/ShaderIR.hpp>
#include <lux/engine/description/MaterialEnums.hpp>          // rdesc::EAlphaMode
#include <lux/engine/description/ShaderInfo.hpp>             // CompiledShader holds a ShaderInfo value

namespace lux::shadergen::glsl
{
    enum class EMaterialPass : std::uint8_t
    {
        FORWARD = 0,
        GBUFFER,
        SHADOW,
        VIS_BUFFER
    };

    /// Shell parameters: determine which pass shell / shading-model branch / alpha
    /// behavior to generate. Sourced from material lowering's LowerResult (these
    /// live outside the ShaderIR -- they're shell concerns, not expressions).
    struct EmitParams
    {
        EMaterialPass pass{EMaterialPass::GBUFFER};
        ::lux::rdesc::ELightingTechnique shading_model{::lux::rdesc::ELightingTechnique::PbrMetallicRoughness};
        ::lux::rdesc::EAlphaMode alpha_mode{::lux::rdesc::EAlphaMode::Opaque};
        float alpha_cutoff{0.5F};
    };

    /// Compilation output: SPIR-V words plus this shader's ShaderInfo (descriptor layout for set2/set4).
    struct CompiledShader
    {
        std::vector<uint32_t>    spirv;
        ::lux::rdesc::ShaderInfo info;
    };

    /// Emits GLSL fragment source (pure: no shaderc, no device, unit-testable).
    /// Returns the source string on success; returns an error string on failure
    /// (e.g. for the Shadow/VisBuffer pass).
    lux::cxx::expected<std::string, std::string>
    emitGlsl(const ShaderIR& ir, const EmitParams& params);

    /// Runs emitGlsl and then compiles the result to SPIR-V via libshaderc (target
    /// env vulkan1.2). Wires up an IncluderInterface: `#include "gbuffer_encode.glsl"`
    /// and similar directives in the shell are resolved via `include_dirs` (supplied
    /// by the caller, typically render's assets/shaders directory) -- so this
    /// component has no dependency on render. Returns CompiledShader on success
    /// (SPIR-V plus ShaderInfo: textures map to set2, tex|param map to set4);
    /// returns an error string on failure.
    lux::cxx::expected<CompiledShader, std::string>
    compileToSpirv(const ShaderIR&                 ir,
                   const EmitParams&               params,
                   const std::vector<std::string>& include_dirs);

} // namespace lux::shadergen::glsl
