// ============================================================================
//  gbuffer_deferred_lit_test.cpp  —  M3d-3 lit + Input-driven proof
//
//  Proves two things the M3d-2 constant test could not:
//    1. CORRECT DEFERRED LIGHTING for a graph PBR material: the compiler packs
//       the gbuffer in the engine's PBR convention (metallic/roughness), so the
//       engine's PBR-only deferred lighting GGX-lights it correctly.
//    2. INPUT-DRIVEN graphs work through the real pipeline: the graph reads the
//       interpolated WorldNormal (Input node -> location 1, the engine
//       gbuffer_vp.vert vWorldNormal output) and feeds it as the surface normal,
//       so the lit result shows a LIGHTING GRADIENT across the sphere.
//
//  Definitive design (W5a: per-material PSO, no builtin family / Config override):
//    - The graph material carries its OWN gbuffer frag via uploadGraphMaterial(data,
//      gbuffer, forward) (R1). Its constant base_color is GREY, so a GREY lit result
//      can ONLY come from the graph frag.
//    - A flat/constant normal would shade the sphere ~uniformly; the interpolated
//      WorldNormal produces a continuous bright(lit)->mid(terminator) gradient.
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

#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/description/Vertex.hpp>
#include <lux/engine/description/ShaderInfo.hpp>
#include <lux/engine/math/AABB.hpp>
#include <lux/engine/render/resources/material/GraphMaterialData.hpp>

