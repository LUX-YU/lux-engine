// =============================================================================
//  lower_material_test.cpp  —  shadergen 中性核自检（exit 0 = pass）
// -----------------------------------------------------------------------------
//  覆盖 rdesc::MaterialGraph -> ShaderIR 的：正常 lowering、两个对抗复核修复的回归锁
//  （Output.dflt 自包含 / InputSlot.location）、隐式转换、输入槽去重、错误路径、
//  fingerprint 的纯函数性与 shading_model 剥离契约。设备无关。
// =============================================================================

#include <lux/engine/shadergen/material/MaterialLowering.hpp>
#include <lux/engine/description/material_graph/MaterialGraph.hpp>
#include <lux/engine/description/material_graph/Nodes.hpp>

#include <cstdio>
#include <memory>
#include <string>

namespace rdesc = lux::rdesc;
namespace sg    = lux::shadergen;
namespace sgm   = lux::shadergen::material;

static int g_fails = 0;
#define CHECK(cond, msg) do { \
    if (cond) std::printf("  [PASS] %s\n", msg); \
    else { std::printf("  [FAIL] %s\n", msg); ++g_fails; } \
} while (0)

// OutputSurface 引脚顺序 = EMaterialAttribute 契约顺序。
enum { ATTR_BASE_COLOR = 0, ATTR_OPACITY, ATTR_METALLIC, ATTR_ROUGHNESS,
       ATTR_NORMAL_TS, ATTR_EMISSIVE, ATTR_AO };

static const sg::Output* findOutput(const sg::ShaderIR& ir, const char* name)
{
    for (const auto& o : ir.outputs) if (o.name == name) return &o;
    return nullptr;
}
static const sg::InputSlot* findInput(const sg::ShaderIR& ir, const char* name)
{
    for (const auto& s : ir.inputs) if (s.name == name) return &s;
    return nullptr;
}
static rdesc::node_id addConst(rdesc::MaterialGraph& g, rdesc::EMatValueType t,
                               float x, float y = 0, float z = 0, float w = 0)
{
    auto n = std::make_unique<rdesc::ConstantNode>();
    n->setType(t);
    n->value[0] = x; n->value[1] = y; n->value[2] = z; n->value[3] = w;
    return g.addNode(std::move(n));
}
static rdesc::node_id addInput(rdesc::MaterialGraph& g, rdesc::EMaterialInput in)
{
    auto n = std::make_unique<rdesc::InputNode>();
    n->input = in;
    return g.addNode(std::move(n));
}
// lowerMaterial 已切 lux::cxx::expected；本测试的断言按 (graph, out&, err) out-param
// 形态写，用这个薄适配保留断言结构（全限定调用，避免下方 replace 改到自身）。
static bool lowerInto(const rdesc::MaterialGraph& g, sgm::LowerResult& out, std::string* err)
{
    auto e = lux::shadergen::material::lowerMaterial(g);
    if (!e) { if (err) *err = std::move(e.error()); return false; }
    out = std::move(*e);
    return true;
}

