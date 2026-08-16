// =============================================================================
//  reflect_test.cpp  --  shadergen_glsl interface self-consistency check (exit 0 = pass)
// -----------------------------------------------------------------------------
//  Goal: prove that the interface contract shadergen hands to render (the
//  hand-filled ShaderInfo produced by compileToSpirv) matches the SPIR-V it
//  actually produces -- after render switches to binding descriptors from
//  ShaderInfo, every binding it binds must actually exist, and every
//  per-material binding the IR is supposed to report must be present with
//  none missing. Device-independent; true rendering equivalence is covered by
//  a later end-to-end test. This is also the first step toward follow-up #3
//  (replacing the hand-filled ShaderInfo with reflection).
//
//  Two complementary reconciliation passes that together mean "ShaderInfo
//  neither lies nor omits":
//
//   (1) Reflection reconciliation (ShaderInfo subset-of reflection, catches
//       "over-reporting/wrong entries"): every hand-filled (set,binding,type)
//       must find an exact match in the real reflected SPIR-V descriptors.
//       This is direction A, and it holds for every pass. GBuffer additionally
//       gets a two-way equality check (it doesn't #include lighting_common, so
//       the reflected set == the per-material set, with no engine-fixed-set
//       noise); Forward only reverses the check for set-4 (set-2/set-3 are
//       engine-fixed bindings brought in by lighting_common and are not part
//       of the per-material ShaderInfo).
//
//   (2) IR-intent reconciliation (checkPerMaterialSets, catches
//       "omissions/over-reporting"): direction A is immune to omissions (a
//       ShaderInfo that's missing an entry is still a valid subset), so
//       reflection reconciliation alone can't catch a missing set-2 entry on
//       forward (forward's uTex[] is declared by lighting_common and isn't
//       stripped by glslang even when unused, so "reflected set-2 subset-of
//       ShaderInfo" can't be used here -- it would false-positive on the
//       unused declaration). Instead we gate on graph intent: if the graph
//       samples a texture, ShaderInfo must contain the set-2 b0 CIS entry; if
//       it has tex or param, it must contain exactly one set-4 b4 SSBO entry;
//       otherwise it must have neither. This is pass-independent, and it
//       catches forward's omission while naturally avoiding a false positive
//       on the unused declaration.
//
//  Reconciliation granularity = the (set,binding,type) triple. Scope note:
//  count/array-ness/blockSize/writable are out of scope for this round of
//  reconciliation (the bindless uTex[]'s hand-filled count=1 versus the real
//  runtime array is a mismatch this test can't see) -- left for a later
//  end-to-end test or a future reflection enhancement.
//
//  Requires spirv-cross-core (a find_package target from resource::asset) plus
//  libshaderc (pulled in transitively via shadergen_glsl). LUX_RENDER_SHADERS_DIR
//  is injected by CMake.
// =============================================================================

#include <lux/engine/toolchain/shader/Backend.hpp>
#include <lux/engine/toolchain/asset/material/MaterialLowering.hpp>
#include <lux/engine/authoring/assets/material/MaterialGraph.hpp>
#include <lux/engine/authoring/assets/material/Nodes.hpp>
#include <lux/engine/description/ShaderInfo.hpp>

#include <spirv_cross/spirv_cross.hpp>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace rdesc = lux::rdesc;
namespace sgm   = lux::shadergen::material;
namespace sgg   = lux::shadergen::glsl;
namespace sc    = SPIRV_CROSS_NAMESPACE;

static int g_fails = 0;
#define CHECK(c, m) do { \
    if (c) std::printf("  [PASS] %s\n", m); \
    else { std::printf("  [FAIL] %s\n", m); ++g_fails; } \
} while (0)

// OutputSurface pin order matches the EMaterialAttribute contract order (NormalTS = index 4).
enum { ATTR_BASE_COLOR = 0, ATTR_EMISSIVE = 3, ATTR_NORMAL_TS = 4 };

// ---- Graph-building helpers (same pattern as emit_test) ------------------------------------

