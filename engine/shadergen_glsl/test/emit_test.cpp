// =============================================================================
//  emit_test.cpp  —  shadergen_glsl engine 半自检（exit 0 = pass）
// -----------------------------------------------------------------------------
//  rdesc::MaterialGraph -> (lowerMaterial) -> ShaderIR -> (emitGlsl/compileToSpirv)
//  -> GLSL/SPIR-V。验证 M1 范围：GBuffer(PBR/Unlit) + Forward Unlit；外壳用
//  #include 真 SSOT（经 IncluderInterface 解析，非内联）；M1 边界明确报错。
//  需要 libshaderc（链接 shadergen_glsl 传递）。LUX_RENDER_SHADERS_DIR 由 CMake 注入。
// =============================================================================

#include <lux/engine/shadergen/glsl/Backend.hpp>
#include <lux/engine/shadergen/material/MaterialLowering.hpp>
#include <lux/engine/description/material_graph/MaterialGraph.hpp>
#include <lux/engine/description/material_graph/Nodes.hpp>
#include <lux/engine/description/ShaderInfo.hpp>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace rdesc = lux::rdesc;
namespace sgm   = lux::shadergen::material;
namespace sgg   = lux::shadergen::glsl;

static int g_fails = 0;
#define CHECK(c, m) do { \
    if (c) std::printf("  [PASS] %s\n", m); \
    else { std::printf("  [FAIL] %s\n", m); ++g_fails; } \
} while (0)

