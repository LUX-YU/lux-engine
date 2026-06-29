// ============================================================================
//  graph_forward_render_test.cpp  —  Forward GGX, live proof
//
//  Proves a material-graph-GENERATED FORWARD fragment shader lights with GGX
//  through the engine's real ForwardMesh pass:
//
//    graph (Param(tint)->BaseColor, PBR) -> compileToSpirv(Forward,+info)
//      -> session.compileShader() -> ShaderHandle
//      -> ForwardMeshCommConfig.graph_fragment (override, Graph family)
//    uploadGraphMaterial({tint=green}) -> Graph-family material
//      -> ForwardMesh draw -> SceneColor -> readback.
//
//  The forward frag bakes NO lighting result — it runs the inlined Cook-Torrance
//  BRDF over the set-3 light list. A green sphere with a brightness GRADIENT
//  (bright toward the light, dark away) can only come from real GGX shading; a
//  flat color would mean lighting didn't run.
//
//  Self-checking: 0 = PASS, 1 = FAIL, 0 (skip) if no Vulkan device.
// ============================================================================

#include <lux/engine/render/comm/client/RenderSession.hpp>
#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/render/comm/RenderProtocol.hpp>

#include <lux/engine/render/renderer/features/forward/ForwardMeshOperation.hpp>
#include <lux/engine/render/renderer/features/shadow/ShadowMapOperation.hpp>
#include <lux/engine/render/renderer/features/shadow/MeshShadowOperation.hpp>
#include <lux/engine/render/renderer/features/light/LightOperation.hpp>
#include <lux/engine/render/renderer/features/meshstack/MeshStackOperation.hpp>
#include <lux/engine/render/renderer/features/material/MaterialOperation.hpp>
#include <lux/engine/render/renderer/features/view_camera/ViewCameraOperation.hpp>
#include <lux/engine/render/core/LightDescriptor.hpp>
#include <lux/engine/render/resources/material/GraphMaterialData.hpp>

#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/description/Vertex.hpp>
#include <lux/engine/description/ShaderInfo.hpp>
#include <lux/engine/math/AABB.hpp>

#include <lux/engine/description/material_graph/MaterialGraph.hpp>
#include <lux/engine/description/material_graph/Nodes.hpp>
#include <lux/engine/description/MaterialGraphContract.hpp>
#include "graph_test_helpers.hpp"

#include <lux/engine/window/LuxWindow.hpp>
#include <GLFW/glfw3.h>

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace lux::render;
namespace rdesc = lux::rdesc;

static std::vector<const char*> getVulkanExtensions()
{
    glfwInit();
    uint32_t count = 0;
    const char** exts = glfwGetRequiredInstanceExtensions(&count);
    std::vector<const char*> result;
    result.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
        result.emplace_back(exts[i]);
    return result;
}

static Eigen::Matrix4f buildViewMatrix(const Eigen::Vector3f& eye,
                                       const Eigen::Vector3f& target,
                                       const Eigen::Vector3f& up)
{
    const Eigen::Vector3f f = (target - eye).normalized();
    const Eigen::Vector3f s = f.cross(up).normalized();
    const Eigen::Vector3f u = s.cross(f);
    Eigen::Matrix4f V = Eigen::Matrix4f::Identity();
    V(0,0)=s.x(); V(0,1)=s.y(); V(0,2)=s.z(); V(0,3)=-s.dot(eye);
    V(1,0)=u.x(); V(1,1)=u.y(); V(1,2)=u.z(); V(1,3)=-u.dot(eye);
    V(2,0)=-f.x();V(2,1)=-f.y();V(2,2)=-f.z();V(2,3)= f.dot(eye);
    return V;
}

static Eigen::Matrix4f buildProjMatrix(float fov_rad, float aspect, float near_z, float far_z)
{
    const float t = std::tan(fov_rad * 0.5f);
    Eigen::Matrix4f P = Eigen::Matrix4f::Zero();
    P(0,0) = 1.f / (aspect * t);
    P(1,1) = -1.f / t;
    P(2,2) = -far_z / (far_z - near_z);
    P(2,3) = -(far_z * near_z) / (far_z - near_z);
    P(3,2) = -1.f;
    return P;
}

