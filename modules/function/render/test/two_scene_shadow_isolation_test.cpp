// ============================================================================
//  two_scene_shadow_isolation_test.cpp
//
//  Acceptance gate for Plan A (per-scene Light/Shadow resource isolation).
//
//  Builds TWO coexisting offscreen RenderScenes, each with its own ground +
//  occluder + a cast-shadow directional light pointing in OPPOSITE directions.
//  Both scenes render every server tick. We read back scene A:
//      (1) while A is the ONLY scene            -> bufferA0
//      (2) after scene B (opposite light) exists -> bufferA1
//  and assert bufferA0 == bufferA1.
//
//  Rationale: before Plan A, LightResources and ShadowResources were process-
//  global singletons, so scene A was lit by B's light (and its shadow atlas /
//  caster light hijacked) the moment B appeared -> bufferA0 != bufferA1. With
//  per-scene resources, B cannot perturb A -> the two readbacks are identical.
//
//  => This test FAILS on a pre-Plan-A checkout and PASSES post-Plan-A.
//
//  Self-checking: 0 = PASS (or skip if no Vulkan), 1 = FAIL.
// ============================================================================

#include <lux/engine/render/comm/client/RenderSession.hpp>
#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/render/comm/RenderProtocol.hpp>

#include <lux/engine/render/renderer/features/forward/ForwardMeshOperation.hpp>
#include <lux/engine/render/renderer/features/shadow/ShadowMapOperation.hpp>
#include <lux/engine/render/renderer/features/shadow/MeshShadowOperation.hpp>
#include <lux/engine/render/renderer/features/light/LightOperation.hpp>  // LightFeature / LightProxy
#include <lux/engine/render/renderer/features/meshstack/MeshStackOperation.hpp>
#include <lux/engine/render/renderer/features/material/MaterialOperation.hpp>
#include <lux/engine/render/renderer/features/view_camera/ViewCameraOperation.hpp>  // kStandardViewCameraFeatureFactory / ViewCameraProxy
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
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace lux::render;

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

// ── Camera helpers (Vulkan Y-down, ZO depth) — same math as thumbnail test ──

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

// ── Geometry ────────────────────────────────────────────────────────────────

static lux::rdesc::Mesh buildSphereMesh(float radius, uint32_t segments, uint32_t rings)
{
    using V3 = Eigen::Vector3f;
    using V2 = Eigen::Vector2f;
    constexpr float kPi = 3.14159265358979323846f;

    std::vector<lux::rdesc::Vertex> verts;
    std::vector<std::uint32_t>      indices;
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

    return lux::rdesc::Mesh{
        std::move(verts), std::move(indices),
        lux::math::AABB(V3(-radius, -radius, -radius), V3(radius, radius, radius))
    };
}

// Flat ground quad in the y=0 plane. Emitted DOUBLE-SIDED (both windings) so it
// is visible as a shadow receiver regardless of the forward pass cull mode.
static lux::rdesc::Mesh buildGroundMesh(float half)
{
    using V3 = Eigen::Vector3f;
    using V2 = Eigen::Vector2f;
    const V3 n(0, 1, 0), t(1, 0, 0), bt = n.cross(t).normalized();
    std::vector<lux::rdesc::Vertex> verts = {
        {V3(-half, 0, -half), n, t, V2(0, 0), bt},
        {V3( half, 0, -half), n, t, V2(1, 0), bt},
        {V3( half, 0,  half), n, t, V2(1, 1), bt},
        {V3(-half, 0,  half), n, t, V2(0, 1), bt},
    };
    std::vector<std::uint32_t> indices = {
        0, 1, 2, 0, 2, 3,   // one winding
        0, 2, 1, 0, 3, 2,   // and the reverse → double-sided
    };
    return lux::rdesc::Mesh{
        std::move(verts), std::move(indices),
        lux::math::AABB(V3(-half, -0.01f, -half), V3(half, 0.01f, half))
    };
}

static void makeTranslate(float out[16], float x, float y, float z)
{
    float m[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, x,y,z,1}; // column-major
    std::memcpy(out, m, sizeof(m));
}

// Drive frames (blocking submit) until the (possibly deferred) reply resolves.
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

struct SceneBundle
{
    RenderSceneId scene_id{};
    ViewHandle    view{};
    ViewCameraOperationIds view_cam_ops{};  // this scene's per-view camera update op (ViewCameraProxy)
};

