// ============================================================================
//  graph_distinct_pso_render_test.cpp  —  R1 proof: per-material graph PSOs
//
//  Proves the R1 milestone: TWO DISTINCT graph materials, each carrying its OWN
//  baked frag shader, render with DISTINCT pipelines in ONE frame — which the
//  pre-R1 single-Config-override conduit could NOT do.
//
//    graph(RED)   -> compileToSpirv(GBuffer) -> compileShader -> ShaderHandle_R
//    graph(GREEN) -> compileToSpirv(GBuffer) -> compileShader -> ShaderHandle_G
//    uploadGraphMaterial(data, ShaderHandle_R, {})  -> matRED   (its own bucket/PSO)
//    uploadGraphMaterial(data, ShaderHandle_G, {})  -> matGREEN (its own bucket/PSO)
//    two sphere instances (left=matRED, right=matGREEN) -> deferred render
//    -> readback -> assert the two halves are DIFFERENT colours (RED vs GREEN).
//
//  NOTE: NO DeferredGBufferCommConfig graph override is set. Each material's frag
//  arrives via the R1 uploadGraphMaterial(data, gbuffer, forward) overload; the
//  feature builds a per-bucket PSO from it (registerGraphBucketPipelines). A
//  two-colour result can ONLY come from two distinct PSOs.
//
//  Self-checking: 0 = PASS, 1 = FAIL, 0 (skip) if no Vulkan device.
// ============================================================================

#include <lux/engine/render/comm/client/RenderSession.hpp>
#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/render/comm/RenderProtocol.hpp>

#include <lux/engine/render/renderer/features/deffer/DeferredGBufferOperation.hpp>
#include <lux/engine/render/renderer/features/deffer/DeferredLightingOperation.hpp>
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