static rdesc::Mesh buildSphereMesh(float radius, uint32_t segments, uint32_t rings)
{
    using V3 = Eigen::Vector3f;
    using V2 = Eigen::Vector2f;
    constexpr float kPi = 3.14159265358979323846f;

    std::vector<rdesc::Vertex> verts;
    std::vector<std::uint32_t>  indices;
    const auto stride = segments + 1;

    for (uint32_t y = 0; y <= rings; ++y)
    {
        const float v = static_cast<float>(y) / static_cast<float>(rings);
        const float phi = v * kPi;
        const float sp = std::sin(phi), cp = std::cos(phi);
        for (uint32_t x = 0; x <= segments; ++x)
        {
            const float u = static_cast<float>(x) / static_cast<float>(segments);
            const float th = u * 2.f * kPi;
            const float st = std::sin(th), ct = std::cos(th);
            const V3 n(sp * ct, cp, sp * st);
            const V3 t(-st, 0.f, ct);
            const V3 bt = n.cross(t).normalized();
            verts.push_back({n * radius, n, t, V2(u, v), bt});
        }
    }
    for (uint32_t y = 0; y < rings; ++y)
        for (uint32_t x = 0; x < segments; ++x)
        {
            const uint32_t i0 = y * stride + x, i1 = i0 + 1;
            const uint32_t i2 = i0 + stride,   i3 = i2 + 1;
            indices.insert(indices.end(), {i0, i1, i2, i2, i1, i3});
        }

    return rdesc::Mesh{
        std::move(verts), std::move(indices),
        lux::math::AABB(V3(-radius, -radius, -radius), V3(radius, radius, radius))
    };
}

// Forward PBR graph frag: a Vec3 param (slot 0) drives BaseColor; metallic 0,
// roughness 0.35; world-normal lit. The emitter inlines GGX over the set-3 lights.
static bool buildGraphForwardFrag(std::vector<uint32_t>& spirv,
                                  std::vector<std::byte>& info_bytes,
                                  std::string&            err)
{
    using namespace lux::rdesc;

    MaterialGraph g;  // PBR
    g.param_slots.push_back(ParamSlotDecl{ "tint", rdesc::EMatValueType::Vec3, { 1, 1, 1, 0 } });

    node_id tint = g.addNode(std::make_unique<ParamNode>(rdesc::EMatValueType::Vec3));
    static_cast<ParamNode*>(g.node(tint))->param_slot = 0;
    node_id metal = g.addNode(std::make_unique<ConstantNode>());
    { auto* c = static_cast<ConstantNode*>(g.node(metal)); c->value_type = rdesc::EMatValueType::Float; c->value[0] = 0.0f; }
    node_id rough = g.addNode(std::make_unique<ConstantNode>());
    { auto* c = static_cast<ConstantNode*>(g.node(rough)); c->value_type = rdesc::EMatValueType::Float; c->value[0] = 0.35f; }
    node_id nrm = g.addNode(std::make_unique<InputNode>());
    static_cast<InputNode*>(g.node(nrm))->input = rdesc::EMaterialInput::WorldNormal;

    node_id o = g.addNode(std::make_unique<OutputSurfaceNode>());
    g.connect(tint,  0, o, static_cast<uint32_t>(rdesc::EMaterialAttribute::BaseColor));
    g.connect(metal, 0, o, static_cast<uint32_t>(rdesc::EMaterialAttribute::Metallic));
    g.connect(rough, 0, o, static_cast<uint32_t>(rdesc::EMaterialAttribute::Roughness));
    g.connect(nrm,   0, o, static_cast<uint32_t>(rdesc::EMaterialAttribute::NormalTS));

    auto cs = lux::mgtest::compileGraph(g, rdesc::EMaterialPass::Forward);
    if (!cs) { err = cs.error(); return false; }
    spirv = std::move(cs->spirv);
    info_bytes = rdesc::ShaderInfo::serialize(cs->info);
    return true;
}