// Build a self-contained scene: offscreen view + shadow/mesh-shadow/forward
// features + ground (receiver) + sphere (occluder) + one cast-shadow
// directional light pointing in `light_dir`.
static SceneBundle buildScene(RenderSession& s, lux::window::LuxWindow& w,
                              const char* name, uint32_t W, uint32_t H,
                              const Eigen::Vector3f& light_dir)
{
    s.beginFrame({});
    const auto scene = await(s, w, s.createScene(name));
    s.beginFrame({});
    await(s, w, s.setActiveScene(scene.scene_id, true));
    s.beginFrame({});
    const auto view = await(s, w, s.addView(scene.scene_id, {W, H}, name));

    // StandardViewCameraFeature owns this scene's per-view 3D camera data (view/
    // proj matrices + frustum) that shadow/forward consume. MUST register +
    // addFeature for this scene BEFORE every camera consumer — owner-first.
    // (Per-scene: this helper runs once per scene, so each scene gets its own.)
    s.beginFrame({});
    const auto view_cam_reg = await(s, w, s.registerFeatureType(lux::render::kStandardViewCameraFeatureFactory));
    ViewCameraOperationIds view_cam_ops = ViewCameraOperationIds::fromOps(view_cam_reg.ops, view_cam_reg.op_count);
    struct EmptyViewCamCfg {} view_cam_cfg{};
    s.beginFrame({});
    await(s, w, s.addFeature(scene.scene_id, view_cam_reg.feature_type_id, view_cam_cfg));

    // LightFeature now owns scene light DATA; shadow/forward CONSUME it via
    // find<LightResources>. ShadowMapFeature caches a raw LightResources* at
    // attach, so Light MUST register + addFeature for this scene BEFORE shadow.
    s.beginFrame({});
    const auto light_reg = await(s, w, s.registerFeatureType(kLightFeatureFactory));
    LightOperationIds light_ops = LightOperationIds::fromOps(light_reg.ops, light_reg.op_count);
    struct EmptyLightCfg {} light_cfg{};
    s.beginFrame({});
    await(s, w, s.addFeature(scene.scene_id, light_reg.feature_type_id, light_cfg));

    // StandardMaterialFeature owns the standard material-stack resources (shading
    // model registry, material resources) that the mesh stack reads. Must attach
    // before StandardMeshStack. (Per-scene: this helper runs once per scene, so
    // each scene gets its own.)
    s.beginFrame({});
    const auto material_reg = await(s, w, s.registerFeatureType(lux::render::kStandardMaterialFeatureFactory));
    MaterialOperationIds material_ops = MaterialOperationIds::fromOps(material_reg.ops, material_reg.op_count);
    struct EmptyMaterialCfg {} material_cfg{};
    s.beginFrame({});
    await(s, w, s.addFeature(scene.scene_id, material_reg.feature_type_id, material_cfg));

    // StandardMeshStackFeature owns the standard 3D mesh-stack resources (cull
    // layout, instance buffers, …) that every mesh consumer reads. Must attach
    // before any mesh feature or nothing renders. (Per-scene: this helper runs
    // once per scene, so each scene gets its own.)
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
    const auto fwd_reg    = await(s, w, s.registerFeatureType(kForwardMeshFeatureFactory));

    ShadowMapCommConfig scc{};
    scc.atlas_page_resolution = 2048;
    scc.atlas_page_count      = 2;
    scc.max_shadow_slices     = 64;
    s.beginFrame({});
    await(s, w, s.addFeature(scene.scene_id, shadow_reg.feature_type_id, scc));

    MeshShadowCommConfig mscc{};
    mscc.comm_config_version       = kMeshShadowCommConfigVersion;
    mscc.descriptor_layout_version = kMeshShadowDescriptorLayoutVersion;
    s.beginFrame({});
    await(s, w, s.addFeature(scene.scene_id, mshsw_reg.feature_type_id, mscc));

    ForwardMeshCommConfig fcc{};
    fcc.comm_config_version       = kForwardMeshCommConfigVersion;
    fcc.descriptor_layout_version = kForwardMeshDescriptorLayoutVersion;
    s.beginFrame({});
    await(s, w, s.addFeature(scene.scene_id, fwd_reg.feature_type_id, fcc));

    // Single neutral grey PBR graph reused by ground + sphere. PBR is required:
    // Unlit/LegacyLit do not sample the shadow atlas. Compiled for the Forward
    // pass and uploaded once (R1 per-material PSO; no Config family override).
    auto mat_cb = lux::mgtest::compileGraphPass(
            lux::mgtest::makeColorGraph(0.6f, 0.6f, 0.6f,
                                        lux::rdesc::EMaterialShadingModel::PbrMetallicRoughness,
                                        /*metallic=*/0.0f, /*roughness=*/0.6f),
            lux::rdesc::EMaterialPass::Forward);
    if (!mat_cb)
    {
        std::cerr << "graph -> SPIR-V failed: " << mat_cb.error() << "\n";
        return SceneBundle{};
    }
    std::vector<std::uint32_t> mat_spirv = std::move(mat_cb->spirv);
    std::vector<std::byte>     mat_info  = std::move(mat_cb->info_bytes);
    const auto mat_bytes = std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(mat_spirv.data()),
        mat_spirv.size() * sizeof(std::uint32_t)};
    s.beginFrame({});
    const auto mat_frag = await(s, w, s.compileShader(
        mat_bytes, std::span<const std::byte>{mat_info.data(), mat_info.size()}));

    lux::render::GraphMaterialData mat_data{};
    s.beginFrame({});
    const auto mat = await(s, w, MaterialProxy(s, material_ops).uploadGraphMaterial(mat_data, ShaderHandle{}, mat_frag.shader));

    const lux::rdesc::Mesh ground = buildGroundMesh(4.f);
    s.beginFrame({});
    const auto ground_mesh = await(s, w, lux::render::MeshStackProxy(s, mesh_stack_ops).uploadMesh(ground));
    const lux::rdesc::Mesh sphere = buildSphereMesh(0.6f, 48, 24);
    s.beginFrame({});
    const auto sphere_mesh = await(s, w, lux::render::MeshStackProxy(s, mesh_stack_ops).uploadMesh(sphere));

    float ground_xform[16]; makeTranslate(ground_xform, 0.f, 0.f, 0.f);
    float sphere_xform[16]; makeTranslate(sphere_xform, 0.f, 0.9f, 0.f);
    s.beginFrame({});
    const auto ground_inst = await(s, w, MeshStackProxy(s, mesh_stack_ops).addMeshInstance(scene.scene_id, ground_mesh.handle, mat.handle, ground_xform));
    s.beginFrame({});
    const auto sphere_inst = await(s, w, MeshStackProxy(s, mesh_stack_ops).addMeshInstance(scene.scene_id, sphere_mesh.handle, mat.handle, sphere_xform));

    s.beginFrame({});
    MeshStackProxy(s, mesh_stack_ops).makeInstanceVisibleForView(scene.scene_id, view.view, ground_inst.object);
    MeshStackProxy(s, mesh_stack_ops).makeInstanceVisibleForView(scene.scene_id, view.view, sphere_inst.object);

    DirectionalLightDesc dl{};
    dl.direction = light_dir.normalized();
    dl.color     = Eigen::Vector3f(1.f, 0.97f, 0.92f);
    dl.intensity = 3.0f;
    dl.flags     = LIGHT_FLAG_CAST_SHADOW;
    s.beginFrame({});
    await(s, w, LightProxy(s, light_ops).createLight(scene.scene_id, LightDescriptor{dl}));

    return SceneBundle{scene.scene_id, view.view, view_cam_ops};
}

