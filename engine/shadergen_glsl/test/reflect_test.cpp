// =============================================================================
//  reflect_test.cpp  —  shadergen_glsl 接口自洽（里程碑④，exit 0 = pass）
// -----------------------------------------------------------------------------
//  目标：证明 shadergen 交给 render 的接口契约（compileToSpirv 手填的 ShaderInfo）
//  与它真实产出的 SPIR-V 一致——render 切换后照 ShaderInfo 绑 descriptor，绑的每个
//  绑定必须真实存在、且 IR 该报的 per-material 绑定一个不漏。设备无关；真渲染等价
//  属后续 ⑤ 端到端。也是 follow-up #3（反射替手填 ShaderInfo）的第一步。
//
//  两套互补的对账，合起来 = "ShaderInfo 既不撒谎也不漏报"：
//
//   (1) 反射对账（ShaderInfo ⊆ 反射，抓"多报/写错"）：手填的每个 (set,binding,type)
//       都能在反射出的真实 SPIR-V descriptor 中精确匹配。方向 A，对所有 pass 成立。
//       GBuffer 另做双向相等（不 #include lighting_common，反射集 == per-material 集，
//       无引擎固定 set 噪声）；Forward 只对 set-4 反向（set-2/set-3 是 lighting_common
//       引入的引擎固定绑定，不进 per-material ShaderInfo）。
//
//   (2) IR 意图对账（checkPerMaterialSets，抓"漏报/多报"）：方向 A 对【漏报】免疫
//       （ShaderInfo 少一条仍是子集），故单靠反射对账抓不到 forward 的 set-2 漏报
//       （forward 的 uTex[] 由 lighting_common 声明、const 也不被 glslang strip，
//       不能用"反射 set-2 ⊆ ShaderInfo"——会误报 const）。改用【图意图】做闸：图采样
//       纹理 ⇒ ShaderInfo 必含 set-2 b0 CIS；含 tex|param ⇒ 必含 set-4 b4 SSBO（恰一
//       条）；否则必无。pass 无关，既抓 forward 漏报又天然不误报 const。
//
//  对账粒度 = (set,binding,type) 三元组。【边界声明】：count/array-性/blockSize/
//  writable 不在本轮对账范围（bindless uTex[] 手填 count=1 vs 真 runtime array 的
//  不一致本测试看不见）——留给 ⑤ 端到端或后续反射增强。
//
//  需要 spirv-cross-core（resource::asset 的 find_package target）+ libshaderc
//  （shadergen_glsl 传递）。LUX_RENDER_SHADERS_DIR 由 CMake 注入。
// =============================================================================

#include <lux/engine/shadergen/glsl/Backend.hpp>
#include <lux/engine/shadergen/material/MaterialLowering.hpp>
#include <lux/engine/description/material_graph/MaterialGraph.hpp>
#include <lux/engine/description/material_graph/Nodes.hpp>
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

// OutputSurface 引脚顺序 = EMaterialAttribute 契约顺序（NormalTS = 索引 4）。
enum { ATTR_BASE_COLOR = 0, ATTR_EMISSIVE = 3, ATTR_NORMAL_TS = 4 };

// ---- 建图 helper（与 emit_test 同模式） ------------------------------------

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

// tex + param 同时：sample->base_color，param->emissive（共用同一 set-4 uMats）。
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

// 法线贴图链（textured）：SampleTexture -> DecodeNormal -> TbnTransform -> normal_ts。
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

// ---- 反射 helper -----------------------------------------------------------

struct RefBinding { uint32_t set; uint32_t binding; rdesc::EDescriptorType type; };

// 反射 SPIR-V 的全部声明 descriptor（无参 get_shader_resources = 所有声明的资源）。
// 类型清单与 ShaderSerDeser::reflect 对齐——漏收某类会让"反射 ⊇ ShaderInfo"的反向方
// 向对那类资源的漏报假阴性放过。
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

// 方向 A：ShaderInfo 每个 binding 在反射中找到精确匹配（set/binding/type）。抓"多报/写错"。
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

// 单个反射 binding 是否在 ShaderInfo（精确 set/binding/type）。
static bool reflectedBindingInShaderInfo(const RefBinding& r, const rdesc::ShaderInfo& info)
{
    for (const auto& s : info.sets)
        for (const auto& b : s.bindings)
            if (b.set == r.set && b.binding == r.binding && b.type == r.type) return true;
    return false;
}