static rdesc::MaterialGraph makeConstGraph(rdesc::EMaterialShadingModel sm)
{
    rdesc::MaterialGraph g;
    g.shading_model = sm;
    auto out = g.addNode(std::make_unique<rdesc::OutputSurfaceNode>());
    auto cn  = std::make_unique<rdesc::ConstantNode>(); cn->setType(rdesc::EMatValueType::Vec3);
    cn->value[0] = 0.8f; cn->value[1] = 0.2f; cn->value[2] = 0.1f;
    auto c   = g.addNode(std::move(cn));
    g.connect(c, 0, out, ATTR_BASE_COLOR);
    return g;
}

static rdesc::MaterialGraph makeTexturedGraph(rdesc::EMaterialShadingModel sm)
{
    rdesc::MaterialGraph g;
    g.shading_model = sm;
    g.texture_slots.push_back({ "albedo" });
    auto out = g.addNode(std::make_unique<rdesc::OutputSurfaceNode>());
    auto s   = std::make_unique<rdesc::SampleTextureNode>(); s->texture_slot = 0;
    auto si  = g.addNode(std::move(s));
    g.connect(si, 0, out, ATTR_BASE_COLOR);  // sample(vec4) -> base_color
    return g;
}

static rdesc::MaterialGraph makeParamGraph(rdesc::EMaterialShadingModel sm)
{
    rdesc::MaterialGraph g;
    g.shading_model = sm;
    rdesc::ParamSlotDecl ps; ps.name = "tint"; ps.type = rdesc::EMatValueType::Vec3;
    g.param_slots.push_back(ps);
    auto out = g.addNode(std::make_unique<rdesc::OutputSurfaceNode>());
    auto pn  = std::make_unique<rdesc::ParamNode>(rdesc::EMatValueType::Vec3); pn->param_slot = 0;
    auto pi  = g.addNode(std::move(pn));
    g.connect(pi, 0, out, ATTR_BASE_COLOR);  // param(vec3) -> base_color
    return g;
}

// tex + param together: sample -> base_color, param -> emissive (sharing the same set-4 uMats).
static rdesc::MaterialGraph makeTexParamGraph(rdesc::EMaterialShadingModel sm)
{
    rdesc::MaterialGraph g;
    g.shading_model = sm;
    g.texture_slots.push_back({ "albedo" });
    rdesc::ParamSlotDecl ps; ps.name = "tint"; ps.type = rdesc::EMatValueType::Vec3;
    g.param_slots.push_back(ps);
    auto out = g.addNode(std::make_unique<rdesc::OutputSurfaceNode>());
    auto s   = std::make_unique<rdesc::SampleTextureNode>(); s->texture_slot = 0;
    auto si  = g.addNode(std::move(s));
    auto pn  = std::make_unique<rdesc::ParamNode>(rdesc::EMatValueType::Vec3); pn->param_slot = 0;
    auto pi  = g.addNode(std::move(pn));
    g.connect(si, 0, out, ATTR_BASE_COLOR);  // sample(vec4) -> base_color
    g.connect(pi, 0, out, ATTR_EMISSIVE);    // param(vec3)  -> emissive
    return g;
}

// Normal-map chain (textured): SampleTexture -> DecodeNormal -> TbnTransform -> normal_ts.
static rdesc::MaterialGraph makeNormalMapGraph(rdesc::EMaterialShadingModel sm)
{
    rdesc::MaterialGraph g;
    g.shading_model = sm;
    g.texture_slots.push_back({ "normal_map" });
    auto out = g.addNode(std::make_unique<rdesc::OutputSurfaceNode>());
    auto si  = g.addNode(std::make_unique<rdesc::SampleTextureNode>());
    static_cast<rdesc::SampleTextureNode*>(g.node(si))->texture_slot = 0;
    auto di  = g.addNode(std::make_unique<rdesc::DecodeNormalNode>());
    auto ti  = g.addNode(std::make_unique<rdesc::TbnTransformNode>());
    g.connect(si, 0, di, 0);
    g.connect(di, 0, ti, 0);
    g.connect(ti, 0, out, ATTR_NORMAL_TS);
    return g;
}

// ---- Reflection helpers -----------------------------------------------------------

struct RefBinding { uint32_t set; uint32_t binding; rdesc::EDescriptorType type; };

