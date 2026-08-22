// ============================================================================
//  graph_params_render_test.cpp  —  B2: per-material params, live proof
//
//  Proves PER-INSTANCE material parameters end to end: TWO instances of the
//  same mesh, sharing ONE graph-generated fragment shader, render in DIFFERENT
//  colors because each instance carries its own param values in the Graph-family
//  SSBO (set 4, binding 4), indexed by vMatIndex.
//
//    graph (Param(slot0)->Emissive) -> compileToSpirv(GBuffer,+ShaderInfo)
//      -> session.compileShader() -> ShaderHandle
//      -> DeferredGBufferCommConfig.gbuffer_graph_fragment_shader (override)
//    uploadGraphMaterial({tint=red})  -> matRed   (Graph family slot 0)
//    uploadGraphMaterial({tint=green}) -> matGreen (Graph family slot 1)
//      -> two sphere instances (left=matRed, right=matGreen), one frag
//      -> deferred -> readback: left half RED, right half GREEN.
//
//  The frag bakes NO color — it reads _mat.params[0].xyz. The only way the left
//  sphere is red and the right is green with the SAME shader is that the engine
//  fed each instance its own params. That is the per-material-params feature.
//
//  Self-checking: 0 = PASS, 1 = FAIL, 0 (skip) if no Vulkan device.
// ============================================================================

#include <lux/engine/function/render/client/RenderFrameSession.hpp>
#include <lux/engine/function/render/client/RenderControlSession.hpp>
#include <lux/engine/function/render/client/RenderUploadSession.hpp>
#include <lux/engine/render/testing/DirectRenderUploadClient.hpp>
#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/function/render/client/RenderProtocol.hpp>

#include <lux/engine/function/render/client/genops/DeferredGBufferOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/DeferredLightingOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/ShadowMapOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MeshShadowOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/LightOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MeshStackOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MaterialOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/ViewCameraOperation.ops.hpp>
#include <lux/engine/function/render/client/resources/lighting/LightDescriptor.hpp>
#include <lux/engine/function/render/client/resources/material/GraphMaterialData.hpp>

#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/description/Vertex.hpp>
#include <lux/engine/description/ShaderInfo.hpp>
#include <lux/engine/math/AABB.hpp>

#include <lux/engine/authoring/assets/material/MaterialGraph.hpp>
#include <lux/engine/authoring/assets/material/Nodes.hpp>
#include <lux/engine/description/MaterialGraphContract.hpp>
#include "graph_test_helpers.hpp"

#include <lux/engine/window/LuxWindow.hpp>

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
    const auto exts = lux::window::LuxWindow::requiredVulkanInstanceExtensions();
    return {exts.begin(), exts.end()};
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

// GBuffer frag from a graph that reads a per-material Vec3 param (slot 0) and
// routes it to Emissive (Unlit -> emissive passthrough makes it directly
// visible). BaseColor = const black, so the only on-screen color is the param.
static bool buildGraphParamFrag(std::vector<uint32_t>& spirv,
                                std::vector<std::byte>& info_bytes,
                                std::string&            err)
{
    using namespace lux::rdesc;

    MaterialGraph g;
    g.shading_model = rdesc::EMaterialShadingModel::Unlit;
    g.param_slots.push_back(ParamSlotDecl{ "tint", rdesc::EMatValueType::Vec3, { 1, 1, 1, 0 } });

    node_id black = g.addNode(std::make_unique<ConstantNode>());
    {
        auto* cn = static_cast<ConstantNode*>(g.node(black));
        cn->value_type = rdesc::EMatValueType::Vec3;
        cn->value[0] = 0.f; cn->value[1] = 0.f; cn->value[2] = 0.f;
    }
    node_id tint = g.addNode(std::make_unique<ParamNode>(rdesc::EMatValueType::Vec3));
    static_cast<ParamNode*>(g.node(tint))->param_slot = 0;

    node_id o = g.addNode(std::make_unique<OutputSurfaceNode>());
    g.connect(black, 0, o, static_cast<uint32_t>(rdesc::EMaterialAttribute::BaseColor));
    g.connect(tint,  0, o, static_cast<uint32_t>(rdesc::EMaterialAttribute::Emissive));

    auto cs = lux::mgtest::compileGraph(g, rdesc::EMaterialPass::GBuffer);
    if (!cs) { err = cs.error(); return false; }
    spirv = std::move(cs->spirv);
    info_bytes = rdesc::ShaderInfo::serialize(cs->info);
    return true;
}

