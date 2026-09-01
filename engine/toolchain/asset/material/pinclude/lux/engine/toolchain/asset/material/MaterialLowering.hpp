#pragma once
// =============================================================================
//  MaterialLowering.hpp — Client A (material): rdesc::MaterialGraph -> ShaderIR
// -----------------------------------------------------------------------------
//  ShaderGen's first client. Lowers an authored material graph into a pure
//  expression ShaderIR:
//    - 7 surface attributes -> named outputs ("base_color"/"metallic"/...)
//    - shading inputs (EMaterialInput) -> ShaderIR.inputs slots
//    - texture/param slots  -> ShaderIR.textures / params
//  The shading model and PSO render state are NOT part of the ShaderIR —
//  they aren't "expressions" — and are carried out separately in this result
//  for the baking bridge to consume (see LowerResult). The algorithm borrows
//  the iterative DFS used by lux::matgraph::lowerToIR.
// =============================================================================

#include <string>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/material/graph/MaterialGraph.hpp>  // rdesc::MaterialGraph
#include <lux/engine/toolchain/shader/ShaderIR.hpp>

namespace lux::shadergen::material
{
    /// Result of material lowering: a pure expression ShaderIR, plus the
    /// shading model and PSO render state that are stripped out of the IR
    /// and consumed by the baking bridge separately (they aren't
    /// "expressions", so they don't belong in ShaderIR).
    struct LowerResult
    {
        ShaderIR ir;
        ::lux::rdesc::EMaterialShadingModel shading_model =
            ::lux::rdesc::EMaterialShadingModel::PbrMetallicRoughness;
        ::lux::rdesc::EAlphaMode alpha_mode = ::lux::rdesc::EAlphaMode::Opaque;
        float                    alpha_cutoff = 0.5f;
        bool                     double_sided = false;

        /// The final shader cache key = ir.fingerprint combined with
        /// shading_model + alpha_mode + (when Mask) alpha_cutoff — the latter
        /// change the emitted SPIR-V (shading_model selects the BRDF/GBuffer
        /// encoding, alpha decides whether to discard) even though they are
        /// not part of ShaderIR.
        /// Consumers/cache layers MUST use this combined key, never the bare
        /// ir.fingerprint — otherwise two graphs with identical expressions
        /// but different shading_model or alpha would wrongly reuse the same
        /// compiled artifact. double_sided is pure PSO state that does not
        /// change the SPIR-V, so it deliberately does NOT go into this key
        /// (the render side folds it into its own bucket key instead).
        uint64_t combined_fingerprint = 0;
    };

    /// Lowers an authored material graph into a pure expression ShaderIR,
    /// plus the shading model / PSO state carried out alongside it.
    /// Returns a LowerResult on success; on failure returns an error string
    /// (a cycle, a required output left unbound, or a type mismatch).
    lux::cxx::expected<LowerResult, std::string>
    lowerMaterial(const ::lux::material::MaterialGraph& graph);

} // namespace lux::shadergen::material
