// ============================================================================
//  deferred_multi_shading_model_test.cpp  —  Phase D proof
//
//  Proves the deferred lighting pass now DISPATCHES per-pixel on a shading-model
//  id carried through the GBuffer (octahedral normal frees RT1.z for the id),
//  instead of lighting every pixel as PBR.
//
//  THREE spheres with the SAME grey albedo but different shading models, lit by
//  ONE directional light — so the shading model is the only variable:
//    left   = PBR    -> smooth Cook-Torrance gradient (bright lit side, dark away)
//    middle = Unlit  -> FLAT grey (no lighting; retires the emissive-passthrough
//                       hack — the deferred Unlit branch emits albedo+emissive)
//    right  = Toon   -> banded cel shading (lit, but few discrete levels)
//
//  Readback + per-third luminance stats assert:
//    * all three render,
//    * Unlit is FLAT  (max-min luminance ~ 0),
//    * PBR  is a GRADIENT (large max-min),
//    * Toon is lit (large max-min) AND has FEWER distinct levels than PBR
//      (banding vs smooth) => a genuinely different shading model, not PBR.
//
//  A single-model (pre-Phase-D) deferred pass could not produce a flat sphere
//  next to a gradient sphere from identical albedo. Self-checking: 0 = PASS,
//  1 = FAIL, 0 (skip) if no Vulkan device.
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
#include <lux/engine/math/AABB.hpp>

#include "graph_test_helpers.hpp" // lux::mgtest::makeColorGraph + compileGraphPass

#include <lux/engine/window/LuxWindow.hpp>

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace lux::render;
namespace rdesc = lux::rdesc;

static std::vector<const char*>
getVulkanExtensions()
{
    const auto exts = lux::window::LuxWindow::requiredVulkanInstanceExtensions();
    return {exts.begin(), exts.end()};
}

static Eigen::Matrix4f
buildViewMatrix(const Eigen::Vector3f& eye, const Eigen::Vector3f& target, const Eigen::Vector3f& up)
{
    const Eigen::Vector3f f = (target - eye).normalized();
    const Eigen::Vector3f s = f.cross(up).normalized();
    const Eigen::Vector3f u = s.cross(f);
    Eigen::Matrix4f V = Eigen::Matrix4f::Identity();
    V(0, 0) = s.x();
    V(0, 1) = s.y();
    V(0, 2) = s.z();
    V(0, 3) = -s.dot(eye);
    V(1, 0) = u.x();
    V(1, 1) = u.y();
    V(1, 2) = u.z();
    V(1, 3) = -u.dot(eye);
    V(2, 0) = -f.x();
    V(2, 1) = -f.y();
    V(2, 2) = -f.z();
    V(2, 3) = f.dot(eye);
    return V;
}

static Eigen::Matrix4f
buildProjMatrix(float fov_rad, float aspect, float near_z, float far_z)
{
    const float t = std::tan(fov_rad * 0.5f);
    Eigen::Matrix4f P = Eigen::Matrix4f::Zero();
    P(0, 0) = 1.f / (aspect * t);
    P(1, 1) = -1.f / t;
    P(2, 2) = -far_z / (far_z - near_z);
    P(2, 3) = -(far_z * near_z) / (far_z - near_z);
    P(3, 2) = -1.f;
    return P;
}

static rdesc::Mesh
buildSphereMesh(float radius, uint32_t segments, uint32_t rings)
{
    using V3 = Eigen::Vector3f;
    using V2 = Eigen::Vector2f;
    constexpr float kPi = 3.14159265358979323846f;

    std::vector<rdesc::Vertex> verts;
    std::vector<std::uint32_t> indices;
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
            const uint32_t i2 = i0 + stride, i3 = i2 + 1;
            indices.insert(indices.end(), {i0, i1, i2, i2, i1, i3});
        }

    return rdesc::Mesh{
        std::move(verts),
        std::move(indices),
        lux::math::AABB(V3(-radius, -radius, -radius), V3(radius, radius, radius))};
}

template <class Session, class T>
static T
await(Session& endpoint, lux::window::LuxWindow& window, RenderRequest<T> request)
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