int main()
{
    std::printf("=== shadergen_lower_material_test ===\n");

    // A. walking skeleton：constant Vec3 -> base_color。
    {
        rdesc::MaterialGraph g;
        auto out = g.addNode(std::make_unique<rdesc::OutputSurfaceNode>());
        auto c   = addConst(g, rdesc::EMatValueType::Vec3, 1, 0, 0);
        const bool cok = g.connect(c, 0, out, ATTR_BASE_COLOR);
        sgm::LowerResult r; std::string err;
        const bool ok = lowerInto(g, r, &err);
        CHECK(cok, "A connect base_color ok");
        CHECK(ok, "A skeleton lowers ok");
        CHECK(r.ir.outputs.size() == static_cast<size_t>(rdesc::EMaterialAttribute::COUNT),
              "A outputs.size() == COUNT (7, 不随连接数变化)");
        const sg::Output* bc = findOutput(r.ir, "base_color");
        CHECK(bc && bc->value_id != sg::kNoValue, "A base_color bound (value_id != kNoValue)");
        CHECK(bc && bc->type == rdesc::EMatValueType::Vec3, "A base_color type Vec3");
    }

    // B. Output.dflt 自包含（issue#1 回归锁）：全默认图，未绑定输出携带契约默认值。
    {
        rdesc::MaterialGraph g;
        g.addNode(std::make_unique<rdesc::OutputSurfaceNode>());
        sgm::LowerResult r; std::string err;
        const bool ok = lowerInto(g, r, &err);
        CHECK(ok, "B all-default graph lowers ok");
        const sg::Output* bc = findOutput(r.ir, "base_color");
        CHECK(bc && bc->value_id == sg::kNoValue, "B unbound base_color -> kNoValue");
        CHECK(bc && bc->dflt[0] == 1.0f && bc->dflt[1] == 1.0f && bc->dflt[2] == 1.0f,
              "B base_color dflt 自包含 {1,1,1}（消费方无需回查契约）");
        const sg::Output* mt = findOutput(r.ir, "metallic");
        CHECK(mt && mt->dflt[0] == 0.0f, "B metallic dflt 0");
        const sg::Output* ro = findOutput(r.ir, "roughness");
        CHECK(ro && ro->dflt[0] == 1.0f, "B roughness dflt 1");
    }

    // C. InputSlot.location + 去重（issue#2 回归锁 + 输入槽语义）。
    {
        rdesc::MaterialGraph g;
        auto out = g.addNode(std::make_unique<rdesc::OutputSurfaceNode>());
        auto i1  = addInput(g, rdesc::EMaterialInput::WorldNormal);   // Vec3
        auto i2  = addInput(g, rdesc::EMaterialInput::WorldNormal);   // 同语义，第二个
        g.connect(i1, 0, out, ATTR_NORMAL_TS);  // Vec3 匹配
        g.connect(i2, 0, out, ATTR_EMISSIVE);   // Vec3 匹配
        sgm::LowerResult r; std::string err;
        const bool ok = lowerInto(g, r, &err);
        CHECK(ok, "C dual-WorldNormal lowers ok");
        int wn_cnt = 0; for (const auto& s : r.ir.inputs) if (s.name == "world_normal") ++wn_cnt;
        CHECK(wn_cnt == 1, "C WorldNormal 槽去重（用两次只建一个槽）");
        const sg::InputSlot* wn = findInput(r.ir, "world_normal");
        CHECK(wn && wn->location == 1, "C world_normal location==1（对齐引擎 vert）");
        CHECK(wn && wn->type == rdesc::EMatValueType::Vec3, "C world_normal type Vec3");
        CHECK(wn && wn->interpolation == sg::EInterpolation::Smooth, "C world_normal smooth");
    }

    // D. 隐式收窄：vec4 SampleTexture -> vec3 base_color 插入 Swizzle。
    {
        rdesc::MaterialGraph g;
        g.texture_slots.push_back({ "albedo" });
        auto out = g.addNode(std::make_unique<rdesc::OutputSurfaceNode>());
        auto s   = std::make_unique<rdesc::SampleTextureNode>(); s->texture_slot = 0;
        auto si  = g.addNode(std::move(s));
        g.connect(si, 0, out, ATTR_BASE_COLOR);  // Vec4 -> Vec3
        sgm::LowerResult r; std::string err;
        const bool ok = lowerInto(g, r, &err);
        CHECK(ok, "D vec4 sample -> vec3 base_color 隐式收窄 ok");
        CHECK(r.ir.textures.size() == 1 && r.ir.textures[0].name == "albedo", "D texture 槽复制");
        bool has_swizzle = false; for (const auto& v : r.ir.values) if (v.op == sg::EOp::Swizzle) has_swizzle = true;
        CHECK(has_swizzle, "D 隐式收窄插入 Swizzle");
    }

    // E. 错误路径（返回 false）。
    {
        rdesc::MaterialGraph g;  // 无 OutputSurface
        sgm::LowerResult r; std::string err;
        CHECK(!lowerInto(g, r, &err), "E 无 OutputSurface -> fail");
    }
    {
        rdesc::MaterialGraph g;
        g.addNode(std::make_unique<rdesc::OutputSurfaceNode>());
        g.addNode(std::make_unique<rdesc::OutputSurfaceNode>());
        sgm::LowerResult r; std::string err;
        CHECK(!lowerInto(g, r, &err), "E 多 OutputSurface -> fail");
    }
    {
        rdesc::MaterialGraph g;  // SampleTexture 但无 texture_slots
        auto out = g.addNode(std::make_unique<rdesc::OutputSurfaceNode>());
        auto s   = std::make_unique<rdesc::SampleTextureNode>(); s->texture_slot = 0;
        auto si  = g.addNode(std::move(s));
        g.connect(si, 0, out, ATTR_BASE_COLOR);
        sgm::LowerResult r; std::string err;
        CHECK(!lowerInto(g, r, &err), "E 未声明 texture 槽 -> fail");
    }

    // F. fingerprint：纯函数性 + shading_model 剥离契约。
    auto buildConst = [](rdesc::EMaterialShadingModel sm, int attr, rdesc::EMatValueType t, float v0) {
        rdesc::MaterialGraph g;
        g.shading_model = sm;
        auto out = g.addNode(std::make_unique<rdesc::OutputSurfaceNode>());
        auto cn  = std::make_unique<rdesc::ConstantNode>(); cn->setType(t); cn->value[0] = v0; cn->value[1] = v0; cn->value[2] = v0;
        auto c   = g.addNode(std::move(cn));
        g.connect(c, 0, out, attr);
        return g;
    };
    {
        rdesc::MaterialGraph ga = buildConst(rdesc::EMaterialShadingModel::PbrMetallicRoughness, ATTR_BASE_COLOR, rdesc::EMatValueType::Vec3, 0.5f);
        sgm::LowerResult r1, r2; std::string e;
        lowerInto(ga, r1, &e);
        lowerInto(ga, r2, &e);
        CHECK(r1.ir.fingerprint == r2.ir.fingerprint, "F 同图 -> 同 ir.fingerprint（确定性）");
        CHECK(r1.combined_fingerprint == r2.combined_fingerprint, "F 同图 -> 同 combined");

        rdesc::MaterialGraph gb = buildConst(rdesc::EMaterialShadingModel::Unlit, ATTR_BASE_COLOR, rdesc::EMatValueType::Vec3, 0.5f);
        sgm::LowerResult rb; lowerInto(gb, rb, &e);
        CHECK(r1.ir.fingerprint == rb.ir.fingerprint, "F shading_model 不进 ir.fingerprint（纯表达式）");
        CHECK(r1.combined_fingerprint != rb.combined_fingerprint, "F shading_model 进 combined_fingerprint");
        CHECK(rb.shading_model == rdesc::EMaterialShadingModel::Unlit, "F shading_model 在 LowerResult");
    }
    {
        // 仅 output 绑定不同（同一常量接不同属性）-> 不同 fingerprint。
        rdesc::MaterialGraph gm = buildConst(rdesc::EMaterialShadingModel::PbrMetallicRoughness, ATTR_METALLIC,  rdesc::EMatValueType::Float, 0.3f);
        rdesc::MaterialGraph gr = buildConst(rdesc::EMaterialShadingModel::PbrMetallicRoughness, ATTR_ROUGHNESS, rdesc::EMatValueType::Float, 0.3f);
        sgm::LowerResult rm, rr; std::string e;
        lowerInto(gm, rm, &e);
        lowerInto(gr, rr, &e);
        CHECK(rm.ir.fingerprint != rr.ir.fingerprint, "F 仅输出绑定不同 -> 不同 fingerprint");
    }

    // G. shading_model/render_state 在 LowerResult 而非 ShaderIR（结构层证明）。
    {
        rdesc::MaterialGraph g;
        g.shading_model = rdesc::EMaterialShadingModel::Stylized;
        g.render_state.alpha_mode = rdesc::EAlphaMode::Mask;
        g.render_state.alpha_cutoff = 0.25f;
        g.render_state.double_sided = true;
        g.addNode(std::make_unique<rdesc::OutputSurfaceNode>());
        sgm::LowerResult r; std::string err;
        lowerInto(g, r, &err);
        CHECK(r.shading_model == rdesc::EMaterialShadingModel::Stylized, "G shading_model 带出 LowerResult");
        CHECK(r.alpha_mode == rdesc::EAlphaMode::Mask && r.alpha_cutoff == 0.25f, "G render_state 带出 LowerResult");
        CHECK(r.double_sided == true, "G double_sided 带出 LowerResult");
    }

    std::printf("=== shadergen_lower_material_test %s (fails=%d) ===\n", g_fails ? "FAILED" : "PASSED", g_fails);
    return g_fails ? 1 : 0;
}
