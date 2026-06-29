// ============================================================================
//  game_loop_test.cpp
//
//  Game-loop style end-to-end test for the render comm pipeline.
//  Uses RenderSession for high-level semantic API instead of raw
//  FrameProgramBuilder payload construction.
// ============================================================================

#include <lux/engine/render/comm/client/RenderSession.hpp>
#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/render/comm/RenderProtocol.hpp>

#include <lux/engine/render/renderer/features/forward/ForwardMeshOperation.hpp>
#include <lux/engine/render/renderer/features/shadow/ShadowMapOperation.hpp>
#include <lux/engine/render/renderer/features/shadow/MeshShadowOperation.hpp>
#include <lux/engine/render/renderer/features/material/MaterialOperation.hpp>  // kStandardMaterialFeatureFactory (standard material stack; no config; attaches before StandardMeshStack)
#include <lux/engine/render/renderer/features/meshstack/MeshStackOperation.hpp>  // kStandardMeshStackFeatureFactory (standard 3D mesh stack; no config)
#include <lux/engine/render/renderer/features/light/LightOperation.hpp>  // LightFeature / LightProxy / LightOperationIds / LightDescriptor / DirectionalLightDesc / LIGHT_FLAG_CAST_SHADOW
#include <lux/engine/render/renderer/features/view_camera/ViewCameraOperation.hpp>  // kStandardViewCameraFeatureFactory / ViewCameraProxy / ViewCameraOperationIds (per-view camera data; no config; attaches before every camera consumer)

#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/description/Vertex.hpp>
#include <lux/engine/render/resources/material/GraphMaterialData.hpp>

#include "graph_test_helpers.hpp"

#include <lux/engine/window/LuxWindow.hpp>
#include <GLFW/glfw3.h>

#include <Eigen/Geometry>  // Eigen::Vector3f::cross — no longer pulled in transitively via render headers


#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace lux::render;

// ── Tiny test harness ───────────────────────────────────────────────────

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const char *name)
{
    if (cond)
    {
        std::cout << "  [PASS] " << name << "\n";
        ++g_pass;
    }
    else
    {
        std::cout << "  [FAIL] " << name << "\n";
        ++g_fail;
    }
}

// ── Simple geometry builders ────────────────────────────────────────────

static void buildGroundPlane(std::vector<lux::rdesc::Vertex> &verts,
                             std::vector<uint32_t> &indices,
                             float half_size = 5.f)
{
    using V3 = Eigen::Vector3f;
    using V2 = Eigen::Vector2f;
    V3 n{0, 1, 0}, t{1, 0, 0}, bt{0, 0, 1};
    verts = {
        {V3{-half_size, 0, half_size}, n, t, V2{0, 0}, bt},
        {V3{half_size, 0, half_size}, n, t, V2{1, 0}, bt},
        {V3{half_size, 0, -half_size}, n, t, V2{1, 1}, bt},
        {V3{-half_size, 0, -half_size}, n, t, V2{0, 1}, bt},
    };
    indices = {0, 1, 2, 0, 2, 3};
}

static void buildCube(std::vector<lux::rdesc::Vertex> &verts,
                      std::vector<uint32_t> &indices,
                      float half = 0.5f)
{
    using V3 = Eigen::Vector3f;
    using V2 = Eigen::Vector2f;

    struct FaceInfo
    {
        V3 n, t, bt;
        V3 corners[4];
    };
    FaceInfo faces[6] = {
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}, {{-half, -half, half}, {half, -half, half}, {half, half, half}, {-half, half, half}}},
        {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}, {{half, -half, -half}, {-half, -half, -half}, {-half, half, -half}, {half, half, -half}}},
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}, {{half, -half, half}, {half, -half, -half}, {half, half, -half}, {half, half, half}}},
        {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}, {{-half, -half, -half}, {-half, -half, half}, {-half, half, half}, {-half, half, -half}}},
        {{0, 1, 0}, {1, 0, 0}, {0, 0, 1}, {{-half, half, half}, {half, half, half}, {half, half, -half}, {-half, half, -half}}},
        {{0, -1, 0}, {1, 0, 0}, {0, 0, -1}, {{-half, -half, -half}, {half, -half, -half}, {half, -half, half}, {-half, -half, half}}},
    };
    V2 uvs[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};

    verts.reserve(24);
    indices.reserve(36);
    for (auto &f : faces)
    {
        auto base = static_cast<uint32_t>(verts.size());
        for (int i = 0; i < 4; ++i)
            verts.push_back({f.corners[i], f.n, f.t, uvs[i], f.bt});
        indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
}