// Per-third luminance statistics over the "lit body" pixels (background is
// black; the gbuffer is 1spp so sphere edges are hard, not anti-aliased).
struct ThirdStats
{
    int count{0}, lmin{255}, lmax{0}, distinct{0};
};
static ThirdStats
classifyThird(const std::vector<std::uint8_t>& px, uint32_t W, uint32_t H, uint32_t x0, uint32_t x1)
{
    ThirdStats r;
    std::set<int> buckets; // coarse luminance buckets (banding metric)
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = x0; x < x1; ++x)
        {
            const std::size_t i = (static_cast<std::size_t>(y) * W + x) * 4;
            const int lum = (int(px[i]) + int(px[i + 1]) + int(px[i + 2])) / 3;
            if (lum <= 20)
                continue; // skip black background
            ++r.count;
            r.lmin = std::min(r.lmin, lum);
            r.lmax = std::max(r.lmax, lum);
            buckets.insert(lum / 8);
        }
    r.distinct = static_cast<int>(buckets.size());
    if (r.count == 0)
    {
        r.lmin = 0;
        r.lmax = 0;
    }
    return r;
}

template <class T>
static T
await(RenderUploadSession& endpoint, lux::window::LuxWindow& window, UploadSubmitResult<T> submitted)
{
    if (!submitted)
        return {};
    return await(endpoint, window, std::move(submitted.value()));
}
int
main()
{
    std::cout << std::unitbuf;
    std::cout << "=== deferred_multi_shading_model_test (Phase D) ===\n";

    constexpr uint32_t W = 384, H = 256;

    auto channel = RenderFrameChannel<>::create();
    auto control_channel = RenderControlChannel<>::create();
    auto upload_channel = RenderUploadChannel<>::create();
    auto sync = std::make_shared<RenderChannelSync>();
    auto exts = getVulkanExtensions();

    lux::window::LuxWindow window(static_cast<int>(W), static_cast<int>(H), "deferred_multi_shading_model_test");

    std::atomic<bool> ready{false}, failed{false};
    std::thread server_thread([&] {
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
        try
        {
            while (server.tick())
            {
            }
        }
        catch (const std::exception& e)
        {
            std::cerr << "[Server] tick threw: " << e.what() << "\n";
        }
    }
    );

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

    // LDR scene so deferred lighting writes SceneColor directly (offscreen
    // readback needs no tonemap pass).
    RenderControlSession::CreateSceneConfig scfg{};
    scfg.name = "MultiSMScene";
    scfg.lit_color_format = lux::rdesc::ETextureFormat::RGBA8_SRGB;
    s.beginFrame({});
    const auto scene = await(control, w, control.createScene(scfg));
    s.beginFrame({});
    await(control, w, control.setActiveScene(scene.scene_id, true));
    s.beginFrame({});
    const auto view = await(control, w, control.addView(scene.scene_id, {W, H}, "MultiSMView"));
    s.beginFrame({});
    const auto target = await(control, w, control.createOffscreenRenderTarget({W, H}));
    s.beginFrame({});
    control.setLayer(target.target, 0, scene.scene_id, view.view);
    s.trySubmitFrame();
    s.waitAndPumpReplies();

    // Deferred feature set (shadow + meshshadow + gbuffer + lighting). No Config
    // override — each graph material carries its OWN gbuffer frag (R1), and the
    // graph's shading_model writes the GBuffer SM-id the lighting dispatches on.
    // LightFeature MUST be registered + added before ShadowMapFeature: ShadowMap
    // caches a raw LightResources* at attach, so Light must attach first.
    // StandardViewCameraFeature owns the per-scene 3D camera data (per-view
    // view/proj matrices + frustum). Every camera consumer (shadow / gbuffer /
    // lighting) reads ViewCameraResource, so it MUST be added FIRST — owner-first,
    // like StandardMeshStack. Carries the per-view camera update op (ViewCameraProxy).
    s.beginFrame({});
    const auto view_cam_reg = await(control, w, control.registerFeatureType(lux::render::kViewCameraFeatureFactory));
    lux::render::ViewCameraOperationIds view_cam_ops =
        lux::render::ViewCameraOperationIds::fromOps(view_cam_reg.ops, view_cam_reg.op_count);
    lux::render::ViewCameraCommTag view_cam_cfg{};
    s.beginFrame({});
    await(control, w, control.addFeature(scene.scene_id, view_cam_reg.feature_type_id, view_cam_cfg));

    s.beginFrame({});
    const auto light_feat_reg = await(control, w, control.registerFeatureType(kLightFeatureFactory));
    LightOperationIds light_ops = LightOperationIds::fromOps(light_feat_reg.ops, light_feat_reg.op_count);
    lux::render::LightCommTag light_cfg{};
    s.beginFrame({});
    await(control, w, control.addFeature(scene.scene_id, light_feat_reg.feature_type_id, light_cfg));

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
    // layout, instance buffers, …) that every mesh consumer reads, and supplies
    // the dynamic mesh-instance ops sent via MeshStackProxy. Must attach before
    // any mesh feature or nothing renders.
    s.beginFrame({});
    const auto mesh_stack_reg = await(control, w, control.registerFeatureType(lux::render::kMeshStackFeatureFactory));
    MeshStackOperationIds mesh_stack_ops = MeshStackOperationIds::fromOps(mesh_stack_reg.ops, mesh_stack_reg.op_count);
    lux::render::MeshStackCommTag mesh_stack_cfg{};
    s.beginFrame({});
    await(control, w, control.addFeature(scene.scene_id, mesh_stack_reg.feature_type_id, mesh_stack_cfg));

    s.beginFrame({});
    const auto shadow_reg = await(control, w, control.registerFeatureType(kShadowMapFeatureFactory));
    s.beginFrame({});
    const auto mshsw_reg = await(control, w, control.registerFeatureType(kMeshShadowFeatureFactory));
    s.beginFrame({});
    const auto gbuf_reg = await(control, w, control.registerFeatureType(kDeferredGBufferFeatureFactory));
    s.beginFrame({});
    const auto light_reg = await(control, w, control.registerFeatureType(kDeferredLightingFeatureFactory));

    ShadowMapCommConfig scc{};
    scc.atlas_page_resolution = 2048;
    scc.atlas_page_count = 2;
    scc.max_shadow_slices = 64;
    scc.enable_directional_csm = 1;
    scc.default_technique = EShadowTechnique::PCF;
    s.beginFrame({});
    await(control, w, control.addFeature(scene.scene_id, shadow_reg.feature_type_id, scc));

    MeshShadowCommConfig mscc{};
    mscc.comm_config_version = kMeshShadowCommConfigVersion;
    mscc.descriptor_layout_version = kMeshShadowDescriptorLayoutVersion;
    s.beginFrame({});
    await(control, w, control.addFeature(scene.scene_id, mshsw_reg.feature_type_id, mscc));

    DeferredGBufferCommConfig gbcc{};
    gbcc.comm_config_version = kDeferredGBufferCommConfigVersion;
    gbcc.descriptor_layout_version = kDeferredGBufferDescriptorLayoutVersion;
    s.beginFrame({});
    await(control, w, control.addFeature(scene.scene_id, gbuf_reg.feature_type_id, gbcc));

    DeferredLightingCommConfig dlcc{};
    dlcc.read_mode = lux::render::ELightingReadMode::SAMPLED;
    dlcc.enable_clustered = 1;
    dlcc.cluster_x = 16;
    dlcc.cluster_y = 9;
    dlcc.cluster_z = 24;
    dlcc.max_cluster_indices = 1'048'576;
    dlcc.technique = EShadowTechnique::PCF;
    s.beginFrame({});
    await(control, w, control.addFeature(scene.scene_id, light_reg.feature_type_id, dlcc));
    std::cout << "[2] deferred features added\n";

    // Three GRAPH materials, IDENTICAL grey albedo, different shading models. The
    // graph's shading_model writes the GBuffer SM-id (Unlit/PBR/Toon) the deferred
    // lighting dispatches per-pixel on; each carries its own gbuffer frag (R1). The
    // deferred toon banding uses FIXED GBuffer-only step counts (brdf_toon.glsl), so
    // no per-material toon params are needed (those are forward-only).
    const Eigen::Vector3f grey(0.6f, 0.6f, 0.6f);

    auto compileSM =
        [&](rdesc::EMaterialShadingModel sm, std::vector<uint32_t>& spv, std::vector<std::byte>& info) -> bool {
        auto cb = lux::mgtest::compileGraphPass(
            lux::mgtest::makeColorGraph(
                grey.x(),
                grey.y(),
                grey.z(),
                sm,
                /*metallic=*/0.0f,
                /*roughness=*/0.4f
            ),
            rdesc::EMaterialPass::GBuffer
        );
        if (cb)
        {
            spv = std::move(cb->spirv);
            info = std::move(cb->info_bytes);
            return true;
        }
        std::cerr << "graph compile failed: " << cb.error() << "\n";
        return false;
    };
    std::vector<uint32_t> spv_pbr, spv_unlit, spv_toon;
    std::vector<std::byte> info_pbr, info_unlit, info_toon;
    if (!compileSM(rdesc::EMaterialShadingModel::PbrMetallicRoughness, spv_pbr, info_pbr) ||
        !compileSM(rdesc::EMaterialShadingModel::Unlit, spv_unlit, info_unlit) ||
        !compileSM(rdesc::EMaterialShadingModel::Stylized, spv_toon, info_toon))
    {
        sync->requestStop();
        server_thread.join();
        return 1;
    }

    // compileShader each baked frag, then upload a graph material carrying it.
    auto uploadGraph = [&](std::vector<uint32_t>& spv, std::vector<std::byte>& info) {
        const auto bytes =
            std::span<const std::byte>{reinterpret_cast<const std::byte*>(spv.data()), spv.size() * sizeof(uint32_t)};
        s.beginFrame({});
        const auto comp =
            await(control, w, control.compileShader(bytes, std::span<const std::byte>{info.data(), info.size()}));
        GraphMaterialData gd{};
        s.beginFrame({});
        return await(
            upload,
            w,
            uploadGraphMaterial(
                MaterialUploadClient(upload_client.client(), material_ops),
                gd,
                comp.shader,
                ShaderHandle{})
        );
    };
    const auto mat_pbr = uploadGraph(spv_pbr, info_pbr);
    const auto mat_unlit = uploadGraph(spv_unlit, info_unlit);
    const auto mat_toon = uploadGraph(spv_toon, info_toon);
    std::cout << "[3] graph materials uploaded: pbr(status=" << mat_pbr.status << ") unlit(status=" << mat_unlit.status
              << ") toon(status=" << mat_toon.status << ")\n";

    const rdesc::Mesh sphere = buildSphereMesh(0.5f, 48, 24);
    s.beginFrame({});
    const auto mesh = await(
        upload,
        w,
        lux::render::uploadMesh(lux::render::MeshStackUploadClient(upload_client.client(), mesh_stack_ops), sphere)
    );

    // Three instances spread so each sphere stays cleanly within its screen
    // third: left=PBR (x=-1.8), middle=Unlit (x=0), right=Toon (x=+1.8).
    auto xform = [](float x) { return std::array<float, 16>{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, 0, 0, 1}; };
    auto x_l = xform(-1.8f), x_m = xform(0.0f), x_r = xform(1.8f);
    s.beginFrame({});
    const auto inst_l = await(
        s,
        w,
        addTransientMeshInstance(
            MeshStackProxy(s, mesh_stack_ops),
            scene.scene_id,
            mesh.handle,
            mat_pbr.handle,
            x_l.data())
    );
    s.beginFrame({});
    const auto inst_m = await(
        s,
        w,
        addTransientMeshInstance(
            MeshStackProxy(s, mesh_stack_ops),
            scene.scene_id,
            mesh.handle,
            mat_unlit.handle,
            x_m.data())
    );
    s.beginFrame({});
    const auto inst_r = await(
        s,
        w,
        addTransientMeshInstance(
            MeshStackProxy(s, mesh_stack_ops),
            scene.scene_id,
            mesh.handle,
            mat_toon.handle,
            x_r.data())
    );
    s.beginFrame({});
    MeshStackProxy(s, mesh_stack_ops)
        .makeInstanceVisibleForView({.scene_id = scene.scene_id, .view = view.view, .object = inst_l.object});
    MeshStackProxy(s, mesh_stack_ops)
        .makeInstanceVisibleForView({.scene_id = scene.scene_id, .view = view.view, .object = inst_m.object});
    MeshStackProxy(s, mesh_stack_ops)
        .makeInstanceVisibleForView({.scene_id = scene.scene_id, .view = view.view, .object = inst_r.object});

    // One directional light from the front-left. NO cast-shadow flag (keeps the
    // spheres free of inter-occlusion so the test isolates the shading model).
    DirectionalLightDesc dl{};
    dl.direction = Eigen::Vector3f(-0.6f, -0.35f, -0.7f).normalized();
    dl.color = Eigen::Vector3f(1.f, 1.f, 1.f);
    dl.intensity = 3.0f;
    dl.flags = 0;
    await(s, w, lightCreate(LightProxy(s, light_ops), scene.scene_id, LightDescriptor{dl}));

    const Eigen::Vector3f eye(0.f, 0.f, 4.2f), center(0.f, 0.f, 0.f);
    const float fov = 50.f * 3.14159265f / 180.f;
    const Eigen::Matrix4f V = buildViewMatrix(eye, center, Eigen::Vector3f(0, 1, 0));
    const Eigen::Matrix4f P = buildProjMatrix(fov, static_cast<float>(W) / H, 0.05f, 50.f);

    for (int i = 0; i < 20; ++i)
    {
        s.beginFrame({});
        w.pollEvents();
        lux::render::viewCameraUpdateTransient(
            lux::render::ViewCameraProxy(s, view_cam_ops),
            scene.scene_id,
            view.view,
            V.data(),
            P.data(),
            eye.data()
        );
        s.trySubmitFrame();
        s.waitAndPumpReplies();
    }
    std::cout << "[4] rendered warm-up frames\n";

    std::vector<std::uint8_t> px(static_cast<std::size_t>(W) * H * 4, 0);
    s.beginFrame({});
    const auto rb = await(control, w, control.readbackTarget(target.target, px.data(), px.size()));
    std::cout << "[5] readback status=" << rb.status << " " << rb.width << "x" << rb.height << "\n";

    const ThirdStats L = classifyThird(px, W, H, 0, W / 3);         // PBR
    const ThirdStats M = classifyThird(px, W, H, W / 3, 2 * W / 3); // Unlit
    const ThirdStats R = classifyThird(px, W, H, 2 * W / 3, W);     // Toon
    auto dump = [](const char* n, const ThirdStats& t) {
        std::cout << "  " << n << ": count=" << t.count << " lum[" << t.lmin << ".." << t.lmax
                  << "] range=" << (t.lmax - t.lmin) << " distinct=" << t.distinct << "\n";
    };
    dump("LEFT  (PBR)  ", L);
    dump("MIDDLE(Unlit)", M);
    dump("RIGHT (Toon) ", R);

    int fails = 0;
    auto check = [&](bool cond, const char* name) {
        std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << "\n";
        if (!cond)
            ++fails;
    };

    check(rb.status == 0, "readback status == 0");
    check(mat_pbr.status == 0 && mat_unlit.status == 0 && mat_toon.status == 0, "all three materials uploaded");
    check(L.count > 300 && M.count > 300 && R.count > 300, "all three spheres rendered");

    // THE PROOF: identical albedo, different shading models -> different output.
    check((M.lmax - M.lmin) < 24, "Unlit sphere is FLAT (no lighting) => SM-id dispatched to the unlit branch");
    check((L.lmax - L.lmin) > 55, "PBR sphere has a lit GRADIENT => SM-id dispatched to GGX");
    check((R.lmax - R.lmin) > 45, "Toon sphere is LIT (not flat) => not the unlit branch");
    check(
        R.distinct < L.distinct,
        "Toon sphere has FEWER distinct levels than PBR (banding vs smooth) => toon dispatch"
    );

    sync->requestStop();
    server_thread.join();

    std::cout << "=== deferred_multi_shading_model_test " << (fails == 0 ? "PASSED" : "FAILED") << " (fails=" << fails
              << ") ===\n";
    return fails == 0 ? 0 : 1;
}