// IR 意图对账：ShaderInfo 的 per-material set（set-2/set-4）必须与图意图精确一致——
// 堵"漏报"（方向 A 对漏报免疫）和"多报"。用图意图（含纹理采样 / 含参数）做闸，而非
// 反射的"是否声明"（forward 的 lighting_common 声明 unused uTex，反射 set-2 恒存在，
// 不能用反射做 forward 的 set-2 反向，会误报 const）。pass 无关。
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

// 编译 + 反射 + entry 断言。失败【即红】（++g_fails）——本测试的价值前提是这些 shader
// 必须能编译出来，编译/lowering 回归不能静默退化成"少跑断言 + exit 0"。
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

// GBuffer 双向相等（反射集 == ShaderInfo，无引擎固定 set 噪声）。
static bool gbufferEqual(const std::vector<RefBinding>& refl, const rdesc::ShaderInfo& info)
{
    std::string miss;
    if (!shaderInfoSubsetOfReflected(info, refl, &miss)) return false;     // 方向 A
    for (const auto& r : refl) if (!reflectedBindingInShaderInfo(r, info)) return false;  // 反向
    return true;
}

int main()
{
    std::printf("=== shadergen_glsl_reflect_test ===\n");
    const std::vector<std::string> inc = { LUX_RENDER_SHADERS_DIR };

    // === A. GBuffer const PBR：无 tex/param → ShaderInfo 空、反射空（GBuffer 无引擎固定 set） ===
    {
        rdesc::ShaderInfo info; std::vector<RefBinding> refl;
        if (buildAndReflect(makeConstGraph(rdesc::EMaterialShadingModel::PbrMetallicRoughness),
                            rdesc::EMaterialPass::GBuffer, inc, info, refl, "A gbuffer-const-pbr")) {
            dumpReflected("A gbuffer-const-pbr", refl);
            checkPerMaterialSets(info, false, false, "A");
            CHECK(info.sets.empty() && refl.empty(), "A GBuffer const-PBR：ShaderInfo 空 == 反射空");
        }
    }

    // === B. GBuffer textured PBR：ShaderInfo {set2,set4}，与反射双向相等 ===
    {
        rdesc::ShaderInfo info; std::vector<RefBinding> refl;
        if (buildAndReflect(makeTexturedGraph(rdesc::EMaterialShadingModel::PbrMetallicRoughness),
                            rdesc::EMaterialPass::GBuffer, inc, info, refl, "B gbuffer-tex-pbr")) {
            dumpReflected("B gbuffer-tex-pbr", refl);
            checkPerMaterialSets(info, true, false, "B");
            CHECK(gbufferEqual(refl, info), "B GBuffer 反射 == ShaderInfo（双向，无漏报/多报）");
        }
    }

    // === C. GBuffer param-only PBR：ShaderInfo {set4}，无 set2，与反射双向相等 ===
    {
        rdesc::ShaderInfo info; std::vector<RefBinding> refl;
        if (buildAndReflect(makeParamGraph(rdesc::EMaterialShadingModel::PbrMetallicRoughness),
                            rdesc::EMaterialPass::GBuffer, inc, info, refl, "C gbuffer-param-pbr")) {
            dumpReflected("C gbuffer-param-pbr", refl);
            checkPerMaterialSets(info, false, true, "C");
            CHECK(gbufferEqual(refl, info), "C GBuffer 反射 == ShaderInfo（双向）");
        }
    }

    // === D. GBuffer const Unlit：ShaderInfo 空 == 反射空（shading_model 不影响 descriptor 布局） ===
    {
        rdesc::ShaderInfo info; std::vector<RefBinding> refl;
        if (buildAndReflect(makeConstGraph(rdesc::EMaterialShadingModel::Unlit),
                            rdesc::EMaterialPass::GBuffer, inc, info, refl, "D gbuffer-const-unlit")) {
            checkPerMaterialSets(info, false, false, "D");
            CHECK(info.sets.empty() && refl.empty(), "D GBuffer const-Unlit：ShaderInfo 空 == 反射空");
        }
    }

    // === H. GBuffer tex+param PBR：set2 + set4 同时，set4 恰一条（共用 uMats），双向相等 ===
    {
        rdesc::ShaderInfo info; std::vector<RefBinding> refl;
        if (buildAndReflect(makeTexParamGraph(rdesc::EMaterialShadingModel::PbrMetallicRoughness),
                            rdesc::EMaterialPass::GBuffer, inc, info, refl, "H gbuffer-tex+param-pbr")) {
            dumpReflected("H gbuffer-tex+param-pbr", refl);
            checkPerMaterialSets(info, true, true, "H");  // set4 恰一条由 checkPerMaterialSets 保证
            CHECK(gbufferEqual(refl, info), "H GBuffer tex+param 反射 == ShaderInfo（双向，set4 不重复）");
        }
    }

    // === I. GBuffer normal-map PBR（textured + 复杂拓扑 Tbn）：双向相等 ===
    {
        rdesc::ShaderInfo info; std::vector<RefBinding> refl;
        if (buildAndReflect(makeNormalMapGraph(rdesc::EMaterialShadingModel::PbrMetallicRoughness),
                            rdesc::EMaterialPass::GBuffer, inc, info, refl, "I gbuffer-normalmap-pbr")) {
            dumpReflected("I gbuffer-normalmap-pbr", refl);
            checkPerMaterialSets(info, true, false, "I");
            CHECK(gbufferEqual(refl, info), "I GBuffer normal-map 反射 == ShaderInfo（textured+Tbn 拓扑不漂移）");
        }
    }

    // === E. Forward const PBR：ShaderInfo 空 ⊆ 反射；引擎固定 set0/3 由 lighting_common 引入 ===
    {
        rdesc::ShaderInfo info; std::vector<RefBinding> refl;
        if (buildAndReflect(makeConstGraph(rdesc::EMaterialShadingModel::PbrMetallicRoughness),
                            rdesc::EMaterialPass::Forward, inc, info, refl, "E forward-const-pbr")) {
            dumpReflected("E forward-const-pbr", refl);
            checkPerMaterialSets(info, false, false, "E");  // const → 无 per-material 绑定（即便反射有 unused uTex）
            std::string miss;
            CHECK(shaderInfoSubsetOfReflected(info, refl, &miss), "E Forward ShaderInfo ⊆ 反射");
            bool has_engine_set = false;
            for (const auto& r : refl) if (r.set == 0 || r.set == 3) has_engine_set = true;
            CHECK(has_engine_set, "E 反射含引擎固定 set-0/3（lighting_common，刻意不进 ShaderInfo）");
        }
    }

    // === F. Forward textured PBR：核心 case —— set2 漏报由 checkPerMaterialSets(IR 闸) 堵住 ===
    {
        rdesc::ShaderInfo info; std::vector<RefBinding> refl;
        if (buildAndReflect(makeTexturedGraph(rdesc::EMaterialShadingModel::PbrMetallicRoughness),
                            rdesc::EMaterialPass::Forward, inc, info, refl, "F forward-tex-pbr")) {
            dumpReflected("F forward-tex-pbr", refl);
            checkPerMaterialSets(info, true, false, "F");  // 含 tex ⇒ 强制 ShaderInfo 含 set2+set4（堵 forward set2 漏报）
            std::string miss;
            CHECK(shaderInfoSubsetOfReflected(info, refl, &miss),
                  (std::string("F Forward ShaderInfo ⊆ 反射（手填绑定真实存在）") +
                   (miss.empty() ? "" : "  MISS=" + miss)).c_str());
            // set-4(uMats) 纯 per-material（lighting_common 不声明 set-4）：先锚定反射确含 set-4
            // （防整组消失让反向恒真），再反向不漏报。
            bool refl_has_s4 = false, set4_rev = true;
            for (const auto& r : refl) {
                if (r.set == 4) refl_has_s4 = true;
                if (r.set == 4 && !reflectedBindingInShaderInfo(r, info)) set4_rev = false;
            }
            CHECK(refl_has_s4, "F 反射确含 set-4 uMats（锚定，防整组消失恒真）");
            CHECK(set4_rev, "F Forward set-4 反向：反射的 set-4 都在 ShaderInfo（per-material SSBO 不漏报）");
        }
    }

    // === G. Forward const Toon：toon 外壳同样接口自洽（per-material 空） ===
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

    // === J. Forward param-only PBR：const 之外多一条 set-4，验证 set-4 反向在 param 触发时也成立 ===
    {
        rdesc::ShaderInfo info; std::vector<RefBinding> refl;
        if (buildAndReflect(makeParamGraph(rdesc::EMaterialShadingModel::PbrMetallicRoughness),
                            rdesc::EMaterialPass::Forward, inc, info, refl, "J forward-param-pbr")) {
            dumpReflected("J forward-param-pbr", refl);
            checkPerMaterialSets(info, false, true, "J");  // param ⇒ set4，无 set2
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
