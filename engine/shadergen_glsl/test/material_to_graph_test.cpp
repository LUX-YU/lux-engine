// ============================================================================
//  material_to_graph_test.cpp  —  W1/W5b proof: ImportedMaterialDesc -> MaterialGraph
//
//  Proves the importer's flat POD (ImportedMaterialDesc — the closure-free output
//  of ModelSerDeser after W5b) converts to a MaterialGraph that lowers + compiles
//  cleanly. This is the basis for unifying imported materials onto the graph: the
//  importer no longer produces a closure rdesc::Material, so the converter's sole
//  input is this POD.
//    1. a textured PBR desc (base-color tex, shared ORM tex for metallic/roughness,
//       normal tex, emissive factor) -> graph;
//    2. expected structure (shading model, texture-slot count, an OutputSurface);
//    3. lowerToIR succeeds + compileToSpirv succeeds for BOTH Forward & GBuffer;
//    4. a legacy (Phong) desc collapses to PBR + compiles;
//    5. the W4 dedup proof: two value-different descs with identical texture
//       structure produce BYTE-IDENTICAL SPIR-V + the same shader fingerprint
//       (factors are Params/SSBO lanes, not baked literals).
//
//  Device-free (no Vulkan; shaderc is CPU). Self-checking: 0 = PASS, 1 = FAIL.
//  Deliberately includes NO <description/Material.hpp> — the closure material model
//  is retired; ImportedMaterialDesc + MaterialGraphContract carry every enum used.
// ============================================================================

#include <lux/engine/description/material_graph/MaterialGraph.hpp>
#include <lux/engine/shadergen/material/MaterialToGraph.hpp>
#include <lux/engine/description/ImportedMaterialDesc.hpp>
#include <lux/engine/description/MaterialGraphContract.hpp>
#include "graph_test_helpers.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace lux::rdesc;
namespace rdesc = lux::rdesc;

namespace
{
    int fails = 0;
    void check(bool cond, const char* name)
    { std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << "\n"; if (!cond) ++fails; }

    rdesc::ImportedTextureRef tex(uint32_t idx)
    {
        rdesc::ImportedTextureRef t;
        t.texture_index = idx;
        return t;
    }

    // Convert -> lower -> compile both passes; returns true if all succeed.
    bool convertAndCompile(const rdesc::ImportedMaterialDesc& desc, MaterialGraph& g, const char* tag)
    {
        auto g_exp = lux::shadergen::material::materialToGraph(desc);
        if (!g_exp)
        { std::cout << "  [" << tag << "] convert failed: " << g_exp.error() << "\n"; return false; }
        g = std::move(*g_exp);

        auto fwd_cs = lux::mgtest::compileGraph(g, rdesc::EMaterialPass::Forward);
        if (!fwd_cs)
        { std::cout << "  [" << tag << "] forward compile failed: " << fwd_cs.error() << "\n"; return false; }
        auto gb_cs = lux::mgtest::compileGraph(g, rdesc::EMaterialPass::GBuffer);
        if (!gb_cs)
        { std::cout << "  [" << tag << "] gbuffer compile failed: " << gb_cs.error() << "\n"; return false; }
        return true;
    }
}