// Reflects every declared descriptor in the SPIR-V (the no-arg
// get_shader_resources() call returns all declared resources). The type list
// is kept aligned with toolchain::reflectSpirv -- missing a category here
// would let the reverse direction of "reflection superset-of ShaderInfo"
// silently pass over an omission of that resource category as a false negative.
static std::vector<RefBinding> reflectDescriptors(const std::vector<uint32_t>& spv)
{
    sc::Compiler comp(spv.data(), spv.size());
    const auto res = comp.get_shader_resources();
    std::vector<RefBinding> out;
    auto collect = [&](const auto& list, rdesc::EDescriptorType ty) {
        for (const auto& r : list)
            out.push_back(RefBinding{
                comp.get_decoration(r.id, spv::DecorationDescriptorSet),
                comp.get_decoration(r.id, spv::DecorationBinding),
                ty });
    };
    collect(res.sampled_images,         rdesc::EDescriptorType::COMBINED_IMAGE_SAMPLER);
    collect(res.storage_buffers,        rdesc::EDescriptorType::STORAGE_BUFFER);
    collect(res.uniform_buffers,        rdesc::EDescriptorType::UNIFORM_BUFFER);
    collect(res.separate_images,        rdesc::EDescriptorType::SAMPLED_IMAGE);
    collect(res.separate_samplers,      rdesc::EDescriptorType::SAMPLER);
    collect(res.storage_images,         rdesc::EDescriptorType::STORAGE_IMAGE);
    collect(res.subpass_inputs,         rdesc::EDescriptorType::INPUT_ATTACHMENT);
    collect(res.acceleration_structures,rdesc::EDescriptorType::ACCELERATION_STRUCTURE);
    return out;
}

static bool reflectEntryIsFragMain(const std::vector<uint32_t>& spv)
{
    sc::Compiler comp(spv.data(), spv.size());
    for (const auto& e : comp.get_entry_points_and_stages())
        if (e.name == "main" && e.execution_model == spv::ExecutionModelFragment) return true;
    return false;
}

static void dumpReflected(const char* label, const std::vector<RefBinding>& refl)
{
    std::printf("    [%s] reflected:", label);
    if (refl.empty()) std::printf(" (none)");
    for (const auto& r : refl)
        std::printf(" (set%u b%u %s)", r.set, r.binding, rdesc::to_string(r.type));
    std::printf("\n");
}

// Direction A: every ShaderInfo binding finds an exact match in the reflection (set/binding/type). Catches "over-reporting/wrong entries".
static bool shaderInfoSubsetOfReflected(const rdesc::ShaderInfo&       info,
                                        const std::vector<RefBinding>& refl,
                                        std::string*                   miss)
{
    for (const auto& s : info.sets)
        for (const auto& b : s.bindings) {
            bool found = false;
            for (const auto& r : refl)
                if (r.set == b.set && r.binding == b.binding && r.type == b.type) { found = true; break; }
            if (!found) {
                if (miss) *miss = "set " + std::to_string(b.set) + " binding " + std::to_string(b.binding);
                return false;
            }
        }
    return true;
}

// Whether a single reflected binding is present in ShaderInfo (exact set/binding/type match).
static bool reflectedBindingInShaderInfo(const RefBinding& r, const rdesc::ShaderInfo& info)
{
    for (const auto& s : info.sets)
        for (const auto& b : s.bindings)
            if (b.set == r.set && b.binding == r.binding && b.type == r.type) return true;
    return false;
}