int main()
{
    std::cout << std::unitbuf;
    std::cout << "=== Two-Scene Light/Shadow Isolation Test ===\n";

    constexpr uint32_t W = 256, H = 256;

    auto channel = RenderProgramChannel<>::create();
    auto sync    = std::make_shared<RenderChannelSync>();
    auto exts    = getVulkanExtensions();

    lux::window::LuxWindow window(static_cast<int>(W), static_cast<int>(H),
                                  "two_scene_shadow_isolation_test");
    std::cout << "[1] window created\n";

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
    std::cout << "[2] server ready\n";

    RenderSession session(channel, sync);
    auto& s = session;
    auto& w = window;

    // Shared framing camera: above + in front, looking at the scene so the
    // ground, the sphere and the cast shadow are all visible.
    const Eigen::Vector3f eye(2.4f, 3.0f, 3.6f);
    const Eigen::Vector3f target(0.f, 0.6f, 0.f);
    const Eigen::Matrix4f V = buildViewMatrix(eye, target, Eigen::Vector3f(0, 1, 0));
    const Eigen::Matrix4f P = buildProjMatrix(45.f * 3.14159265f / 180.f,
                                              static_cast<float>(W) / H, 0.05f, 50.f);

    auto warmUp = [&](const std::vector<SceneBundle>& scenes, int frames)
    {
        for (int i = 0; i < frames; ++i)
        {
            s.beginFrame({});
            w.pollEvents();
            for (const auto& sc : scenes)
                ViewCameraProxy(s, sc.view_cam_ops).update(sc.scene_id, sc.view, V.data(), P.data(), eye.data());
            s.submitFrame(/*blocking=*/true);
            s.waitAndPumpReplies();
        }
    };

    auto readback = [&](const SceneBundle& sc, std::vector<std::uint8_t>& out)
    {
        out.assign(static_cast<std::size_t>(W) * H * 4, 0);
        s.beginFrame({});
        const auto rb = await(s, w, s.readbackView(sc.scene_id, sc.view, out.data(), out.size()));
        return rb;
    };

    // ── Scene A alone (light pointing one way) ──────────────────────────
    const SceneBundle A = buildScene(s, w, "SceneA", W, H,
                                     Eigen::Vector3f(-1.0f, -1.2f, -0.35f));
    std::cout << "[3] scene A built (id=" << A.scene_id.index << ")\n";
    warmUp({A}, 12);
    std::vector<std::uint8_t> bufA0;
    const auto rb0 = readback(A, bufA0);
    std::cout << "[4] readback A-alone: status=" << rb0.status
              << " bytes=" << rb0.bytes_written << "\n";

    // ── Scene B with the OPPOSITE light direction ───────────────────────
    const SceneBundle B = buildScene(s, w, "SceneB", W, H,
                                     Eigen::Vector3f(+1.0f, -1.2f, +0.35f));
    std::cout << "[5] scene B built (id=" << B.scene_id.index << ")\n";
    warmUp({A, B}, 12);            // both scenes render each tick now
    std::vector<std::uint8_t> bufA1;
    const auto rb1 = readback(A, bufA1);
    std::cout << "[6] readback A-with-B: status=" << rb1.status
              << " bytes=" << rb1.bytes_written << "\n";

    // Also read back scene B: its opposite light must produce a DIFFERENT image
    // than A — otherwise the A0==A1 equality could be a vacuous "lights are
    // ignored" pass rather than genuine isolation.
    std::vector<std::uint8_t> bufB;
    const auto rbB = readback(B, bufB);
    std::cout << "[7] readback B-with-A: status=" << rbB.status
              << " bytes=" << rbB.bytes_written << "\n";

    // Per-pair pixel diff helper.
    auto pixelDiff = [](const std::vector<std::uint8_t>& x,
                        const std::vector<std::uint8_t>& y,
                        std::size_t& diff_pixels, int& max_abs)
    {
        diff_pixels = 0; max_abs = 0;
        if (x.size() != y.size()) { diff_pixels = x.size(); max_abs = 255; return; }
        for (std::size_t i = 0; i + 4 <= x.size(); i += 4)
        {
            bool px_diff = false;
            for (int c = 0; c < 4; ++c)
            {
                const int d = std::abs(static_cast<int>(x[i + c]) - static_cast<int>(y[i + c]));
                max_abs = std::max(max_abs, d);
                if (d != 0) px_diff = true;
            }
            if (px_diff) ++diff_pixels;
        }
    };

    // A-alone vs A-with-B → the isolation signal (want ~0).
    std::size_t aa_diff = 0; int aa_max = 0;
    pixelDiff(bufA0, bufA1, aa_diff, aa_max);
    // A vs B → the "scenes genuinely differ" signal (want large).
    std::size_t ab_diff = 0; int ab_max = 0;
    pixelDiff(bufA0, bufB, ab_diff, ab_max);

    // Content of A-alone (non-blank + lit), so equality isn't a both-blank pass.
    bool a0_uniform = true; std::size_t a0_bright = 0;
    for (std::size_t i = 0; i + 4 <= bufA0.size(); i += 4)
    {
        if (i >= 4 && (bufA0[i] != bufA0[0] || bufA0[i+1] != bufA0[1] ||
                       bufA0[i+2] != bufA0[2] || bufA0[i+3] != bufA0[3]))
            a0_uniform = false;
        if (std::max({bufA0[i], bufA0[i+1], bufA0[i+2]}) > 40) ++a0_bright;
    }

    const std::size_t total = static_cast<std::size_t>(W) * H;
    // Allow ±1/channel for driver FP reordering; the isolation guarantee is
    // that B's existence does not perceptibly change A.
    const bool isolated = (bufA0.size() == bufA1.size()) && (aa_max <= 1);

    std::cout << "  A-alone vs A-with-B: diff_pixels=" << aa_diff << "/" << total
              << " max_abs_diff=" << aa_max << "\n";
    std::cout << "  A vs B (opposite light): diff_pixels=" << ab_diff << "/" << total
              << " max_abs_diff=" << ab_max << "\n";
    std::cout << "  A-alone: uniform=" << (a0_uniform ? "yes" : "no")
              << " bright=" << a0_bright << "/" << total << "\n";

    int fails = 0;
    auto check = [&](bool cond, const char* name)
    { std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << "\n"; if (!cond) ++fails; };

    check(rb0.status == 0 && rb1.status == 0 && rbB.status == 0, "all readbacks status == 0");
    check(rb0.bytes_written == static_cast<std::uint64_t>(W) * H * 4, "A-alone full bytes");
    check(!a0_uniform,           "scene A renders content (not a blank image)");
    check(a0_bright > 300,       "scene A has lit pixels");
    check(ab_diff > 1000,        "scene B (opposite light) renders differently from A");
    check(isolated,
          "scene A is UNCHANGED by coexisting scene B (light/shadow isolated)");

    sync->requestStop();
    server_thread.join();

    std::cout << "=== two_scene_shadow_isolation_test "
              << (fails == 0 ? "PASSED" : "FAILED") << " (fails=" << fails << ") ===\n";
    return fails == 0 ? 0 : 1;
}