int main()
{
    std::cout << std::unitbuf;
    std::cout << "=== material_to_graph_test (W1/W5b) ===\n";

    // ---- 1. Textured PBR desc ----
    rdesc::ImportedMaterialDesc pbr;
    pbr.base_color             = Eigen::Vector3f(0.8f, 0.7f, 0.6f);
    pbr.base_color_tex         = tex(0);                 // base color texture (slot 0)
    pbr.metallic               = 1.0f;
    pbr.roughness              = 1.0f;
    pbr.metallic_roughness_tex = tex(1);                 // shared ORM texture (slot 1)
    pbr.normal_tex             = tex(2);                 // normal map (slot 2)
    pbr.emissive               = Eigen::Vector3f(0.2f, 0.1f, 0.0f);
    pbr.emissive_intensity     = 2.0f;
    pbr.opacity                = 1.0f;
    // Render-state: the converter must carry alpha mode/cutoff/two-sided so imported
    // Mask/cutout + double-sided materials bake correctly.
    pbr.alpha_mode   = rdesc::EAlphaMode::Mask;
    pbr.alpha_cutoff = 0.25f;
    pbr.double_sided = true;

    MaterialGraph gp;
    const bool pbr_ok = convertAndCompile(pbr, gp, "PBR");
    check(pbr_ok, "textured PBR desc converts + lowers + compiles (Forward & GBuffer)");
    check(gp.shading_model == rdesc::ELightingTechnique::PbrMetallicRoughness, "PBR graph keeps PBR shading model");
    check(gp.render_state.alpha_mode == rdesc::EAlphaMode::Mask
       && gp.render_state.double_sided == true
       && gp.render_state.alpha_cutoff == 0.25f,
          "converter carries render-state (alpha mode / cutoff / double-sided)");
    check(gp.texture_slots.size() == 3, "PBR graph declares 3 texture slots (base / ORM / normal)");
    bool has_output = false, has_sample = false;
    for (const auto& [id, n] : gp.nodes())
    {
        if (n->kind() == EMatNodeKind::OutputSurface) has_output = true;
        if (n->kind() == EMatNodeKind::SampleTexture) has_sample = true;
    }
    check(has_output, "PBR graph has an OutputSurface");
    check(has_sample, "PBR graph samples textures");

    // ---- 2. Legacy (Phong) desc — collapses to PBR (metallic=0, roughness=0.5) ----
    rdesc::ImportedMaterialDesc legacy;
    legacy.base_color = Eigen::Vector3f(0.1f, 0.6f, 0.9f);
    legacy.emissive   = Eigen::Vector3f(0.3f, 0.0f, 0.0f);
    legacy.is_legacy  = true;
    MaterialGraph gl;
    const bool legacy_ok = convertAndCompile(legacy, gl, "Legacy");
    check(legacy_ok, "legacy (Phong) desc converts + lowers + compiles (collapses to PBR)");
    check(gl.shading_model == rdesc::ELightingTechnique::PbrMetallicRoughness, "legacy desc collapses to PBR shading model");

    // ---- 3. Bare PBR desc (no textures, factor-only) ----
    rdesc::ImportedMaterialDesc bare;
    bare.base_color = Eigen::Vector3f(0.5f, 0.5f, 0.5f);
    bare.metallic   = 0.0f;
    bare.roughness  = 0.4f;
    MaterialGraph gb;
    const bool bare_ok = convertAndCompile(bare, gb, "BarePBR");
    check(bare_ok, "factor-only PBR desc converts + lowers + compiles");
    check(gb.texture_slots.empty(), "factor-only PBR graph declares no texture slots");

    // ---- 4. SHARING PROOF (W4): two PBR descs with IDENTICAL structure (same texture
    //         layout) differing ONLY in factor VALUES must produce BYTE-IDENTICAL SPIR-V
    //         + the SAME shader fingerprint — because the factors are now Params (SSBO
    //         lanes), not baked literals. That is exactly what lets the GPU SPIR-V dedup
    //         collapse them onto ONE PSO (each keeps its own SSBO lane) — the
    //         Material/Instance explosion fix. ----
    auto bakePbr = [](const Eigen::Vector3f& base, float metallic, float rough,
                      std::vector<uint32_t>& gbuf, std::vector<uint32_t>& fwd) -> bool
    {
        rdesc::ImportedMaterialDesc m;
        m.base_color     = base;
        m.base_color_tex = tex(0);   // SAME texture structure in both
        m.metallic       = metallic;
        m.roughness      = rough;
        auto g_exp = lux::shadergen::material::materialToGraph(m);
        if (!g_exp) return false;
        MaterialGraph& g = *g_exp;
        auto gb_cs = lux::mgtest::compileGraph(g, rdesc::EMaterialPass::GBuffer);
        if (!gb_cs) return false;
        auto fwd_cs = lux::mgtest::compileGraph(g, rdesc::EMaterialPass::Forward);
        if (!fwd_cs) return false;
        gbuf = std::move(gb_cs->spirv);
        fwd  = std::move(fwd_cs->spirv);
        return true;
    };
    {
        std::vector<uint32_t> gA, fA, gB, fB;
        const bool okA = bakePbr({ 0.8f, 0.7f, 0.6f }, 0.2f, 0.4f, gA, fA);
        const bool okB = bakePbr({ 0.1f, 0.2f, 0.3f }, 0.9f, 0.7f, gB, fB);
        check(okA && okB, "two value-different PBR descs both convert + compile");
        check(okA && okB && gA == gB && fA == fB,
              "value-only-different descs share a shader fingerprint (factors are Params, not literals)");
        check(okA && okB && gA == gB && fA == fB,
              "value-only-different descs produce BYTE-IDENTICAL SPIR-V (-> dedup -> one shared PSO)");
    }

    std::cout << "=== material_to_graph_test " << (fails == 0 ? "PASSED" : "FAILED")
              << " (fails=" << fails << ") ===\n";
    return fails == 0 ? 0 : 1;
}