// IR-intent reconciliation: ShaderInfo's per-material sets (set-2/set-4) must
// match graph intent exactly -- this closes both the "omission" gap (direction
// A is immune to omissions) and the "over-reporting" gap. It gates on graph
// intent (contains a texture sample / contains a param) rather than on
// reflection's "is it declared" (forward's lighting_common declares an unused
// uTex, so set-2 is always present in the reflection; using reflection for
// forward's set-2 reverse check would false-positive on that unused
// declaration). Pass-independent.
static void checkPerMaterialSets(const rdesc::ShaderInfo& info,
                                 bool expect_tex, bool expect_param, const char* label)
{
    bool has_s2 = false, s2_shape_ok = true;
    bool has_s4 = false, s4_shape_ok = true; int s4_count = 0;
    for (const auto& s : info.sets)
        for (const auto& b : s.bindings) {
            if (b.set == 2) { has_s2 = true;
                if (b.binding != 0 || b.type != rdesc::EDescriptorType::COMBINED_IMAGE_SAMPLER) s2_shape_ok = false; }
            if (b.set == 4) { has_s4 = true; ++s4_count;
                if (b.binding != 4 || b.type != rdesc::EDescriptorType::STORAGE_BUFFER) s4_shape_ok = false; }
        }
    const bool expect_s2 = expect_tex;
    const bool expect_s4 = expect_tex || expect_param;
    const std::string m2 = std::string(label) + " set-2 与 IR 意图一致（含 tex⇒set2 b0 CIS，否则无）";
    const std::string m4 = std::string(label) + " set-4 与 IR 意图一致（含 tex|param⇒set4 b4 SSBO 恰一条，否则无）";
    CHECK(has_s2 == expect_s2 && s2_shape_ok, m2.c_str());
    CHECK(has_s4 == expect_s4 && s4_shape_ok && (!expect_s4 || s4_count == 1), m4.c_str());
}

// Compile + reflect + assert the entry point. A failure here is an immediate
// red (++g_fails) -- this test's whole premise is that these shaders must be
// able to compile, so a compile/lowering regression must never silently
// degrade into "fewer assertions ran, but still exit 0".
static bool buildAndReflect(const rdesc::MaterialGraph&      g,
                            rdesc::EMaterialPass             pass,
                            const std::vector<std::string>&  inc,
                            rdesc::ShaderInfo&               info,
                            std::vector<RefBinding>&         refl,
                            const char*                      label)
{
    auto lr = sgm::lowerMaterial(g);
    if (!lr) {
        std::printf("  [FAIL] %s: lower 失败: %s\n", label, lr.error().c_str()); ++g_fails; return false;
    }
    sgg::EmitParams p; p.pass = pass; p.shading_model = lr->shading_model;
    auto cs = sgg::compileToSpirv(lr->ir, p, inc);
    if (!cs) {
        std::printf("  [FAIL] %s: compile 失败: %s\n", label, cs.error().c_str()); ++g_fails; return false;
    }
    info = std::move(cs->info);
    const std::vector<uint32_t> spv = std::move(cs->spirv);
    if (spv.empty() || spv[0] != 0x07230203u) {
        std::printf("  [FAIL] %s: bad SPIR-V magic\n", label); ++g_fails; return false;
    }
    refl = reflectDescriptors(spv);
    CHECK(reflectEntryIsFragMain(spv), (std::string(label) + " 反射出 entry main/FRAGMENT").c_str());
    return true;
}

// GBuffer two-way equality (reflected set == ShaderInfo, no engine-fixed-set noise).
static bool gbufferEqual(const std::vector<RefBinding>& refl, const rdesc::ShaderInfo& info)
{
    std::string miss;
    if (!shaderInfoSubsetOfReflected(info, refl, &miss)) return false;     // direction A
    for (const auto& r : refl) if (!reflectedBindingInShaderInfo(r, info)) return false;  // reverse direction
    return true;
}