#include "graph_test_helpers.hpp"   // mgtest::makeColorGraph + compileGraphPass

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
    std::cout << "=== gbuffer_deferred_lit_test (M3d-3) ===\n";

    // GREY PBR graph + Input(WorldNormal) -> surface normal (lit gradient).
    auto frag_cb = lux::mgtest::compileGraphPass(
            lux::mgtest::makeColorGraph(0.72f, 0.72f, 0.74f,
                                        rdesc::EMaterialShadingModel::PbrMetallicRoughness,
                                        /*metallic=*/0.0f, /*roughness=*/0.35f,
                                        /*world_normal_input=*/true),
            rdesc::EMaterialPass::GBuffer);
    if (!frag_cb)
    {
        std::cerr << "graph -> SPIR-V failed: " << frag_cb.error() << "\n";
        return 1;
    }
    std::vector<uint32_t>  frag_spirv = std::move(frag_cb->spirv);
    std::vector<std::byte> frag_info  = std::move(frag_cb->info_bytes);
    std::cout << "[0] lit graph frag generated (" << frag_spirv.size() << " words)\n";

    constexpr uint32_t W = 256, H = 256;

    auto channel = RenderProgramChannel<>::create();
    auto sync    = std::make_shared<RenderChannelSync>();
    auto exts    = getVulkanExtensions();

    lux::window::LuxWindow window(static_cast<int>(W), static_cast<int>(H), "gbuffer_deferred_lit_test");

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

    RenderSession::CreateSceneConfig scfg{};
    scfg.name             = "GraphLitScene";
    scfg.lit_color_format = lux::common::ETextureFormat::RGBA8_SRGB;  // LDR: lighting writes readback target
    s.beginFrame({});
    const auto scene = await(s, w, s.createScene(scfg));
    s.beginFrame({});
    await(s, w, s.setActiveScene(scene.scene_id, true));
    s.beginFrame({});
    const auto view = await(s, w, s.addView(scene.scene_id, {W, H}, "GraphLitView"));

    const auto frag_bytes = std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(frag_spirv.data()),
        frag_spirv.size() * sizeof(uint32_t)};
    s.beginFrame({});
    const auto compiled = await(s, w, s.compileShader(
        frag_bytes, std::span<const std::byte>{frag_info.data(), frag_info.size()}));
    std::cout << "[2] compileShader(lit graph frag) status=" << compiled.status << "\n";

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

    // NO Config family override: R1 routes each material's own frag via its variant
    // bucket (uploadGraphMaterial below), not a single gbuffer_*_fragment_shader slot.
    DeferredGBufferCommConfig gbcc{};
    gbcc.comm_config_version       = kDeferredGBufferCommConfigVersion;
    gbcc.descriptor_layout_version = kDeferredGBufferDescriptorLayoutVersion;
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
    std::cout << "[3] deferred features added (gbuffer PBR frag = graph)\n";

    // Graph material carries its OWN gbuffer frag (R1 per-material PSO). The grey
    // constant base_color comes from the graph itself — no builtin material involved.
    GraphMaterialData gmat{};
    s.beginFrame({});
    const auto mat = await(s, w, MaterialProxy(s, material_ops).uploadGraphMaterial(gmat, compiled.shader, ShaderHandle{}));

    const rdesc::Mesh sphere = buildSphereMesh(0.5f, 64, 32);
    s.beginFrame({});
    const auto mesh = await(s, w, lux::render::MeshStackProxy(s, mesh_stack_ops).uploadMesh(sphere));
    std::cout << "[4] graph material(status=" << mat.status << ") + mesh(status=" << mesh.status << ")\n";

    float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    s.beginFrame({});
    const auto inst = await(s, w, MeshStackProxy(s, mesh_stack_ops).addMeshInstance(scene.scene_id, mesh.handle, mat.handle, identity));
    s.beginFrame({});
    MeshStackProxy(s, mesh_stack_ops).makeInstanceVisibleForView(scene.scene_id, view.view, inst.object);

    // Neutral white directional light so a grey surface stays grey (no colour cast).
    DirectionalLightDesc dl{};
    dl.direction = Eigen::Vector3f(-0.5f, -0.55f, -0.65f).normalized();
    dl.color     = Eigen::Vector3f(1.f, 1.f, 1.f);
    dl.intensity = 3.0f;
    dl.flags     = LIGHT_FLAG_CAST_SHADOW;
    await(s, w, LightProxy(s, light_ops).createLight(scene.scene_id, LightDescriptor{dl}));

    const Eigen::Vector3f center(0.f, 0.f, 0.f);
    const float radius = 0.5f * std::sqrt(3.f);
    const float fov    = 45.f * 3.14159265f / 180.f;
    const float dist   = (radius / std::sin(fov * 0.5f)) * 1.25f;
    const Eigen::Vector3f eye = center + Eigen::Vector3f(0.4f, 0.35f, 1.0f).normalized() * dist;
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

    // Pixels are BGRA8. luma = 0.114 B + 0.587 G + 0.299 R.
    std::size_t bright = 0, mid = 0, grey_bright = 0;
    int luma_min = 255, luma_max = 0;
    for (std::size_t i = 0; i + 4 <= px.size(); i += 4)
    {
        const int b = px[i], g = px[i+1], r = px[i+2];
        const int luma = (114 * b + 587 * g + 299 * r) / 1000;
        if (luma > 25) { luma_min = std::min(luma_min, luma); luma_max = std::max(luma_max, luma); }
        const int chroma = std::max({std::abs(r - g), std::abs(r - b), std::abs(g - b)});
        if (luma > 140) { ++bright; if (chroma < 45) ++grey_bright; }
        else if (luma > 40) ++mid;
    }
    const std::size_t cx = (static_cast<std::size_t>(H/2) * W + W/2) * 4;
    std::cout << "  centre pixel (BGRA8) = (" << (int)px[cx] << ", " << (int)px[cx+1]
              << ", " << (int)px[cx+2] << ", " << (int)px[cx+3] << ")\n";
    std::cout << "  bright(luma>140)=" << bright << "  mid(40<luma<=140)=" << mid
              << "  grey_bright=" << grey_bright << "  luma=[" << luma_min << "," << luma_max << "]\n";

    int fails = 0;
    auto check = [&](bool cond, const char* name)
    { std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << "\n"; if (!cond) ++fails; };

    check(compiled.status == 0,                  "graph frag compiled to a server ShaderHandle");
    check(rb.status == 0,                         "readback status == 0");
    check(rb.width == W && rb.height == H,        "dimensions match the view");
    check(bright > 200,                           "sphere is lit (bright pixels present)");
    check(mid > 200,
          "lighting GRADIENT present (bright lit side + mid terminator) => interpolated WorldNormal drove GGX");
    check((luma_max - luma_min) > 70,            "wide luma range (continuous shading gradient)");
    check(grey_bright * 2 >= bright,
          "lit surface is GREY => the graph's grey constant base_color drove the gbuffer (per-material PSO)");

    sync->requestStop();
    server_thread.join();

    std::cout << "=== gbuffer_deferred_lit_test " << (fails == 0 ? "PASSED" : "FAILED")
              << " (fails=" << fails << ") ===\n";
    return fails == 0 ? 0 : 1;
}