template <class T>
static T await(RenderSession& s, lux::window::LuxWindow& w, RenderRequest<T> req)
{
    s.submitFrame(/*blocking=*/true);
    s.waitAndPumpReplies();
    while (!req.isReady())
    {
        s.beginFrame({});
        w.pollEvents();
        s.submitFrame(/*blocking=*/true);
        s.waitAndPumpReplies();
    }
    return req.result();
}

int main()
{
    std::cout << std::unitbuf;
    std::cout << "=== graph_forward_render_test (Forward GGX) ===\n";

    std::vector<uint32_t> frag_spirv;
    std::vector<std::byte> frag_info;
    std::string gerr;
    if (!buildGraphForwardFrag(frag_spirv, frag_info, gerr))
    {
        std::cerr << "graph -> SPIR-V failed: " << gerr << "\n";
        return 1;
    }
    std::cout << "[0] graph forward frag generated (" << frag_spirv.size() << " words)\n";

    constexpr uint32_t W = 256, H = 256;

    auto channel = RenderProgramChannel<>::create();
    auto sync    = std::make_shared<RenderChannelSync>();
    auto exts    = getVulkanExtensions();

    lux::window::LuxWindow window(static_cast<int>(W), static_cast<int>(H), "graph_forward_render_test");

    std::atomic<bool> ready{false}, failed{false};
    std::thread server_thread([&]
    {
        GeneralRenderServer server(channel, sync);
        ServerConfig cfg;
        cfg.instance_extensions = exts;
        if (auto r = server.init(std::move(cfg)); !r)
        {
            std::cerr << "[Server] init failed: " << r.error().message() << "\n";
            failed.store(true, std::memory_order_release);
            ready.store(true, std::memory_order_release);
            return;
        }
        if (auto r = server.attachToWindow(window); !r)
        {
            std::cerr << "[Server] attach failed: " << r.error().message() << "\n";
            failed.store(true, std::memory_order_release);
            ready.store(true, std::memory_order_release);
            return;
        }
        ready.store(true, std::memory_order_release);
        try { while (server.tick()) {} }
        catch (const std::exception& e) { std::cerr << "[Server] tick threw: " << e.what() << "\n"; }
    });

    while (!ready.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (failed.load(std::memory_order_acquire))
    {
        std::cerr << "Server init failed (no Vulkan?). Skipping.\n";
        sync->requestStop();
        server_thread.join();
        return 0;
    }
    std::cout << "[1] server ready\n";

    RenderSession session(channel, sync);
    auto& s = session;
    auto& w = window;

    // LDR scene so the forward pass writes the readback target (SceneColor) directly.
    RenderSession::CreateSceneConfig scfg{};
    scfg.name             = "GraphForwardScene";
    scfg.lit_color_format = lux::common::ETextureFormat::RGBA8_SRGB;
    s.beginFrame({});
    const auto scene = await(s, w, s.createScene(scfg));
    s.beginFrame({});
    await(s, w, s.setActiveScene(scene.scene_id, true));
    s.beginFrame({});
    const auto view = await(s, w, s.addView(scene.scene_id, {W, H}, "GraphForwardView"));

    const auto frag_bytes = std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(frag_spirv.data()),
        frag_spirv.size() * sizeof(uint32_t)};
    s.beginFrame({});
    const auto compiled = await(s, w, s.compileShader(
        frag_bytes, std::span<const std::byte>{frag_info.data(), frag_info.size()}));
    std::cout << "[2] compileShader(graph forward frag) status=" << compiled.status << "\n";

    // LightFeature FIRST — owns this scene's per-scene LightResources (shadow +
    // forward consume it; ShadowMapFeature caches a raw LightResources* at attach,
    // so Light must attach before it).
    s.beginFrame({});
    const auto light_reg = await(s, w, s.registerFeatureType(kLightFeatureFactory));
    LightOperationIds light_ops = LightOperationIds::fromOps(light_reg.ops, light_reg.op_count);
    struct EmptyLightCfg {} light_cfg{};
    s.beginFrame({});
    await(s, w, s.addFeature(scene.scene_id, light_reg.feature_type_id, light_cfg));

    // StandardViewCamera owns the per-view camera matrices/frustum; MUST attach
    // before every camera consumer (mesh shadow / forward).
    s.beginFrame({});
    const auto view_cam_reg = await(s, w, s.registerFeatureType(lux::render::kStandardViewCameraFeatureFactory));
    lux::render::ViewCameraOperationIds view_cam_ops = lux::render::ViewCameraOperationIds::fromOps(view_cam_reg.ops, view_cam_reg.op_count);
    struct EmptyViewCamCfg {} view_cam_cfg{};
    s.beginFrame({});
    await(s, w, s.addFeature(scene.scene_id, view_cam_reg.feature_type_id, view_cam_cfg));

    // StandardMaterialFeature owns the standard material-stack resources (shading
    // model registry, material resources) that the mesh stack reads. Must attach
    // before StandardMeshStack.
    s.beginFrame({});
    const auto material_reg = await(s, w, s.registerFeatureType(lux::render::kStandardMaterialFeatureFactory));
    MaterialOperationIds material_ops = MaterialOperationIds::fromOps(material_reg.ops, material_reg.op_count);
    struct EmptyMaterialCfg {} material_cfg{};
    s.beginFrame({});
    await(s, w, s.addFeature(scene.scene_id, material_reg.feature_type_id, material_cfg));

    // StandardMeshStackFeature owns the standard 3D mesh-stack resources (cull
    // layout, instance buffers, …) that every mesh consumer reads. Must attach
    // before any mesh feature or nothing renders.
    s.beginFrame({});
    const auto mesh_stack_reg = await(s, w, s.registerFeatureType(lux::render::kStandardMeshStackFeatureFactory));
    MeshStackOperationIds mesh_stack_ops = MeshStackOperationIds::fromOps(mesh_stack_reg.ops, mesh_stack_reg.op_count);
    struct EmptyMeshStackCfg {} mesh_stack_cfg{};
    s.beginFrame({});
    await(s, w, s.addFeature(scene.scene_id, mesh_stack_reg.feature_type_id, mesh_stack_cfg));

    // Features: shadow + mesh-shadow (populate the set-3 light/shadow DS) + ForwardMesh.
    s.beginFrame({});
    const auto shadow_reg = await(s, w, s.registerFeatureType(kShadowMapFeatureFactory));
    s.beginFrame({});
    const auto mshsw_reg  = await(s, w, s.registerFeatureType(kMeshShadowFeatureFactory));
    s.beginFrame({});
    const auto fwd_reg    = await(s, w, s.registerFeatureType(kForwardMeshFeatureFactory));

    ShadowMapCommConfig scc{};
    scc.atlas_page_resolution  = 2048;
    scc.atlas_page_count       = 2;
    scc.max_shadow_slices      = 64;
    scc.enable_directional_csm = 1;
    scc.default_technique      = EShadowTechnique::PCF;
    s.beginFrame({});
    await(s, w, s.addFeature(scene.scene_id, shadow_reg.feature_type_id, scc));

    MeshShadowCommConfig mscc{};
    mscc.comm_config_version       = kMeshShadowCommConfigVersion;
    mscc.descriptor_layout_version = kMeshShadowDescriptorLayoutVersion;
    s.beginFrame({});
    await(s, w, s.addFeature(scene.scene_id, mshsw_reg.feature_type_id, mscc));

    // THE OVERRIDE: graph frag drives the GRAPH family forward pipeline.
    ForwardMeshCommConfig fcc{};
    fcc.comm_config_version       = kForwardMeshCommConfigVersion;
    fcc.descriptor_layout_version = kForwardMeshDescriptorLayoutVersion;
    fcc.graph_fragment            = compiled.shader;   // <-- Graph family forward frag
    s.beginFrame({});
    await(s, w, s.addFeature(scene.scene_id, fwd_reg.feature_type_id, fcc));
    std::cout << "[3] forward features added (graph-frag override)\n";

    // GREEN graph material (its tint param) + sphere.
    GraphMaterialData green{};
    green.param_count = 1;
    green.params[0][0] = 0.05f; green.params[0][1] = 0.85f; green.params[0][2] = 0.05f;
    s.beginFrame({});
    const auto mat = await(s, w, MaterialProxy(s, material_ops).uploadGraphMaterial(green));

    const rdesc::Mesh sphere = buildSphereMesh(0.5f, 48, 24);
    s.beginFrame({});
    const auto mesh = await(s, w, lux::render::MeshStackProxy(s, mesh_stack_ops).uploadMesh(sphere));
    std::cout << "[4] graph material(status=" << mat.status << ") + mesh(status=" << mesh.status << ")\n";

    float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    s.beginFrame({});
    const auto inst = await(s, w, MeshStackProxy(s, mesh_stack_ops).addMeshInstance(scene.scene_id, mesh.handle, mat.handle, identity));
    s.beginFrame({});
    MeshStackProxy(s, mesh_stack_ops).makeInstanceVisibleForView(scene.scene_id, view.view, inst.object);

    DirectionalLightDesc dl{};
    dl.direction = Eigen::Vector3f(-0.4f, -0.8f, -0.45f).normalized();
    dl.color     = Eigen::Vector3f(1.f, 0.97f, 0.92f);
    dl.intensity = 3.0f;
    dl.flags     = LIGHT_FLAG_CAST_SHADOW;
    await(s, w, LightProxy(s, light_ops).createLight(scene.scene_id, LightDescriptor{dl}));

    const Eigen::Vector3f center(0.f, 0.f, 0.f);
    const float radius = 0.5f * std::sqrt(3.f);
    const float fov    = 45.f * 3.14159265f / 180.f;
    const float dist   = (radius / std::sin(fov * 0.5f)) * 1.25f;
    const Eigen::Vector3f eye = center + Eigen::Vector3f(0.6f, 0.45f, 1.0f).normalized() * dist;
    const Eigen::Matrix4f V = buildViewMatrix(eye, center, Eigen::Vector3f(0, 1, 0));
    const Eigen::Matrix4f P = buildProjMatrix(fov, static_cast<float>(W) / H, 0.05f, 50.f);

    for (int i = 0; i < 20; ++i)
    {
        s.beginFrame({});
        w.pollEvents();
        lux::render::ViewCameraProxy(s, view_cam_ops).update(scene.scene_id, view.view, V.data(), P.data(), eye.data());
        s.submitFrame(/*blocking=*/true);
        s.waitAndPumpReplies();
    }
    std::cout << "[5] rendered warm-up frames\n";

    std::vector<std::uint8_t> px(static_cast<std::size_t>(W) * H * 4, 0);
    s.beginFrame({});
    const auto rb = await(s, w, s.readbackView(scene.scene_id, view.view, px.data(), px.size()));
    std::cout << "[6] readback status=" << rb.status << " " << rb.width << "x" << rb.height << "\n";

    std::size_t bright = 0, green_dom = 0;
    int g_min = 255, g_max = 0;
    for (std::size_t i = 0; i + 4 <= px.size(); i += 4)
    {
        const int b = px[i], g = px[i + 1], r = px[i + 2];
        if (std::max({b, g, r}) > 40) ++bright;
        if (g > 60 && g > r + 25 && g > b + 25)
        {
            ++green_dom;
            g_min = std::min(g_min, g);
            g_max = std::max(g_max, g);
        }
    }
    std::cout << "  bright=" << bright << "  green_dominant=" << green_dom
              << "  green range=[" << g_min << "," << g_max << "]\n";

    int fails = 0;
    auto check = [&](bool cond, const char* name)
    { std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << "\n"; if (!cond) ++fails; };

    check(compiled.status == 0, "graph forward frag compiled to a server ShaderHandle");
    check(mat.status == 0,      "graph material uploaded");
    check(rb.status == 0,       "readback status == 0");
    check(bright > 300,         "object is visible");
    check(green_dom > 200,      "object is GREEN (the graph tint param drove BaseColor)");
    // GGX shading => a brightness gradient across the lit sphere (NdotL falloff +
    // specular highlight). A flat color would mean lighting never ran.
    check((g_max - g_min) > 40, "green has a brightness gradient => GGX lighting ran");

    sync->requestStop();
    server_thread.join();

    std::cout << "=== graph_forward_render_test " << (fails == 0 ? "PASSED" : "FAILED")
              << " (fails=" << fails << ") ===\n";
    return fails == 0 ? 0 : 1;
}