int main()
{
    std::printf("=== shadergen_glsl_reflect_test ===\n");
    const std::vector<std::string> inc = { LUX_LGLSL_EMITTED_DIR, LUX_RENDER_SHADERS_DIR };

    // === A. GBuffer const PBR: no tex/param -> ShaderInfo empty, reflection empty (GBuffer has no engine-fixed set) ===
    {
        rdesc::ShaderInfo info; std::vector<RefBinding> refl;
        if (buildAndReflect(makeConstGraph(rdesc::EMaterialShadingModel::PbrMetallicRoughness),
                            rdesc::EMaterialPass::GBuffer, inc, info, refl, "A gbuffer-const-pbr")) {
            dumpReflected("A gbuffer-const-pbr", refl);
            checkPerMaterialSets(info, false, false, "A");
            CHECK(info.sets.empty() && refl.empty(), "A GBuffer const-PBR：ShaderInfo 空 == 反射空");
        }
    }

    // === B. GBuffer textured PBR: ShaderInfo {set2,set4}, two-way equal with reflection ===
    {
        rdesc::ShaderInfo info; std::vector<RefBinding> refl;
        if (buildAndReflect(makeTexturedGraph(rdesc::EMaterialShadingModel::PbrMetallicRoughness),
                            rdesc::EMaterialPass::GBuffer, inc, info, refl, "B gbuffer-tex-pbr")) {
            dumpReflected("B gbuffer-tex-pbr", refl);
            checkPerMaterialSets(info, true, false, "B");
            CHECK(gbufferEqual(refl, info), "B GBuffer 反射 == ShaderInfo（双向，无漏报/多报）");
        }
    }

    // === C. GBuffer param-only PBR: ShaderInfo {set4}, no set2, two-way equal with reflection ===
    {
        rdesc::ShaderInfo info; std::vector<RefBinding> refl;
        if (buildAndReflect(makeParamGraph(rdesc::EMaterialShadingModel::PbrMetallicRoughness),
                            rdesc::EMaterialPass::GBuffer, inc, info, refl, "C gbuffer-param-pbr")) {
            dumpReflected("C gbuffer-param-pbr", refl);
            checkPerMaterialSets(info, false, true, "C");
            CHECK(gbufferEqual(refl, info), "C GBuffer 反射 == ShaderInfo（双向）");
        }
    }

    // === D. GBuffer const Unlit: ShaderInfo empty == reflection empty (shading_model doesn't affect descriptor layout) ===
    {
        rdesc::ShaderInfo info; std::vector<RefBinding> refl;
        if (buildAndReflect(makeConstGraph(rdesc::EMaterialShadingModel::Unlit),
                            rdesc::EMaterialPass::GBuffer, inc, info, refl, "D gbuffer-const-unlit")) {
            checkPerMaterialSets(info, false, false, "D");
            CHECK(info.sets.empty() && refl.empty(), "D GBuffer const-Unlit：ShaderInfo 空 == 反射空");
        }
    }

    // === H. GBuffer tex+param PBR: set2 + set4 together, exactly one set4 entry (sharing uMats), two-way equal ===
    {
        rdesc::ShaderInfo info; std::vector<RefBinding> refl;
        if (buildAndReflect(makeTexParamGraph(rdesc::EMaterialShadingModel::PbrMetallicRoughness),
                            rdesc::EMaterialPass::GBuffer, inc, info, refl, "H gbuffer-tex+param-pbr")) {
            dumpReflected("H gbuffer-tex+param-pbr", refl);
            checkPerMaterialSets(info, true, true, "H");  // exactly one set4 entry is guaranteed by checkPerMaterialSets
            CHECK(gbufferEqual(refl, info), "H GBuffer tex+param 反射 == ShaderInfo（双向，set4 不重复）");
        }
    }

    // === I. GBuffer normal-map PBR (textured + complex Tbn topology): two-way equal ===
    {
        rdesc::ShaderInfo info; std::vector<RefBinding> refl;
        if (buildAndReflect(makeNormalMapGraph(rdesc::EMaterialShadingModel::PbrMetallicRoughness),
                            rdesc::EMaterialPass::GBuffer, inc, info, refl, "I gbuffer-normalmap-pbr")) {
            dumpReflected("I gbuffer-normalmap-pbr", refl);
            checkPerMaterialSets(info, true, false, "I");
            CHECK(gbufferEqual(refl, info), "I GBuffer normal-map 反射 == ShaderInfo（textured+Tbn 拓扑不漂移）");
        }
    }

    // === E. Forward const PBR: ShaderInfo empty subset-of reflection; engine-fixed set0/3 brought in by lighting_common ===
    {
        rdesc::ShaderInfo info; std::vector<RefBinding> refl;
        if (buildAndReflect(makeConstGraph(rdesc::EMaterialShadingModel::PbrMetallicRoughness),
                            rdesc::EMaterialPass::Forward, inc, info, refl, "E forward-const-pbr")) {
            dumpReflected("E forward-const-pbr", refl);
            checkPerMaterialSets(info, false, false, "E");  // const -> no per-material bindings (even though the reflection has an unused uTex)
            std::string miss;
            CHECK(shaderInfoSubsetOfReflected(info, refl, &miss), "E Forward ShaderInfo ⊆ 反射");
            bool has_engine_set = false;
            for (const auto& r : refl) if (r.set == 0 || r.set == 3) has_engine_set = true;
            CHECK(has_engine_set, "E 反射含引擎固定 set-0/3（lighting_common，刻意不进 ShaderInfo）");
        }
    }

    // === F. Forward textured PBR: the core case -- a missing set2 entry is caught by checkPerMaterialSets (the IR gate) ===
    {
        rdesc::ShaderInfo info; std::vector<RefBinding> refl;
        if (buildAndReflect(makeTexturedGraph(rdesc::EMaterialShadingModel::PbrMetallicRoughness),
                            rdesc::EMaterialPass::Forward, inc, info, refl, "F forward-tex-pbr")) {
            dumpReflected("F forward-tex-pbr", refl);
            checkPerMaterialSets(info, true, false, "F");  // contains tex -> ShaderInfo must contain set2+set4 (closes the forward set2 omission gap)
            std::string miss;
            CHECK(shaderInfoSubsetOfReflected(info, refl, &miss),
                  (std::string("F Forward ShaderInfo ⊆ 反射（手填绑定真实存在）") +
                   (miss.empty() ? "" : "  MISS=" + miss)).c_str());
            // set-4 (uMats) is purely per-material (lighting_common doesn't
            // declare set-4): first anchor that the reflection does contain
            // set-4 (guarding against the whole group vanishing and making the
            // reverse check vacuously true), then check the reverse direction
            // for omissions.
            bool refl_has_s4 = false, set4_rev = true;
            for (const auto& r : refl) {
                if (r.set == 4) refl_has_s4 = true;
                if (r.set == 4 && !reflectedBindingInShaderInfo(r, info)) set4_rev = false;
            }
            CHECK(refl_has_s4, "F 反射确含 set-4 uMats（锚定，防整组消失恒真）");
            CHECK(set4_rev, "F Forward set-4 反向：反射的 set-4 都在 ShaderInfo（per-material SSBO 不漏报）");
        }
    }

    // === G. Forward const Toon: the toon shell is equally self-consistent (per-material is empty) ===
    {
        rdesc::ShaderInfo info; std::vector<RefBinding> refl;
        if (buildAndReflect(makeConstGraph(rdesc::EMaterialShadingModel::Stylized),
                            rdesc::EMaterialPass::Forward, inc, info, refl, "G forward-const-toon")) {
            dumpReflected("G forward-const-toon", refl);
            checkPerMaterialSets(info, false, false, "G");
            std::string miss;
            CHECK(shaderInfoSubsetOfReflected(info, refl, &miss), "G Forward Toon ShaderInfo ⊆ 反射");
        }
    }

    // === J. Forward param-only PBR: one extra set-4 entry beyond const, verifies the set-4 reverse check also holds when triggered by param ===
    {
        rdesc::ShaderInfo info; std::vector<RefBinding> refl;
        if (buildAndReflect(makeParamGraph(rdesc::EMaterialShadingModel::PbrMetallicRoughness),
                            rdesc::EMaterialPass::Forward, inc, info, refl, "J forward-param-pbr")) {
            dumpReflected("J forward-param-pbr", refl);
            checkPerMaterialSets(info, false, true, "J");  // param -> set4, no set2
            std::string miss;
            CHECK(shaderInfoSubsetOfReflected(info, refl, &miss), "J Forward param ShaderInfo ⊆ 反射");
            bool refl_has_s4 = false, set4_rev = true;
            for (const auto& r : refl) {
                if (r.set == 4) refl_has_s4 = true;
                if (r.set == 4 && !reflectedBindingInShaderInfo(r, info)) set4_rev = false;
            }
            CHECK(refl_has_s4 && set4_rev, "J Forward param-only set-4 反向不漏报");
        }
    }

    std::printf("=== shadergen_glsl_reflect_test %s (fails=%d) ===\n", g_fails ? "FAILED" : "PASSED", g_fails);
    return g_fails ? 1 : 0;
}