static bool contains(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

// OutputSurface 引脚顺序 = EMaterialAttribute 契约顺序（NormalTS = 索引 4）。
enum { ATTR_NORMAL_TS = 4 };

// 最简 PBR 材质：constant Vec3 -> base_color（其余默认）。
static rdesc::MaterialGraph makeConstGraph(rdesc::EMaterialShadingModel sm)
{
    rdesc::MaterialGraph g;
    g.shading_model = sm;
    auto out = g.addNode(std::make_unique<rdesc::OutputSurfaceNode>());
    auto cn  = std::make_unique<rdesc::ConstantNode>(); cn->setType(rdesc::EMatValueType::Vec3);
    cn->value[0] = 0.8f; cn->value[1] = 0.2f; cn->value[2] = 0.1f;
    auto c   = g.addNode(std::move(cn));
    g.connect(c, 0, out, 0);  // base_color
    return g;
}

int main()
{
    std::printf("=== shadergen_glsl_emit_test ===\n");
    const std::vector<std::string> inc = { LUX_RENDER_SHADERS_DIR };

    // 1. emitGlsl GBuffer PBR：结构检查（#include 真 SSOT、packGNormalSM、3 MRT）。
    {
        rdesc::MaterialGraph g = makeConstGraph(rdesc::EMaterialShadingModel::PbrMetallicRoughness);
        auto lr = sgm::lowerMaterial(g);
        CHECK(lr.has_value(), "1 lowerMaterial ok");
        sgg::EmitParams p; p.pass = rdesc::EMaterialPass::GBuffer; p.shading_model = lr->shading_model;
        auto ge = sgg::emitGlsl(lr->ir, p);
        CHECK(ge.has_value(), "1 emitGlsl GBuffer PBR 非空");
        const std::string glsl = ge.has_value() ? *ge : std::string();
        CHECK(contains(glsl, "#version 450"), "1 含 #version 450");
        CHECK(contains(glsl, "#include \"gbuffer_encode.glsl\""), "1 #include 真 SSOT（非内联手抄）");
        CHECK(contains(glsl, "packGNormalSM"), "1 调 packGNormalSM");
        CHECK(contains(glsl, "LUX_SM_PBR"), "1 PBR shading-model id");
        CHECK(contains(glsl, "gAlbedoMetallic") && contains(glsl, "gNormalRoughness") && contains(glsl, "gEmissiveAo"),
              "1 三 MRT 输出声明");
        CHECK(contains(glsl, "vWorldNormal"), "1 世界法线兜底声明（引擎 interpolant 名）");
    }

    // 2. compileToSpirv GBuffer PBR：Includer 解析 SSOT -> valid SPIR-V（核心）。
    {
        rdesc::MaterialGraph g = makeConstGraph(rdesc::EMaterialShadingModel::PbrMetallicRoughness);
        auto lr = sgm::lowerMaterial(g);
        sgg::EmitParams p; p.pass = rdesc::EMaterialPass::GBuffer; p.shading_model = lr->shading_model;
        auto cs = sgg::compileToSpirv(lr->ir, p, inc);
        if (!cs) std::printf("    err: %s\n", cs.error().c_str());
        CHECK(cs.has_value(), "2 compileToSpirv ok（Includer 解析 gbuffer_encode.glsl）");
        const std::vector<uint32_t> spv = cs.has_value() ? std::move(cs->spirv) : std::vector<uint32_t>();
        const rdesc::ShaderInfo info = cs.has_value() ? std::move(cs->info) : rdesc::ShaderInfo();
        CHECK(!spv.empty() && spv[0] == 0x07230203u, "2 SPIR-V magic 有效");
        CHECK(!info.entry_points.empty() && info.entry_points[0].name == "main", "2 ShaderInfo 入口点 main");
    }

    // 3. GBuffer Unlit。
    {
        rdesc::MaterialGraph g = makeConstGraph(rdesc::EMaterialShadingModel::Unlit);
        auto lr = sgm::lowerMaterial(g);
        sgg::EmitParams p; p.pass = rdesc::EMaterialPass::GBuffer; p.shading_model = lr->shading_model;
        auto ge = sgg::emitGlsl(lr->ir, p);
        const std::string glsl = ge.has_value() ? *ge : std::string();
        CHECK(contains(glsl, "LUX_SM_UNLIT"), "3 Unlit shading-model id");
        auto cs = sgg::compileToSpirv(lr->ir, p, inc);
        CHECK(cs.has_value() && !cs->spirv.empty(), "3 Unlit GBuffer 编译 valid SPIR-V");
    }

    // 4. Forward Unlit。
    {
        rdesc::MaterialGraph g = makeConstGraph(rdesc::EMaterialShadingModel::Unlit);
        auto lr = sgm::lowerMaterial(g);
        sgg::EmitParams p; p.pass = rdesc::EMaterialPass::Forward; p.shading_model = lr->shading_model;
        auto ge = sgg::emitGlsl(lr->ir, p);
        const std::string glsl = ge.has_value() ? *ge : std::string();
        CHECK(contains(glsl, "outColor"), "4 Forward Unlit outColor");
        auto cs = sgg::compileToSpirv(lr->ir, p, inc);
        CHECK(cs.has_value() && !cs->spirv.empty(), "4 Forward Unlit 编译 valid SPIR-V");
    }

    // 5. Forward PBR：GGX 光照外壳。#include lighting_common(聚合 SSOT) + shadow_pcf + brdf_ggx；
    //    emitter 不自声明 input（复用 SSOT —— forward/gbuffer 外壳不对称的锁）。
    {
        rdesc::MaterialGraph g = makeConstGraph(rdesc::EMaterialShadingModel::PbrMetallicRoughness);
        auto lr = sgm::lowerMaterial(g);
        CHECK(lr.has_value(), "5 forward-pbr graph lowers ok");
        sgg::EmitParams p; p.pass = rdesc::EMaterialPass::Forward; p.shading_model = lr->shading_model;
        auto ge = sgg::emitGlsl(lr->ir, p);
        if (!ge) std::printf("    emit err: %s\n", ge.error().c_str());
        const std::string glsl = ge.has_value() ? *ge : std::string();
        CHECK(!glsl.empty(), "5 forward PBR emitGlsl 非空");
        CHECK(contains(glsl, "#include \"lighting_common.glsl\""), "5 #include lighting_common 聚合 SSOT");
        CHECK(contains(glsl, "#include \"shadow_pcf.glsl\"") && contains(glsl, "LUX_SHADOW_TECHNIQUE_ID"),
              "5 默认 PCF 阴影技术");
        CHECK(contains(glsl, "#include \"brdf/brdf_ggx.glsl\"") && contains(glsl, "pbrDirectLight("), "5 GGX BRDF");
        CHECK(contains(glsl, "directionalShadow(") && contains(glsl, "pointShadow("), "5 directional+point 阴影采样");
        CHECK(contains(glsl, "vViewDir") && contains(glsl, "vWorldPos"), "5 forward interpolant 引用");
        CHECK(!contains(glsl, "layout(location = 1) in"), "5 不自声明 input（复用 lighting_common，锁不对称）");
        CHECK(contains(glsl, "kAmbient"), "5 kAmbient 来自 lighting_common SSOT");
        auto cs = sgg::compileToSpirv(lr->ir, p, inc);
        if (!cs) std::printf("    compile err: %s\n", cs.error().c_str());
        CHECK(cs.has_value() && !cs->spirv.empty(), "5 forward PBR 编译 valid SPIR-V（Includer 解析 lighting_common→shadow→brdf）");
    }

    // 5b. Forward PBR + textures：set-4 Graph SSBO 自声明，但 uTex/material_types 来自
    //     lighting_common（不重复声明 —— 锁"复用 SSOT 而非自声明"）。
    {
        rdesc::MaterialGraph g;
        g.shading_model = rdesc::EMaterialShadingModel::PbrMetallicRoughness;
        g.texture_slots.push_back({ "albedo" });
        auto out = g.addNode(std::make_unique<rdesc::OutputSurfaceNode>());
        auto si  = g.addNode(std::make_unique<rdesc::SampleTextureNode>());
        static_cast<rdesc::SampleTextureNode*>(g.node(si))->texture_slot = 0;
        g.connect(si, 0, out, 0);  // sample -> base_color
        auto lr = sgm::lowerMaterial(g);
        CHECK(lr.has_value(), "5b textured forward lowers ok");
        sgg::EmitParams p; p.pass = rdesc::EMaterialPass::Forward; p.shading_model = lr->shading_model;
        auto ge = sgg::emitGlsl(lr->ir, p);
        const std::string glsl = ge.has_value() ? *ge : std::string();
        CHECK(contains(glsl, "MatBuf"), "5b set-4 Graph SSBO 自声明");
        CHECK(!contains(glsl, "uniform sampler2D uTex[]"), "5b uTex 来自 lighting_common（不重复声明）");
        CHECK(!contains(glsl, "#include \"material_types.glsl\""), "5b material_types 来自 lighting_common（不重复 include）");
        CHECK(contains(glsl, "texture(uTex[nonuniformEXT"), "5b bindless 采样表达式");
        auto cs = sgg::compileToSpirv(lr->ir, p, inc);
        if (!cs) std::printf("    compile err: %s\n", cs.error().c_str());
        CHECK(cs.has_value() && !cs->spirv.empty(), "5b textured forward PBR 编译 valid SPIR-V");
    }

    // 6. textures：SampleTexture -> set-2 bindless uTex + set-4 GraphMaterialGPU SSBO，
    //    family struct 经 #include material_types.glsl 解析（又一处内联消灭）。
    {
        rdesc::MaterialGraph g;
        g.texture_slots.push_back({ "albedo" });
        auto out = g.addNode(std::make_unique<rdesc::OutputSurfaceNode>());
        auto s   = std::make_unique<rdesc::SampleTextureNode>(); s->texture_slot = 0;
        auto si  = g.addNode(std::move(s));
        g.connect(si, 0, out, 0);  // sample(vec4) -> base_color(vec3) 隐式收窄
        auto lr = sgm::lowerMaterial(g);
        CHECK(lr.has_value(), "6 textured graph lowers ok");
        sgg::EmitParams p; p.pass = rdesc::EMaterialPass::GBuffer; p.shading_model = lr->shading_model;
        auto ge = sgg::emitGlsl(lr->ir, p);
        const std::string glsl = ge.has_value() ? *ge : std::string();
        CHECK(!glsl.empty(), "6 textured emitGlsl 非空");
        CHECK(contains(glsl, "#include \"material_types.glsl\""), "6 #include material_types SSOT");
        CHECK(contains(glsl, "GL_EXT_nonuniform_qualifier"), "6 nonuniform extension");
        CHECK(contains(glsl, "uniform sampler2D uTex[]"), "6 set-2 bindless uTex");
        CHECK(contains(glsl, "GraphMaterialGPU mats[]") && contains(glsl, "uMats"), "6 set-4 GraphMaterialGPU SSBO");
        CHECK(contains(glsl, "vMatIndex"), "6 flat vMatIndex 索引");
        CHECK(contains(glsl, "texture(uTex[nonuniformEXT"), "6 bindless 采样表达式");
        auto cs = sgg::compileToSpirv(lr->ir, p, inc);
        if (!cs) std::printf("    err: %s\n", cs.error().c_str());
        CHECK(cs.has_value() && !cs->spirv.empty(), "6 textured 编译 valid SPIR-V（Includer 解析 material_types+gbuffer_encode）");
        bool has_s2 = false, has_s4 = false;
        if (cs) for (const auto& s : cs->info.sets) { if (s.set == 2) has_s2 = true; if (s.set == 4) has_s4 = true; }
        CHECK(has_s2 && has_s4, "6 ShaderInfo 含 set-2 + set-4");
    }

    // 7. Includer 真在工作：空 include_dirs -> #include 解析失败 -> 编译失败。
    //    （若外壳是内联手抄而非 #include，这条会"通过"——所以它锁住了"用 SSOT"。）
    {
        rdesc::MaterialGraph g = makeConstGraph(rdesc::EMaterialShadingModel::PbrMetallicRoughness);
        auto lr = sgm::lowerMaterial(g);
        sgg::EmitParams p; p.pass = rdesc::EMaterialPass::GBuffer; p.shading_model = lr->shading_model;
        auto cs = sgg::compileToSpirv(lr->ir, p, {});  // 空搜索路径
        CHECK(!cs, "7 空 include_dirs -> 编译失败（证明真依赖 Includer 解析 SSOT，非内联）");
    }

    // 8. params：Param -> set-4 GraphMaterialGPU SSBO（binding 4），无 textures（无 set-2）。
    {
        rdesc::MaterialGraph g;
        rdesc::ParamSlotDecl ps; ps.name = "tint"; ps.type = rdesc::EMatValueType::Vec3;
        g.param_slots.push_back(ps);
        auto out = g.addNode(std::make_unique<rdesc::OutputSurfaceNode>());
        auto pn  = std::make_unique<rdesc::ParamNode>(rdesc::EMatValueType::Vec3); pn->param_slot = 0;
        auto pi  = g.addNode(std::move(pn));
        g.connect(pi, 0, out, 0);  // param(vec3) -> base_color(vec3)
        auto lr = sgm::lowerMaterial(g);
        CHECK(lr.has_value(), "8 param graph lowers ok");
        sgg::EmitParams p; p.pass = rdesc::EMaterialPass::GBuffer; p.shading_model = lr->shading_model;
        auto ge = sgg::emitGlsl(lr->ir, p);
        const std::string glsl = ge.has_value() ? *ge : std::string();
        CHECK(contains(glsl, "_mat.params[0]"), "8 param lane 表达式");
        CHECK(contains(glsl, "uMats") && !contains(glsl, "uTex"), "8 set-4 SSBO 但无 uTex（params-only）");
        auto cs = sgg::compileToSpirv(lr->ir, p, inc);
        if (!cs) std::printf("    err: %s\n", cs.error().c_str());
        CHECK(cs.has_value() && !cs->spirv.empty(), "8 param 编译 valid SPIR-V");
        bool has_s2 = false, has_s4 = false;
        if (cs) for (const auto& s : cs->info.sets) { if (s.set == 2) has_s2 = true; if (s.set == 4) has_s4 = true; }
        CHECK(!has_s2 && has_s4, "8 ShaderInfo 仅 set-4（params-only 无 set-2）");
    }

    // 9. 法线贴图链 + TbnNormal：SampleTexture -> DecodeNormal -> TbnTransform -> normal_ts。
    //    切线空间法线变换到世界，用 T/B/N interpolant（tangent@5 vec3 + bitangent@6）。
    {
        rdesc::MaterialGraph g;
        g.texture_slots.push_back({ "normal_map" });
        auto out = g.addNode(std::make_unique<rdesc::OutputSurfaceNode>());
        auto si  = g.addNode(std::make_unique<rdesc::SampleTextureNode>());
        static_cast<rdesc::SampleTextureNode*>(g.node(si))->texture_slot = 0;
        auto di  = g.addNode(std::make_unique<rdesc::DecodeNormalNode>());
        auto ti  = g.addNode(std::make_unique<rdesc::TbnTransformNode>());
        g.connect(si, 0, di, 0);                 // sample(vec4) -> decode
        g.connect(di, 0, ti, 0);                 // decode(vec3) -> tbn
        g.connect(ti, 0, out, ATTR_NORMAL_TS);   // tbn(vec3) -> normal_ts
        auto lr = sgm::lowerMaterial(g);
        if (!lr) std::printf("    lower err: %s\n", lr.error().c_str());
        CHECK(lr.has_value(), "9 normal-map chain lowers ok");
        sgg::EmitParams p; p.pass = rdesc::EMaterialPass::GBuffer; p.shading_model = lr->shading_model;
        auto ge = sgg::emitGlsl(lr->ir, p);
        if (!ge) std::printf("    emit err: %s\n", ge.error().c_str());
        const std::string glsl = ge.has_value() ? *ge : std::string();
        CHECK(!glsl.empty(), "9 法线贴图 emitGlsl 非空");
        CHECK(contains(glsl, "vWorldTangent") && contains(glsl, "vWorldBitangent") && contains(glsl, "vWorldNormal"),
              "9 声明 T/B/N interpolant（引擎 interpolant 名）");
        CHECK(contains(glsl, "layout(location = 5)") && contains(glsl, "layout(location = 6)"),
              "9 tangent@5 + bitangent@6");
        CHECK(contains(glsl, "mat3(normalize(vWorldTangent)"), "9 TBN 矩阵变换");
        auto cs = sgg::compileToSpirv(lr->ir, p, inc);
        if (!cs) std::printf("    compile err: %s\n", cs.error().c_str());
        CHECK(cs.has_value() && !cs->spirv.empty(), "9 法线贴图链编译 valid SPIR-V");
    }

    // 11. Forward Stylized（cel/toon）：#include brdf_toon + toonStep，不走 GGX；复用 lighting_common。
    {
        rdesc::MaterialGraph g = makeConstGraph(rdesc::EMaterialShadingModel::Stylized);
        auto lr = sgm::lowerMaterial(g);
        CHECK(lr.has_value(), "11 forward-toon graph lowers ok");
        sgg::EmitParams p; p.pass = rdesc::EMaterialPass::Forward; p.shading_model = lr->shading_model;
        auto ge = sgg::emitGlsl(lr->ir, p);
        if (!ge) std::printf("    emit err: %s\n", ge.error().c_str());
        const std::string glsl = ge.has_value() ? *ge : std::string();
        CHECK(!glsl.empty(), "11 forward toon emitGlsl 非空");
        CHECK(contains(glsl, "#include \"brdf/brdf_toon.glsl\""), "11 #include brdf_toon");
        CHECK(contains(glsl, "toonStep("), "11 toonStep 量化");
        CHECK(!contains(glsl, "pbrDirectLight"), "11 不走 GGX BRDF");
        CHECK(contains(glsl, "#include \"lighting_common.glsl\"") && contains(glsl, "directionalShadow("),
              "11 复用 lighting_common + 阴影");
        auto cs = sgg::compileToSpirv(lr->ir, p, inc);
        if (!cs) std::printf("    compile err: %s\n", cs.error().c_str());
        CHECK(cs.has_value() && !cs->spirv.empty(), "11 forward toon 编译 valid SPIR-V");
    }

    std::printf("=== shadergen_glsl_emit_test %s (fails=%d) ===\n", g_fails ? "FAILED" : "PASSED", g_fails);
    return g_fails ? 1 : 0;
}
