#pragma once
// =============================================================================
//  LglslEmitter.hpp  —  .lglsl -> canonical GLSL emitter
// -----------------------------------------------------------------------------
//  .lglsl is the engine's shader source format: the logic sections are
//  identical to GLSL; resource declarations are written by the author as
//  [type + name] only, without a layout(set/binding) prefix -- this emitter
//  injects the canonical values for the slot according to the LayoutContract
//  (the description layer, the single source of truth for layout).
//
//  Self-describing pragmas at the top of the file (the source of truth the
//  build manifest and the C++ shader class reconcile against):
//      //! lux-shader stage=fragment entry=main
//      //! lux-variant USE_EVSM
//
//  Parsing is convention-based: it only recognizes top-level resource
//  declaration shapes (qualifier* uniform|buffer ...); lines that already
//  carry a layout(...) (push constant / in / out / explicit declarations)
//  pass through unchanged; any declaration it doesn't recognize, or any
//  resource name not registered in the contract table, is always an error
//  (with a line number) -- it never guesses.
//
//  This component is a pure text transform with zero heavyweight
//  dependencies (no shaderc / no SPIR-V) -- it can run at runtime for
//  projects that opt in to procedurally generating shaders; the offline path
//  is invoked through the thin CLI wrapper lux_shader_emitter
//  (engine/asset_pipeline).
//
//  Design doc: .internal/lux-engine-descriptor-layout-architecture.md §4.4
// =============================================================================

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/description/ShaderInfo.hpp>   // rdesc::EShaderType

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lux::shadergen::lglsl
{
    /// One variant-domain axis (v1: a boolean macro domain; the name is
    /// exactly the -D macro name used at offline compile time).
    struct ShaderVariantDesc
    {
        std::string name;
    };

    /// The parsed result of the file-header pragmas.
    struct ShaderMeta
    {
        rdesc::EShaderType stage{rdesc::EShaderType::UNDEFINED};
        std::string        entry{"main"};
        std::vector<ShaderVariantDesc> variants;
    };

    /// A resource declaration that had a slot injected into it (for
    /// diagnostics / reconciling against reflection).
    struct InjectedBinding
    {
        std::string name;
        uint32_t    set{0};
        uint32_t    binding{0};
    };

    struct EmitOutput
    {
        std::string glsl;   ///< the complete GLSL with canonical layout already injected (can be fed directly to glslc)
        ShaderMeta  meta;
        std::vector<InjectedBinding> injected;
    };

    /// Emit mode. Shader = a complete shader (must have a lux-shader pragma);
    /// Header = a shared .lglslh header (only does declaration injection; a
    /// lux-shader pragma here is actually an error -- a header doesn't own a
    /// stage, the stage belongs to the shader that includes it).
    enum class EEmitMode : uint8_t
    {
        Shader,
        Header,
    };

    /// .lglsl/.lglslh source text -> canonical GLSL. Error messages are
    /// prefixed with "line N: "; an unrecognized declaration shape or an
    /// unregistered resource name both fail; the pragma requirements depend
    /// on mode (see above).
    lux::cxx::expected<EmitOutput, std::string>
    emitCanonicalGlsl(std::string_view source, EEmitMode mode = EEmitMode::Shader);

} // namespace lux::shadergen::lglsl