template <class Session, class T>
static T await(Session& endpoint, lux::window::LuxWindow& window, RenderRequest<T> request)
{
    if constexpr (std::is_same_v<Session, RenderFrameSession>)
        endpoint.trySubmitFrame();

    while (!request.isReady())
    {
        window.pollEvents();
        if (!endpoint.waitAndPumpReplies())
            break;
        if constexpr (std::is_same_v<Session, RenderFrameSession>)
        {
            if (!request.isReady())
            {
                endpoint.beginFrame({});
                endpoint.trySubmitFrame();
            }
        }
    }
    return request.tryResult()->get();
}

template <class T>
static T await(
    RenderUploadSession& endpoint,
    lux::window::LuxWindow& window,
    UploadSubmitResult<T> submitted)
{
    if (!submitted)
        return {};
    return await(endpoint, window, std::move(submitted.value()));
}
int main()
{
    std::cout << std::unitbuf;
    std::cout << "=== graph_params_render_test (B2: per-instance params) ===\n";

    // ---- 0. Graph param frag (CPU-side) ----
    std::vector<uint32_t> frag_spirv;
    std::vector<std::byte> frag_info;
    std::string gerr;
    if (!buildGraphParamFrag(frag_spirv, frag_info, gerr))
    {
        std::cerr << "graph -> SPIR-V failed: " << gerr << "\n";
        return 1;
    }
    std::cout << "[0] graph param frag generated (" << frag_spirv.size() << " words)\n";

    constexpr uint32_t W = 256, H = 256;

    auto channel = RenderFrameChannel<>::create();
    auto control_channel = RenderControlChannel<>::create();
    auto upload_channel = RenderUploadChannel<>::create();
    auto sync    = std::make_shared<RenderChannelSync>();
    auto exts    = getVulkanExtensions();

    lux::window::LuxWindow window(static_cast<int>(W), static_cast<int>(H), "graph_params_render_test");

    std::atomic<bool> ready{false}, failed{false};
    std::thread server_thread([&]
    {
        GeneralRenderServer server(channel, control_channel, upload_channel, sync);
        ServerConfig cfg;
        cfg.instance_extensions = exts;
        if (auto r = server.init(std::move(cfg)); !r)
        {
            std::cerr << "[Server] init failed: " << formatRenderError(renderErrorRegistry(), r.error()) << "\n";
            failed.store(true, std::memory_order_release);
            ready.store(true, std::memory_order_release);
            return;
        }
        if (auto r = server.attachToWindow(window); !r)
        {
            std::cerr << "[Server] attach failed: " << formatRenderError(renderErrorRegistry(), r.error()) << "\n";
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

    RenderFrameSession session(channel, sync);
    RenderControlSession control(control_channel, sync);
    RenderUploadSession upload(upload_channel, sync);
    lux::render::testing::DirectRenderUploadClient upload_client{upload};
    auto& s = session;
    auto& w = window;

    // ---- 2. Scene (LDR) + offscreen view ----
    RenderControlSession::CreateSceneConfig scfg{};
    scfg.name             = "GraphParamsScene";
    scfg.lit_color_format = lux::rdesc::ETextureFormat::RGBA8_SRGB;
    s.beginFrame({});
    const auto scene = await(control, w, control.createScene(scfg));
    s.beginFrame({});
    await(control, w, control.setActiveScene(scene.scene_id, true));
    s.beginFrame({});
    const auto view = await(control, w, control.addView(scene.scene_id, {W, H}, "GraphParamsView"));
    s.beginFrame({});
    const auto target = await(control, w, control.createOffscreenRenderTarget({W, H}));
    s.beginFrame({});
    control.setLayer(target.target, 0, scene.scene_id, view.view);
    s.trySubmitFrame();
    s.waitAndPumpReplies();

    // ---- 3. Compile graph frag -> ShaderHandle ----
    const auto frag_bytes = std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(frag_spirv.data()),
        frag_spirv.size() * sizeof(uint32_t)};
    s.beginFrame({});
    const auto compiled = await(control, w, control.compileShader(
        frag_bytes, std::span<const std::byte>{frag_info.data(), frag_info.size()}));
    std::cout << "[2] compileShader(graph param frag) status=" << compiled.status << "\n";

    // ---- 4. Deferred feature set (graph frag override on the Graph family) ----
    // LightFeature FIRST — owns this scene's per-scene LightResources (shadow +
    // lighting consume it; ShadowMapFeature caches a raw LightResources* at attach,
    // so Light must attach before it).
    s.beginFrame({});
    const auto light_feat_reg = await(control, w, control.registerFeatureType(kLightFeatureFactory));
    LightOperationIds light_ops = LightOperationIds::fromOps(light_feat_reg.ops, light_feat_reg.op_count);
    lux::render::LightCommTag light_cfg{};
    s.beginFrame({});
    await(control, w, control.addFeature(scene.scene_id, light_feat_reg.feature_type_id, light_cfg));

    // StandardViewCamera owns the per-view camera matrices/frustum; MUST attach
    // before every camera consumer (mesh shadow / deferred / lighting).
    s.beginFrame({});
    const auto view_cam_reg = await(control, w, control.registerFeatureType(lux::render::kViewCameraFeatureFactory));
    lux::render::ViewCameraOperationIds view_cam_ops = lux::render::ViewCameraOperationIds::fromOps(view_cam_reg.ops, view_cam_reg.op_count);
    lux::render::ViewCameraCommTag view_cam_cfg{};
    s.beginFrame({});
    await(control, w, control.addFeature(scene.scene_id, view_cam_reg.feature_type_id, view_cam_cfg));

    // StandardMaterialFeature owns the standard material-stack resources (shading
    // model registry, material resources) that the mesh stack reads. Must attach
    // before StandardMeshStack.
    s.beginFrame({});
    const auto material_reg = await(control, w, control.registerFeatureType(lux::render::kMaterialFeatureFactory));
    MaterialOperationIds material_ops = MaterialOperationIds::fromOps(material_reg.ops, material_reg.op_count);
    lux::render::MaterialCommTag material_cfg{};
    s.beginFrame({});
    await(control, w, control.addFeature(scene.scene_id, material_reg.feature_type_id, material_cfg));

    // StandardMeshStackFeature owns the standard 3D mesh-stack resources (cull
    // layout, instance buffers, …) that every mesh consumer reads. Must attach
    // before any mesh feature or nothing renders.
    s.beginFrame({});
    const auto mesh_stack_reg = await(control, w, control.registerFeatureType(lux::render::kMeshStackFeatureFactory));
    MeshStackOperationIds mesh_stack_ops = MeshStackOperationIds::fromOps(mesh_stack_reg.ops, mesh_stack_reg.op_count);
    lux::render::MeshStackCommTag mesh_stack_cfg{};
    s.beginFrame({});
    await(control, w, control.addFeature(scene.scene_id, mesh_stack_reg.feature_type_id, mesh_stack_cfg));

    s.beginFrame({});
    const auto shadow_reg = await(control, w, control.registerFeatureType(kShadowMapFeatureFactory));
    s.beginFrame({});
    const auto mshsw_reg  = await(control, w, control.registerFeatureType(kMeshShadowFeatureFactory));
    s.beginFrame({});
    const auto gbuf_reg   = await(control, w, control.registerFeatureType(kDeferredGBufferFeatureFactory));
    s.beginFrame({});
    const auto light_reg  = await(control, w, control.registerFeatureType(kDeferredLightingFeatureFactory));

    ShadowMapCommConfig scc{};
    scc.atlas_page_resolution  = 2048;
    scc.atlas_page_count       = 2;
    scc.max_shadow_slices      = 64;
    scc.enable_directional_csm = 1;
    scc.default_technique      = EShadowTechnique::PCF;
    s.beginFrame({});
    await(control, w, control.addFeature(scene.scene_id, shadow_reg.feature_type_id, scc));

    MeshShadowCommConfig mscc{};
    mscc.comm_config_version       = kMeshShadowCommConfigVersion;
    mscc.descriptor_layout_version = kMeshShadowDescriptorLayoutVersion;
    s.beginFrame({});
    await(control, w, control.addFeature(scene.scene_id, mshsw_reg.feature_type_id, mscc));

    // THE OVERRIDE: graph frag drives the GRAPH family gbuffer pipeline.
    DeferredGBufferCommConfig gbcc{};
    gbcc.comm_config_version           = kDeferredGBufferCommConfigVersion;
    gbcc.descriptor_layout_version     = kDeferredGBufferDescriptorLayoutVersion;
    gbcc.gbuffer_graph_fragment_shader = compiled.shader;   // <-- Graph family frag
    s.beginFrame({});
    await(control, w, control.addFeature(scene.scene_id, gbuf_reg.feature_type_id, gbcc));

    DeferredLightingCommConfig dlcc{};
    dlcc.read_mode           = lux::render::ELightingReadMode::SAMPLED;
    dlcc.enable_clustered    = 1;
    dlcc.cluster_x           = 16;
    dlcc.cluster_y           = 9;
    dlcc.cluster_z           = 24;
    dlcc.max_cluster_indices = 1'048'576;
    dlcc.technique           = EShadowTechnique::PCF;
    s.beginFrame({});
    await(control, w, control.addFeature(scene.scene_id, light_reg.feature_type_id, dlcc));
    std::cout << "[3] deferred features added (graph-frag override)\n";

    // ---- 5. TWO graph materials: red + green tint, SAME frag ----
    GraphMaterialData red{};
    red.param_count = 1;
    red.params[0][0] = 0.90f; red.params[0][1] = 0.05f; red.params[0][2] = 0.05f;
    GraphMaterialData green{};
    green.param_count = 1;
    green.params[0][0] = 0.05f; green.params[0][1] = 0.90f; green.params[0][2] = 0.05f;

    s.beginFrame({});
    const auto mat_red = await(upload, w, uploadGraphMaterial(MaterialUploadClient(upload_client.client(), material_ops), red));
    s.beginFrame({});
    const auto mat_green = await(upload, w, uploadGraphMaterial(MaterialUploadClient(upload_client.client(), material_ops), green));
    std::cout << "[4] graph materials: red(status=" << mat_red.status
              << ") green(status=" << mat_green.status << ")\n";

    const rdesc::Mesh sphere = buildSphereMesh(0.45f, 48, 24);
    s.beginFrame({});
    const auto mesh = await(upload, w, lux::render::uploadMesh(lux::render::MeshStackUploadClient(upload_client.client(), mesh_stack_ops), sphere));

    // ---- 6. Two instances: red on the left, green on the right ----
    auto makeXform = [](float tx) {
        // column-major mat4; translation in elements [12],[13],[14]
        return std::array<float, 16>{ 1,0,0,0, 0,1,0,0, 0,0,1,0, tx,0,0,1 };
    };
    const auto xf_left  = makeXform(-0.7f);
    const auto xf_right = makeXform(+0.7f);

    s.beginFrame({});
    const auto inst_l = await(s, w, addTransientMeshInstance(MeshStackProxy(s, mesh_stack_ops), scene.scene_id, mesh.handle, mat_red.handle,   xf_left.data()));
    s.beginFrame({});
    const auto inst_r = await(s, w, addTransientMeshInstance(MeshStackProxy(s, mesh_stack_ops), scene.scene_id, mesh.handle, mat_green.handle, xf_right.data()));
    s.beginFrame({});
    MeshStackProxy(s, mesh_stack_ops).makeInstanceVisibleForView({.scene_id = scene.scene_id, .view = view.view, .object = inst_l.object});
    MeshStackProxy(s, mesh_stack_ops).makeInstanceVisibleForView({.scene_id = scene.scene_id, .view = view.view, .object = inst_r.object});

    DirectionalLightDesc dl{};
    dl.direction = Eigen::Vector3f(-0.4f, -0.8f, -0.45f).normalized();
    dl.color     = Eigen::Vector3f(1.f, 0.97f, 0.92f);
    dl.intensity = 3.0f;
    dl.flags     = LIGHT_FLAG_CAST_SHADOW;
    await(s, w, lightCreate(LightProxy(s, light_ops), scene.scene_id, LightDescriptor{dl}));

    // ---- 7. Camera framing both spheres + warm-up ----
    const Eigen::Vector3f center(0.f, 0.f, 0.f);
    const float fov = 45.f * 3.14159265f / 180.f;
    const Eigen::Vector3f eye(0.f, 0.f, 3.2f);
    const Eigen::Matrix4f V = buildViewMatrix(eye, center, Eigen::Vector3f(0, 1, 0));
    const Eigen::Matrix4f P = buildProjMatrix(fov, static_cast<float>(W) / H, 0.05f, 50.f);

    for (int i = 0; i < 20; ++i)
    {
        s.beginFrame({});
        w.pollEvents();
        lux::render::viewCameraUpdateTransient(lux::render::ViewCameraProxy(s, view_cam_ops), scene.scene_id, view.view, V.data(), P.data(), eye.data());
        s.trySubmitFrame();
        s.waitAndPumpReplies();
    }
    std::cout << "[5] rendered warm-up frames\n";

    // ---- 8. Readback + per-half analysis (pixels are BGRA8) ----
    std::vector<std::uint8_t> px(static_cast<std::size_t>(W) * H * 4, 0);
    s.beginFrame({});
    const auto rb = await(control, w, control.readbackTarget(target.target, px.data(), px.size()));
    std::cout << "[6] readback status=" << rb.status << " " << rb.width << "x" << rb.height << "\n";

    std::size_t left_red = 0, left_green = 0, right_red = 0, right_green = 0, bright = 0;
    for (uint32_t y = 0; y < H; ++y)
    {
        for (uint32_t x = 0; x < W; ++x)
        {
            const std::size_t i = (static_cast<std::size_t>(y) * W + x) * 4;
            const int b = px[i], g = px[i + 1], r = px[i + 2];
            if (std::max({b, g, r}) > 40) ++bright;
            const bool is_red   = (r > 60 && r > g + 30 && r > b + 30);
            const bool is_green = (g > 60 && g > r + 30 && g > b + 30);
            if (x < W / 2) { left_red  += is_red; left_green  += is_green; }
            else           { right_red += is_red; right_green += is_green; }
        }
    }
    std::cout << "  left:  red=" << left_red  << " green=" << left_green  << "\n";
    std::cout << "  right: red=" << right_red << " green=" << right_green << "\n";

    int fails = 0;
    auto check = [&](bool cond, const char* name)
    { std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << "\n"; if (!cond) ++fails; };

    check(compiled.status == 0,  "graph param frag compiled to a server ShaderHandle");
    check(mat_red.status == 0,   "red graph material uploaded");
    check(mat_green.status == 0, "green graph material uploaded");
    check(rb.status == 0,        "readback status == 0");
    check(bright > 400,          "both objects visible");
    // The proof: left sphere reads its OWN red param, right its OWN green param,
    // through ONE shared frag. A baked-color frag could not do both.
    check(left_red  > 150,                 "left sphere is RED (instance red param)");
    check(right_green > 150,               "right sphere is GREEN (instance green param)");
    check(left_red  > left_green  * 4 + 10, "left half is red-dominant (not green)");
    check(right_green > right_red * 4 + 10, "right half is green-dominant (not red)");

    sync->requestStop();
    server_thread.join();

    std::cout << "=== graph_params_render_test " << (fails == 0 ? "PASSED" : "FAILED")
              << " (fails=" << fails << ") ===\n";
    return fails == 0 ? 0 : 1;
}