// ── Matrix helpers ──────────────────────────────────────────────────────

static void setIdentity(float m[16])
{
    std::memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.f;
}

static void setTranslation(float m[16], float x, float y, float z)
{
    setIdentity(m);
    m[12] = x;
    m[13] = y;
    m[14] = z;
}

// ── Vulkan instance extensions ──────────────────────────────────────────

static std::vector<const char*> getVulkanExtensions()
{
    glfwInit();
    uint32_t count = 0;
    const char **exts = glfwGetRequiredInstanceExtensions(&count);
    std::vector<const char*> result;
    result.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
        result.emplace_back(exts[i]);
    return result;
}

// ── Build look-at view matrix ───────────────────────────────────────────

static Eigen::Matrix4f buildViewMatrix(const Eigen::Vector3f &eye,
                                       const Eigen::Vector3f &target,
                                       const Eigen::Vector3f &up)
{
    Eigen::Vector3f f = (target - eye).normalized();
    Eigen::Vector3f s = f.cross(up).normalized();
    Eigen::Vector3f u = s.cross(f);

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

// ── Build perspective projection matrix (Vulkan Y-down, ZO depth) ────────

static Eigen::Matrix4f buildProjMatrix(float fov_rad, float aspect,
                                       float near_z, float far_z)
{
    float tanHalf = std::tan(fov_rad * 0.5f);
    Eigen::Matrix4f P = Eigen::Matrix4f::Zero();
    P(0, 0) = 1.f / (aspect * tanHalf);
    P(1, 1) = -1.f / tanHalf;
    P(2, 2) = -far_z / (far_z - near_z);
    P(2, 3) = -(far_z * near_z) / (far_z - near_z);
    P(3, 2) = -1.f;
    return P;
}

// ── Phase state machine ─────────────────────────────────────────────────

enum class Phase
{
    CreateScene,
    ActivateScene,
    BindSwapchain,
    RegisterFeatures,
    AddShadowFeatures,
    AddForwardFeature,
    UploadMaterials,
    UploadMeshes,
    WaitMeshUpload,
    CreateInstances,
    CreateLight,
    Render,
    Done
};

static const char *phaseName(Phase p)
{
    switch (p)
    {
    case Phase::CreateScene:
        return "CreateScene";
    case Phase::ActivateScene:
        return "ActivateScene";
    case Phase::BindSwapchain:
        return "BindSwapchain";
    case Phase::RegisterFeatures:
        return "RegisterFeatures";
    case Phase::AddShadowFeatures:
        return "AddShadowFeatures";
    case Phase::AddForwardFeature:
        return "AddForwardFeature";
    case Phase::UploadMaterials:
        return "UploadMaterials";
    case Phase::UploadMeshes:
        return "UploadMeshes";
    case Phase::WaitMeshUpload:
        return "WaitMeshUpload";
    case Phase::CreateInstances:
        return "CreateInstances";
    case Phase::CreateLight:
        return "CreateLight";
    case Phase::Render:
        return "Render";
    case Phase::Done:
        return "Done";
    }
    return "?";
}

// ── main ────────────────────────────────────────────────────────────────

int main()
{
    std::cout << "=== Game Loop Test ===\n\n";

    // ── Pre-build geometry ──────────────────────────────────────────────

    std::vector<lux::rdesc::Vertex> ground_verts, cube_verts;
    std::vector<uint32_t> ground_idx, cube_idx;
    buildGroundPlane(ground_verts, ground_idx);
    buildCube(cube_verts, cube_idx);

    auto ground_mesh = lux::rdesc::Mesh{
        std::move(ground_verts), std::move(ground_idx),
        lux::math::AABB(Eigen::Vector3f(-5.f, 0.f, -5.f), Eigen::Vector3f(5.f, 0.f, 5.f))};
    auto cube_mesh = lux::rdesc::Mesh{
        std::move(cube_verts), std::move(cube_idx),
        lux::math::AABB(Eigen::Vector3f(-0.5f, -0.5f, -0.5f), Eigen::Vector3f(0.5f, 0.5f, 0.5f))};

    // ── Precompute camera matrices ──────────────────────────────────────
    Eigen::Vector3f eye(3.f, 3.f, 3.f);
    Eigen::Vector3f target(0.f, 0.5f, 0.f);
    Eigen::Vector3f up(0.f, 1.f, 0.f);

    Eigen::Matrix4f V = buildViewMatrix(eye, target, up);
    Eigen::Matrix4f P = buildProjMatrix(60.f * 3.14159265f / 180.f, 1280.f / 720.f, 0.1f, 100.f);

    // ── Channel + sync + window ─────────────────────────────────────────

    auto channel = RenderProgramChannel<>::create();
    auto sync = std::make_shared<RenderChannelSync>();

    auto surface_exts = getVulkanExtensions();
    lux::window::LuxWindow window(1280, 720, "Game Loop Test");

    // ── Server thread ───────────────────────────────────────────────────

    std::atomic<bool> server_ready{false};
    std::atomic<bool> server_failed{false};

    std::thread server_thread(
        [&]{
            GeneralRenderServer server(channel, sync);

            ServerConfig cfg;
            cfg.instance_extensions = surface_exts;
            if (auto r = server.init(std::move(cfg)); !r)
            {
                std::cerr << "[Server] Init failed: " << r.error().message() << "\n";
                server_failed.store(true, std::memory_order_release);
                server_ready.store(true, std::memory_order_release);
                return;
            }
            if (auto r = server.attachToWindow(window); !r)
            {
                std::cerr << "[Server] Attach failed: " << r.error().message() << "\n";
                server_failed.store(true, std::memory_order_release);
                server_ready.store(true, std::memory_order_release);
                return;
            }
            server_ready.store(true, std::memory_order_release);

            while (server.tick()){} 
        }
    );

    while (!server_ready.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    if (server_failed.load())
    {
        std::cerr << "Server init failed (no Vulkan?). Skipping test.\n";
        sync->requestStop();
        server_thread.join();
        return 0;
    }

    RenderSession session(channel, sync);

    // ════════════════════════════════════════════════════════════════════
    //  State — all variables that persist across frames
    // ════════════════════════════════════════════════════════════════════

    Phase phase = Phase::CreateScene;
    uint32_t frame = 0;

    // CreateScene
    RenderRequest<SceneCreatedReply> scene_req;

    // ActivateScene
    RenderRequest<GenericOkReply> active_req;
    RenderRequest<ViewCreatedReply> view_req;

    // RegisterFeatures
    RenderRequest<FeatureTypeRegisteredReply> fwd_reg_req, shadow_reg_req, mesh_shadow_reg_req, light_reg_req, material_reg_req, mesh_stack_reg_req, view_cam_reg_req;

    // ViewCamera feature op ids — resolved from view_cam_reg_req once the
    // registration reply lands, then used by ViewCameraProxy for the per-view
    // camera update. RenderSession::updateView is gone — the per-view camera data
    // is a StandardViewCameraFeature domain now.
    ViewCameraOperationIds view_cam_ops{};

    // Light feature op ids — resolved from light_reg_req once the registration
    // reply lands, then used by LightProxy to create the directional light.
    // (Light is a feature now: RenderSession::createLight is gone.)
    LightOperationIds light_ops{};

    // Mesh-stack feature op ids — resolved from mesh_stack_reg_req once the
    // registration reply lands, then used by MeshStackProxy for instance CRUD
    // (add / per-view visibility / transform). RenderSession's mesh-instance
    // methods are gone — these ops are a StandardMeshStackFeature domain now.
    MeshStackOperationIds mesh_stack_ops{};

    // Material feature op ids — resolved from material_reg_req once the
    // registration reply lands, then used by MaterialProxy to upload graph
    // materials. RenderSession::uploadGraphMaterial is gone — these ops are a
    // StandardMaterialFeature domain now.
    MaterialOperationIds material_ops{};

    // AddFeatures
    RenderRequest<FeatureAddedReply> fwd_feat_req, shadow_feat_req, mesh_shadow_feat_req, light_feat_req, material_feat_req, mesh_stack_feat_req, view_cam_feat_req;

    // UploadMaterials
    RenderRequest<MaterialUploadedReply> ground_mat_req, cube_mat_req;

    // UploadMeshes
    RenderRequest<MeshUploadedReply> ground_mesh_req, cube_mesh_req;

    // CreateInstances
    RenderRequest<MeshInstanceSlotReply> ground_inst_req, cube_inst_req;

    // CreateLight
    RenderRequest<LightCreatedReply> light_req;

    // Render
    constexpr int kRenderFrames = 500;
    int render_count = 0;
    auto start_time = std::chrono::steady_clock::now();
    constexpr float rotation_speed = 1.0f;

    // ════════════════════════════════════════════════════════════════════
    //  Main game loop
    // ════════════════════════════════════════════════════════════════════
    while (!window.shouldClose())
    {
        window.pollEvents();
        session.beginFrame();
        {
            switch (phase)
            {
            // ────────────────────────────────────────────────────────────
            case Phase::CreateScene:
            {
                scene_req = session.createScene("GameScene");
                break;
            }

            // ────────────────────────────────────────────────────────────
            case Phase::ActivateScene:
            {
                active_req = session.setActiveScene(scene_req.result().scene_id, true);
                view_req = session.addView(
                    scene_req.result().scene_id, {1280, 720}, "MainView");
                break;
            }

            // ────────────────────────────────────────────────────────────
            case Phase::BindSwapchain:
            {
                session.bindSwapchain(scene_req.result().scene_id, view_req.result().view);
                break;
            }

            // ────────────────────────────────────────────────────────────
            case Phase::RegisterFeatures:
            {
                // StandardViewCameraFeature owns the per-view 3D camera data; it
                // MUST attach before every camera consumer (shadow / forward), so
                // register it ahead of the rest — owner-first.
                view_cam_reg_req = session.registerFeatureType(kStandardViewCameraFeatureFactory);
                fwd_reg_req = session.registerFeatureType(kForwardMeshFeatureFactory);
                shadow_reg_req = session.registerFeatureType(kShadowMapFeatureFactory);
                mesh_shadow_reg_req = session.registerFeatureType(kMeshShadowFeatureFactory);
                light_reg_req = session.registerFeatureType(kLightFeatureFactory);
                // StandardMaterialFeature owns the global material stack; it MUST
                // attach before StandardMeshStack (addMeshInstance reads the material
                // slot) and every material consumer. Register it ahead of mesh-stack.
                material_reg_req = session.registerFeatureType(kStandardMaterialFeatureFactory);
                mesh_stack_reg_req = session.registerFeatureType(kStandardMeshStackFeatureFactory);
                break;
            }
            // ────────────────────────────────────────────────────────────
            case Phase::AddShadowFeatures:
            {
                // StandardViewCameraFeature owns the per-view 3D camera data that
                // shadow/forward consume. It MUST attach before every camera
                // consumer — owner-first. No comm config — empty struct param.
                struct EmptyViewCamCfg {} view_cam_cfg{};
                view_cam_feat_req = session.addFeature(
                    scene_req.result().scene_id,
                    view_cam_reg_req.result().feature_type_id, view_cam_cfg);

                // Light feature MUST attach before ShadowMapFeature: the shadow
                // feature caches a raw LightResources* at attach time.
                struct EmptyLightCfg {} light_cfg{};
                light_feat_req = session.addFeature(
                    scene_req.result().scene_id,
                    light_reg_req.result().feature_type_id, light_cfg);

                // StandardMaterialFeature owns the global material stack
                // (ShadingModelRegistry + MaterialResources). It MUST attach before
                // StandardMeshStack (addMeshInstance reads the material slot) and
                // every material consumer. No comm config — empty struct param.
                struct EmptyMaterialCfg {} material_cfg{};
                material_feat_req = session.addFeature(
                    scene_req.result().scene_id,
                    material_reg_req.result().feature_type_id, material_cfg);

                // StandardMeshStackFeature owns the standard 3D mesh-stack scene
                // resources (no longer built in RenderScene's ctor). It MUST attach
                // before every mesh consumer (ShadowMap / MeshShadow / ForwardMesh)
                // or the meshes don't render. No comm config — empty struct param.
                struct EmptyMeshStackCfg {} mesh_stack_cfg{};
                mesh_stack_feat_req = session.addFeature(
                    scene_req.result().scene_id,
                    mesh_stack_reg_req.result().feature_type_id, mesh_stack_cfg);

                ShadowMapCommConfig scc{};
                scc.atlas_page_resolution = 2048;
                scc.atlas_page_count = 2;
                scc.max_shadow_slices = 64;
                shadow_feat_req = session.addFeature(
                    scene_req.result().scene_id,
                    shadow_reg_req.result().feature_type_id, scc);

                MeshShadowCommConfig mscc{};
                mscc.comm_config_version = kMeshShadowCommConfigVersion;
                mscc.descriptor_layout_version = kMeshShadowDescriptorLayoutVersion;
                mesh_shadow_feat_req = session.addFeature(
                    scene_req.result().scene_id,
                    mesh_shadow_reg_req.result().feature_type_id, mscc);
                break;
            }

            // ────────────────────────────────────────────────────────────
            case Phase::AddForwardFeature:
            {
                ForwardMeshCommConfig cc{};
                cc.comm_config_version = kForwardMeshCommConfigVersion;
                cc.descriptor_layout_version = kForwardMeshDescriptorLayoutVersion;
                fwd_feat_req = session.addFeature(
                    scene_req.result().scene_id,
                    fwd_reg_req.result().feature_type_id, cc);
                break;
            }

            // ────────────────────────────────────────────────────────────
            case Phase::UploadMaterials:
            {
                // rdesc::Material was retired: each surface is now an Unlit GRAPH
                // material compiled for the Forward pass. Distinct baked colours =>
                // distinct frags => distinct per-material PSOs (R1). The compile +
                // upload are driven synchronously here so the persistent
                // ground_mat_req / cube_mat_req hold the upload replies the phase
                // transition validates (status == 0), exactly as before.
                const auto compileForwardColor =
                    [&](float r, float g, float b) -> ShaderHandle
                {
                    auto cb = lux::mgtest::compileGraphPass(
                        lux::mgtest::makeColorGraph(
                            r, g, b,
                            lux::rdesc::EMaterialShadingModel::Unlit,
                            /*metallic=*/0.f, /*roughness=*/0.6f,
                            /*world_normal_input=*/false,
                            /*emissive_scale=*/0.f),
                        lux::rdesc::EMaterialPass::Forward);
                    if (!cb) {
                        std::cerr << "graph -> SPIR-V failed: " << cb.error() << "\n";
                        return ShaderHandle{};
                    }
                    std::vector<uint32_t>  spv  = std::move(cb->spirv);
                    std::vector<std::byte> info = std::move(cb->info_bytes);
                    const auto bytes = std::span<const std::byte>{
                        reinterpret_cast<const std::byte*>(spv.data()),
                        spv.size() * sizeof(uint32_t)};
                    // syncCall submits the open frame, blocks for the reply, then
                    // begins a fresh frame — so a frame stays open for the
                    // uploadGraphMaterial calls + the loop's trailing submitFrame.
                    const auto comp = session.syncCall(
                        session.compileShader(
                            bytes,
                            std::span<const std::byte>{info.data(), info.size()}));
                    return comp.shader;
                };

                const ShaderHandle ground_frag = compileForwardColor(0.2f, 0.6f, 0.2f);
                const ShaderHandle cube_frag    = compileForwardColor(0.85f, 0.45f, 0.1f);

                GraphMaterialData ground_gd{};
                ground_mat_req = MaterialProxy(session, material_ops).uploadGraphMaterial(
                    ground_gd, ShaderHandle{}, ground_frag);

                GraphMaterialData cube_gd{};
                cube_mat_req = MaterialProxy(session, material_ops).uploadGraphMaterial(
                    cube_gd, ShaderHandle{}, cube_frag);
                break;
            }

            // ────────────────────────────────────────────────────────────
            case Phase::UploadMeshes:
            {
                ground_mesh_req = lux::render::MeshStackProxy(session, mesh_stack_ops).uploadMesh(ground_mesh);
                cube_mesh_req = lux::render::MeshStackProxy(session, mesh_stack_ops).uploadMesh(cube_mesh);
                break;
            }

            // ────────────────────────────────────────────────────────────
            case Phase::WaitMeshUpload:
                // Empty frame — ticking so deferred upload replies arrive.
                break;

            // ────────────────────────────────────────────────────────────
            case Phase::CreateInstances:
            {
                float identity[16];
                setIdentity(identity);
                ground_inst_req = MeshStackProxy(session, mesh_stack_ops).addMeshInstance(
                    scene_req.result().scene_id,
                    ground_mesh_req.result().handle,
                    ground_mat_req.result().handle,
                    identity);

                float cube_xform[16];
                setTranslation(cube_xform, 0.f, 0.5f, 0.f);
                cube_inst_req = MeshStackProxy(session, mesh_stack_ops).addMeshInstance(
                    scene_req.result().scene_id,
                    cube_mesh_req.result().handle,
                    cube_mat_req.result().handle,
                    cube_xform);
                break;
            }

            // ────────────────────────────────────────────────────────────
            case Phase::CreateLight:
            {
                MeshStackProxy(session, mesh_stack_ops).makeInstanceVisibleForView(
                    scene_req.result().scene_id, view_req.result().view,
                    ground_inst_req.result().object);
                MeshStackProxy(session, mesh_stack_ops).makeInstanceVisibleForView(
                    scene_req.result().scene_id, view_req.result().view,
                    cube_inst_req.result().object);

                DirectionalLightDesc dl{};
                float dx = -0.5f, dy = -1.f, dz = -0.3f;
                float len = std::sqrt(dx * dx + dy * dy + dz * dz);
                dl.direction = Eigen::Vector3f(dx / len, dy / len, dz / len);
                dl.color = Eigen::Vector3f(1.f, 0.95f, 0.8f);
                dl.intensity = 1.5f;
                dl.flags = LIGHT_FLAG_CAST_SHADOW;
                dl.shadow_map_size = 2048;
                dl.shadow_bias = 0.005f;
                dl.shadow_normal_bias = 0.01f;
                dl.cascade_count = 4;
                dl.cascade_splits = {0.1f, 0.25f, 0.5f, 1.0f};

                light_req = LightProxy(session, light_ops)
                                .createLight(scene_req.result().scene_id, LightDescriptor{dl});
                break;
            }

            // ────────────────────────────────────────────────────────────
            case Phase::Render:
            {
                auto now = std::chrono::steady_clock::now();
                float elapsed = std::chrono::duration<float>(now - start_time).count();
                float angle = elapsed * rotation_speed;

                float cos_a = std::cos(angle);
                float sin_a = std::sin(angle);
                float cube_transform[16];
                std::memset(cube_transform, 0, sizeof(cube_transform));
                cube_transform[0] = cos_a;
                cube_transform[2] = -sin_a;
                cube_transform[5] = 1.f;
                cube_transform[8] = sin_a;
                cube_transform[10] = cos_a;
                cube_transform[12] = 0.f;
                cube_transform[13] = 0.5f;
                cube_transform[15] = 1.f;

                MeshStackProxy(session, mesh_stack_ops).updateTransform(cube_inst_req.result().object, cube_transform);

                ViewCameraProxy(session, view_cam_ops).update(
                    scene_req.result().scene_id, view_req.result().view,
                    V.data(), P.data(),
                    eye.data());
                break;
            }
            }

            // ════════════════════════════════════════════════════════════════
            //  Phase transition + validation
            // ════════════════════════════════════════════════════════════════
            // session.removeView(scene_req.result().scene_id, view_req.result().view);
            // session.destroyScene(scene_req.result().scene_id);
            session.submitFrame(true);
            session.waitAndPumpReplies();

            switch (phase)
            {
            case Phase::CreateScene:
                check(scene_req.isReady(), "CreateScene — reply received");
                phase = Phase::ActivateScene;
                break;

            case Phase::ActivateScene:
                check(active_req.isReady(), "SetActiveScene — reply received");
                check(active_req.result().code == 0, "SetActiveScene — code == 0");
                check(view_req.isReady(), "AddView — reply received");
                check(view_req.result().view.valid(), "AddView — handle valid");
                if (!scene_req.isReady() || !view_req.isReady() ||
                    !view_req.result().view.valid() || active_req.result().code != 0)
                {
                    std::cerr << "Critical setup failed; aborting.\n";
                    phase = Phase::Done;
                }
                else
                {
                    // bindSwapchain deferred to next frame's recording phase.
                    phase = Phase::BindSwapchain;
                }
                break;

            case Phase::BindSwapchain:
                phase = Phase::RegisterFeatures;
                break;

            case Phase::RegisterFeatures:
                check(fwd_reg_req.isReady(), "RegisterFeatureType ForwardMesh — OK");
                check(fwd_reg_req.result().feature_type_id > 0, "RegisterFeatureType ForwardMesh — valid id");
                check(shadow_reg_req.isReady(), "RegisterFeatureType ShadowMap — OK");
                check(mesh_shadow_reg_req.isReady(), "RegisterFeatureType MeshShadow — OK");
                check(light_reg_req.isReady(), "RegisterFeatureType Light — OK");
                check(view_cam_reg_req.isReady(), "RegisterFeatureType StandardViewCamera — OK");
                view_cam_ops = ViewCameraOperationIds::fromOps(
                    view_cam_reg_req.result().ops, view_cam_reg_req.result().op_count);
                light_ops = LightOperationIds::fromOps(
                    light_reg_req.result().ops, light_reg_req.result().op_count);
                mesh_stack_ops = MeshStackOperationIds::fromOps(
                    mesh_stack_reg_req.result().ops, mesh_stack_reg_req.result().op_count);
                material_ops = MaterialOperationIds::fromOps(
                    material_reg_req.result().ops, material_reg_req.result().op_count);
                phase = Phase::AddShadowFeatures;
                break;

            case Phase::AddShadowFeatures:
                check(view_cam_feat_req.isReady(), "AddFeature StandardViewCamera — OK");
                check(view_cam_feat_req.result().feature.valid(), "AddFeature StandardViewCamera — valid handle");
                check(light_feat_req.isReady(), "AddFeature Light — OK");
                check(light_feat_req.result().feature.valid(), "AddFeature Light — valid handle");
                check(shadow_feat_req.isReady(), "AddFeature ShadowMap — OK");
                check(shadow_feat_req.result().feature.valid(), "AddFeature ShadowMap — valid handle");
                check(mesh_shadow_feat_req.isReady(), "AddFeature MeshShadow — OK");
                check(mesh_shadow_feat_req.result().feature.valid(), "AddFeature MeshShadow — valid handle");
                phase = Phase::AddForwardFeature;
                break;

            case Phase::AddForwardFeature:
                check(fwd_feat_req.isReady(), "AddFeature ForwardMesh — OK");
                check(fwd_feat_req.result().feature.valid(), "AddFeature ForwardMesh — valid handle");
                phase = Phase::UploadMaterials;
                break;

            case Phase::UploadMaterials:
                check(ground_mat_req.isReady() && ground_mat_req.result().status == 0,
                      "UploadMaterial ground — OK");
                check(cube_mat_req.isReady() && cube_mat_req.result().status == 0,
                      "UploadMaterial cube — OK");
                phase = Phase::UploadMeshes;
                break;

            case Phase::UploadMeshes:
                phase = Phase::WaitMeshUpload;
                break;

            case Phase::WaitMeshUpload:
                if (ground_mesh_req.isReady() && cube_mesh_req.isReady())
                {
                    check(ground_mesh_req.result().status == 0 &&
                              cube_mesh_req.result().status == 0,
                          "UploadMesh ground + cube — deferred reply OK");
                    phase = Phase::CreateInstances;
                }
                break;

            case Phase::CreateInstances:
                check(ground_inst_req.isReady() &&
                          static_cast<bool>(ground_inst_req.result().object),
                      "AddMeshInstance ground — valid object");
                check(cube_inst_req.isReady() &&
                          static_cast<bool>(cube_inst_req.result().object),
                      "AddMeshInstance cube — valid object");
                phase = Phase::CreateLight;
                break;

            case Phase::CreateLight:
                check(light_req.isReady() && light_req.result().status == 0,
                      "CreateLight directional — OK");
                start_time = std::chrono::steady_clock::now();
                phase = Phase::Render;
                break;

            case Phase::Done:
                break;
            }
            ++frame;
        }
    }
    // ════════════════════════════════════════════════════════════════════
    //  Shutdown
    // ════════════════════════════════════════════════════════════════════
    sync->requestStop();
    if (server_thread.joinable())
        server_thread.join();

    std::cout << "\n=== Results: " << g_pass << " passed, " << g_fail << " failed ===\n";
    return g_fail > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