// Build a GBuffer frag from a graph that bakes a CONSTANT colour (ignores any
// per-material data). The colour is baked into the SHADER, so two colours == two
// distinct shaders == (under R1) two distinct PSOs.
static bool buildGraphGBufferFrag(const Eigen::Vector3f& color,
                                  std::vector<uint32_t>& spirv,
                                  std::vector<std::byte>& info_bytes,
                                  std::string&            err)
{
    using namespace lux::rdesc;

    MaterialGraph g;
    g.shading_model = rdesc::EMaterialShadingModel::Unlit;

    node_id c_base = g.addNode(std::make_unique<ConstantNode>());
    {
        auto* cn = static_cast<ConstantNode*>(g.node(c_base));
        cn->value_type = rdesc::EMatValueType::Vec3;
        cn->value[0] = color.x(); cn->value[1] = color.y(); cn->value[2] = color.z();
    }
    node_id c_emis = g.addNode(std::make_unique<ConstantNode>());
    {
        auto* cn = static_cast<ConstantNode*>(g.node(c_emis));
        cn->value_type = rdesc::EMatValueType::Vec3;
        cn->value[0] = color.x() * 3.f; cn->value[1] = color.y() * 3.f; cn->value[2] = color.z() * 3.f;
    }
    node_id o = g.addNode(std::make_unique<OutputSurfaceNode>());
    g.connect(c_base, 0, o, static_cast<uint32_t>(rdesc::EMaterialAttribute::BaseColor));
    g.connect(c_emis, 0, o, static_cast<uint32_t>(rdesc::EMaterialAttribute::Emissive));

    auto cs = lux::mgtest::compileGraph(g, rdesc::EMaterialPass::GBuffer);
    if (!cs) { err = cs.error(); return false; }
    spirv = std::move(cs->spirv);

    rdesc::ShaderInfo si;
    si.entry_points.push_back(rdesc::EntryPointInfo{std::string("main"), rdesc::EShaderType::FRAGMENT});
    info_bytes = rdesc::ShaderInfo::serialize(si);
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

// Classify a WxH-pixel rectangle of the readback (BGRA8): how many pixels are
// red-dominant vs green-dominant vs bright.
struct RegionStats { std::size_t bright{0}, red{0}, green{0}; };
static RegionStats classifyRegion(const std::vector<std::uint8_t>& px, uint32_t W,
                                  uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    RegionStats r;
    for (uint32_t y = y0; y < y1; ++y)
        for (uint32_t x = x0; x < x1; ++x)
        {
            const std::size_t i = (static_cast<std::size_t>(y) * W + x) * 4;
            const int b = px[i], g = px[i+1], rd = px[i+2];
            if (std::max({b, g, rd}) > 40) ++r.bright;
            if (rd > 60 && rd > g + 30 && rd > b + 30) ++r.red;
            if (g  > 60 && g  > rd + 30 && g  > b + 30) ++r.green;
        }
    return r;
}

int main()
{
    std::cout << std::unitbuf;
    std::cout << "=== graph_distinct_pso_render_test (R1) ===\n";

    // ---- 0. Two graph GBuffer frags: RED and GREEN (CPU-side, no device) ----
    std::vector<uint32_t> spirv_r, spirv_g;
    std::vector<std::byte> info_r, info_g;
    std::string gerr;
    if (!buildGraphGBufferFrag(Eigen::Vector3f(0.90f, 0.04f, 0.04f), spirv_r, info_r, gerr) ||
        !buildGraphGBufferFrag(Eigen::Vector3f(0.04f, 0.90f, 0.04f), spirv_g, info_g, gerr))
    {
        std::cerr << "graph -> SPIR-V failed: " << gerr << "\n";
        return 1;
    }
    std::cout << "[0] two graph frags generated (R=" << spirv_r.size()
              << " words, G=" << spirv_g.size() << " words)\n";

    constexpr uint32_t W = 256, H = 256;

    auto channel = RenderProgramChannel<>::create();
    auto sync    = std::make_shared<RenderChannelSync>();
    auto exts    = getVulkanExtensions();

    lux::window::LuxWindow window(static_cast<int>(W), static_cast<int>(H), "graph_distinct_pso_render_test");

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

    // ---- 2. Scene (LDR) + offscreen view ----
    RenderSession::CreateSceneConfig scfg{};
    scfg.name             = "GraphDistinctPsoScene";
    scfg.lit_color_format = lux::common::ETextureFormat::RGBA8_SRGB;
    s.beginFrame({});
    const auto scene = await(s, w, s.createScene(scfg));
    s.beginFrame({});
    await(s, w, s.setActiveScene(scene.scene_id, true));
    s.beginFrame({});
    const auto view = await(s, w, s.addView(scene.scene_id, {W, H}, "GraphView"));

    // ---- 3. Compile both graph frags into server-side ShaderHandles ----
    auto compileFrag = [&](const std::vector<uint32_t>& spv, const std::vector<std::byte>& info)
    {
        const auto bytes = std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(spv.data()), spv.size() * sizeof(uint32_t)};
        s.beginFrame({});
        return await(s, w, s.compileShader(bytes, std::span<const std::byte>{info.data(), info.size()}));
    };
    const auto compiled_r = compileFrag(spirv_r, info_r);
    const auto compiled_g = compileFrag(spirv_g, info_g);
    std::cout << "[2] compileShader R status=" << compiled_r.status
              << " G status=" << compiled_g.status << "\n";

    // ---- 4. Deferred feature set. NO graph Config override — each material
    //         supplies its own frag via uploadGraphMaterial (R1). ----
    // LightFeature MUST be registered + added before ShadowMapFeature: ShadowMap
    // caches a raw LightResources* at attach, so Light must attach first.
    s.beginFrame({});
    const auto light_feat_reg = await(s, w, s.registerFeatureType(kLightFeatureFactory));
    LightOperationIds light_ops = LightOperationIds::fromOps(light_feat_reg.ops, light_feat_reg.op_count);
    struct EmptyLightCfg {} light_cfg{};
    s.beginFrame({});
    await(s, w, s.addFeature(scene.scene_id, light_feat_reg.feature_type_id, light_cfg));

    // StandardViewCamera owns the per-view camera matrices/frustum; MUST attach
    // before every camera consumer (mesh shadow / deferred / lighting).
    s.beginFrame({});
    const auto view_cam_reg = await(s, w, s.registerFeatureType(lux::render::kStandardViewCameraFeatureFactory));
    lux::render::ViewCameraOperationIds view_cam_ops = lux::render::ViewCameraOperationIds::fromOps(view_cam_reg.ops, view_cam_reg.op_count);
    struct EmptyViewCamCfg {} view_cam_cfg{};
    s.beginFrame({});
    await(s, w, s.addFeature(scene.scene_id, view_cam_reg.feature_type_id, view_cam_cfg));

    s.beginFrame({});
    const auto material_reg = await(s, w, s.registerFeatureType(lux::render::kStandardMaterialFeatureFactory));
    MaterialOperationIds material_ops = MaterialOperationIds::fromOps(material_reg.ops, material_reg.op_count);
    struct EmptyMaterialCfg {} material_cfg{};
    s.beginFrame({});
    await(s, w, s.addFeature(scene.scene_id, material_reg.feature_type_id, material_cfg));

    s.beginFrame({});
    const auto mesh_stack_reg = await(s, w, s.registerFeatureType(lux::render::kStandardMeshStackFeatureFactory));
    MeshStackOperationIds mesh_stack_ops = MeshStackOperationIds::fromOps(mesh_stack_reg.ops, mesh_stack_reg.op_count);
    struct EmptyMeshStackCfg {} mesh_stack_cfg{};
    s.beginFrame({});
    await(s, w, s.addFeature(scene.scene_id, mesh_stack_reg.feature_type_id, mesh_stack_cfg));

    s.beginFrame({});
    const auto shadow_reg = await(s, w, s.registerFeatureType(kShadowMapFeatureFactory));
    s.beginFrame({});
    const auto mshsw_reg  = await(s, w, s.registerFeatureType(kMeshShadowFeatureFactory));
    s.beginFrame({});
    const auto gbuf_reg   = await(s, w, s.registerFeatureType(kDeferredGBufferFeatureFactory));
    s.beginFrame({});
    const auto light_reg  = await(s, w, s.registerFeatureType(kDeferredLightingFeatureFactory));

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

    DeferredGBufferCommConfig gbcc{};
    gbcc.comm_config_version       = kDeferredGBufferCommConfigVersion;
    gbcc.descriptor_layout_version = kDeferredGBufferDescriptorLayoutVersion;
    // gbcc.gbuffer_graph_fragment_shader intentionally left null: R1 routes each
    // material's own frag via its variant bucket, not this single override.
    s.beginFrame({});
    await(s, w, s.addFeature(scene.scene_id, gbuf_reg.feature_type_id, gbcc));

    DeferredLightingCommConfig dlcc{};
    dlcc.read_mode           = 0;
    dlcc.enable_clustered    = 1;
    dlcc.cluster_x           = 16;
    dlcc.cluster_y           = 9;
    dlcc.cluster_z           = 24;
    dlcc.max_cluster_indices = 1'048'576;
    dlcc.technique           = EShadowTechnique::PCF;
    s.beginFrame({});
    await(s, w, s.addFeature(scene.scene_id, light_reg.feature_type_id, dlcc));
    std::cout << "[3] deferred features added (no graph Config override)\n";

    // ---- 5. Two graph materials, each carrying its OWN baked frag (R1) ----
    GraphMaterialData data_r{};   // constant-colour frag ignores params; default is fine
    GraphMaterialData data_g{};
    s.beginFrame({});
    const auto mat_r = await(s, w, MaterialProxy(s, material_ops).uploadGraphMaterial(data_r, compiled_r.shader, ShaderHandle{}));
    s.beginFrame({});
    const auto mat_g = await(s, w, MaterialProxy(s, material_ops).uploadGraphMaterial(data_g, compiled_g.shader, ShaderHandle{}));
    std::cout << "[4] graph materials uploaded: R(status=" << mat_r.status
              << ") G(status=" << mat_g.status << ")\n";

    const rdesc::Mesh sphere = buildSphereMesh(0.5f, 48, 24);
    s.beginFrame({});
    const auto mesh = await(s, w, lux::render::MeshStackProxy(s, mesh_stack_ops).uploadMesh(sphere));

    // ---- 6. Two instances: left = RED material, right = GREEN material ----
    // Column-major 4x4; translation in elements [12..14].
    float xform_l[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, -0.7f,0,0,1};
    float xform_r[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0,  0.7f,0,0,1};
    s.beginFrame({});
    const auto inst_l = await(s, w, MeshStackProxy(s, mesh_stack_ops).addMeshInstance(scene.scene_id, mesh.handle, mat_r.handle, xform_l));
    s.beginFrame({});
    const auto inst_r = await(s, w, MeshStackProxy(s, mesh_stack_ops).addMeshInstance(scene.scene_id, mesh.handle, mat_g.handle, xform_r));
    s.beginFrame({});
    MeshStackProxy(s, mesh_stack_ops).makeInstanceVisibleForView(scene.scene_id, view.view, inst_l.object);
    MeshStackProxy(s, mesh_stack_ops).makeInstanceVisibleForView(scene.scene_id, view.view, inst_r.object);

    DirectionalLightDesc dl{};
    dl.direction = Eigen::Vector3f(-0.4f, -0.8f, -0.45f).normalized();
    dl.color     = Eigen::Vector3f(1.f, 0.97f, 0.92f);
    dl.intensity = 3.0f;
    dl.flags     = LIGHT_FLAG_CAST_SHADOW;
    await(s, w, LightProxy(s, light_ops).createLight(scene.scene_id, LightDescriptor{dl}));

    // ---- 7. Camera framing both spheres + warm-up ----
    const Eigen::Vector3f eye(0.f, 0.f, 4.f), center(0.f, 0.f, 0.f);
    const float fov = 45.f * 3.14159265f / 180.f;
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

    // ---- 8. Readback + analyse the two halves ----
    std::vector<std::uint8_t> px(static_cast<std::size_t>(W) * H * 4, 0);
    s.beginFrame({});
    const auto rb = await(s, w, s.readbackView(scene.scene_id, view.view, px.data(), px.size()));
    std::cout << "[6] readback status=" << rb.status << " " << rb.width << "x" << rb.height << "\n";

    // Left sphere ~ screen x≈0.29W, right ~ 0.71W; both vertically centred.
    const RegionStats L = classifyRegion(px, W, W*7/24, H*7/16, W*11/24, H*9/16);
    const RegionStats R = classifyRegion(px, W, W*13/24, H*7/16, W*17/24, H*9/16);
    std::cout << "  LEFT  region: bright=" << L.bright << " red=" << L.red << " green=" << L.green << "\n";
    std::cout << "  RIGHT region: bright=" << R.bright << " red=" << R.red << " green=" << R.green << "\n";

    const bool left_red    = L.red  > 30 && L.red  > L.green * 3;
    const bool left_green  = L.green > 30 && L.green > L.red  * 3;
    const bool right_red   = R.red  > 30 && R.red  > R.green * 3;
    const bool right_green = R.green > 30 && R.green > R.red  * 3;

    int fails = 0;
    auto check = [&](bool cond, const char* name)
    { std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << "\n"; if (!cond) ++fails; };

    check(compiled_r.status == 0 && compiled_g.status == 0, "both graph frags compiled to ShaderHandles");
    check(rb.status == 0,                                   "readback status == 0");
    check(L.bright > 100 && R.bright > 100,                  "both spheres rendered (left+right visible)");
    // THE PROOF: the two halves are DIFFERENT solid colours -> two distinct PSOs.
    check((left_red && right_green) || (left_green && right_red),
          "left and right render DIFFERENT colours (RED vs GREEN) => distinct per-material PSOs");

    sync->requestStop();
    server_thread.join();

    std::cout << "=== graph_distinct_pso_render_test " << (fails == 0 ? "PASSED" : "FAILED")
              << " (fails=" << fails << ") ===\n";
    return fails == 0 ? 0 : 1;
}
