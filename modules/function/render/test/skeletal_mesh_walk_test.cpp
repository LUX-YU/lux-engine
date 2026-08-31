// ============================================================================
//  skeletal_mesh_walk_test.cpp — end-to-end GPU-skinning visual test
//
//  End-to-end GPU-skinning visual test. Self-contained: builds a procedural
//  2-bone "bending box" (no asset/model dependency, always runs) and drives
//  the full skinning pipeline:
//
//    bone palette (CPU, animated) --uploadBonePalette--> SkinningResources
//      --SkinningFeature compute (skin_compute.comp)--> transient vertex pool
//      --ForwardMesh _vp pipeline (set 7)--> screen
//
//  The box's upper half is weighted to a bone that rotates back and forth, so
//  a working skinning path shows the box bending/swaying. A static ground
//  plane confirms static + skinned meshes coexist (variant routing in the
//  shared MeshDraw kernel).
//
//  WINDOWED: open a window and render until closed (close manually). The real
//  rigged-model path (LUX_ANIMATION_TEST_MODEL via World + RenderableSystem)
//  is a follow-up — RenderableSystem's synchronous addTransientMeshInstance().tryResult()->get()
//  needs an async-readiness fix before it can drive a windowed frame loop.
// ============================================================================

#include <lux/engine/function/render/client/RenderProgramSession.hpp>
#include <lux/engine/function/render/client/RenderControlSession.hpp>
#include <lux/engine/function/render/client/RenderUploadSession.hpp>
#include <lux/engine/render/testing/DirectRenderUploadClient.hpp>
#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/function/render/client/RenderProtocol.hpp>

#include <lux/engine/function/render/client/genops/ForwardMeshOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/ShadowMapOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MeshShadowOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MaterialOperation.ops.hpp> // kMaterialFeatureFactory
#include <lux/engine/function/render/client/genops/MeshStackOperation.ops.hpp> // kMeshStackFeatureFactory
#include <lux/engine/function/render/client/genops/SkinningOperation.ops.hpp>
// LightFeature / LightProxy / LightOperationIds
#include <lux/engine/function/render/client/genops/LightOperation.ops.hpp>
// StandardViewCamera (per-view camera op; replaces RenderProgramSession::updateView)
#include <lux/engine/function/render/client/genops/ViewCameraOperation.ops.hpp>

#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/description/Vertex.hpp>
#include <lux/engine/description/Skeleton.hpp>
#include <lux/engine/description/AnimationClip.hpp>
#include <lux/engine/animation/PoseSampling.hpp> // buildSkinningMatrices

#include <lux/engine/function/render/client/resources/material/GraphMaterialData.hpp>
#include "graph_test_helpers.hpp" // mgtest::makeColorGraph + compileGraphPass

#include <lux/engine/window/LuxWindow.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <thread>
#include <vector>

using namespace lux::render;

// ── Matrix helpers ──────────────────────────────────────────────────────
static Eigen::Matrix4f
buildViewMatrix(const Eigen::Vector3f& eye, const Eigen::Vector3f& target, const Eigen::Vector3f& up)
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

static Eigen::Matrix4f
buildProjMatrix(float fov_rad, float aspect, float near_z, float far_z)
{
    float t = std::tan(fov_rad * 0.5f);
    Eigen::Matrix4f P = Eigen::Matrix4f::Zero();
    P(0, 0) = 1.f / (aspect * t);
    P(1, 1) = -1.f / t;
    P(2, 2) = -far_z / (far_z - near_z);
    P(2, 3) = -(far_z * near_z) / (far_z - near_z);
    P(3, 2) = -1.f;
    return P;
}

static std::vector<const char*>
getVulkanExtensions()
{
    const auto exts = lux::window::LuxWindow::requiredVulkanInstanceExtensions();
    return {exts.begin(), exts.end()};
}

// ── Geometry ────────────────────────────────────────────────────────────

static void
buildGroundPlane(std::vector<lux::rdesc::Vertex>& v, std::vector<uint32_t>& idx)
{
    using V3 = Eigen::Vector3f;
    using V2 = Eigen::Vector2f;
    V3 n{0, 1, 0}, t{1, 0, 0}, bt{0, 0, 1};
    lux::rdesc::Vertex a{V3{-6, 0, 6}, n, t, V2{0, 0}, bt}, b{V3{6, 0, 6}, n, t, V2{1, 0}, bt},
        c{V3{6, 0, -6}, n, t, V2{1, 1}, bt}, d{V3{-6, 0, -6}, n, t, V2{0, 1}, bt};
    v = {a, b, c, d};
    idx = {0, 1, 2, 0, 2, 3};
}

// Tall box from y=0 to y=H. Each vertex is weighted between bone 0 (bottom)
// and bone 1 (top) by its height, so when bone 1 rotates the box bends.
static constexpr float kBoxHeight = 6.0f;

static void
buildBendyBox(std::vector<lux::rdesc::Vertex>& verts, std::vector<uint32_t>& indices)
{
    using V3 = Eigen::Vector3f;
    using V2 = Eigen::Vector2f;
    const float hx = 0.8f, hz = 0.8f, H = kBoxHeight;
    struct Face
    {
        V3 n, t, bt;
        V3 c[4];
    };
    Face faces[6] = {
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}, {{-hx, 0, hz}, {hx, 0, hz}, {hx, H, hz}, {-hx, H, hz}}},
        {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}, {{hx, 0, -hz}, {-hx, 0, -hz}, {-hx, H, -hz}, {hx, H, -hz}}},
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}, {{hx, 0, hz}, {hx, 0, -hz}, {hx, H, -hz}, {hx, H, hz}}},
        {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}, {{-hx, 0, -hz}, {-hx, 0, hz}, {-hx, H, hz}, {-hx, H, -hz}}},
        {{0, 1, 0}, {1, 0, 0}, {0, 0, 1}, {{-hx, H, hz}, {hx, H, hz}, {hx, H, -hz}, {-hx, H, -hz}}},
        {{0, -1, 0}, {1, 0, 0}, {0, 0, -1}, {{-hx, 0, -hz}, {hx, 0, -hz}, {hx, 0, hz}, {-hx, 0, hz}}},
    };
    V2 uvs[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (auto& f : faces)
    {
        auto base = static_cast<uint32_t>(verts.size());
        for (int i = 0; i < 4; ++i)
        {
            lux::rdesc::Vertex vx{};
            vx.position = f.c[i];
            vx.normal = f.n;
            vx.tangent = f.t;
            vx.uv = uvs[i];
            vx.bitangent = f.bt;
            const float w1 = std::clamp(f.c[i].y() / H, 0.0f, 1.0f); // top → bone 1
            vx.bone.bone_ids[0] = 0;
            vx.bone.bone_ids[1] = 1;
            vx.bone.bone_ids[2] = 0;
            vx.bone.bone_ids[3] = 0;
            vx.bone.weights[0] = 1.0f - w1;
            vx.bone.weights[1] = w1;
            vx.bone.weights[2] = 0.0f;
            vx.bone.weights[3] = 0.0f;
            verts.push_back(vx);
        }
        indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
}

// 2-bone skeleton, both bind = identity at origin (so inv_bind = identity).
static lux::rdesc::Skeleton
buildTwoBoneSkeleton()
{
    lux::rdesc::Skeleton s;
    s.bones.resize(2);
    s.bones[0].name = "root";
    s.bones[0].parent_index = -1;
    s.bones[0].bind_local = Eigen::Affine3f::Identity();
    s.bones[0].inv_bind_world = Eigen::Affine3f::Identity();
    s.bones[1].name = "upper";
    s.bones[1].parent_index = 0;
    s.bones[1].bind_local = Eigen::Affine3f::Identity();
    s.bones[1].inv_bind_world = Eigen::Affine3f::Identity();
    return s;
}

// ── Phases ──────────────────────────────────────────────────────────────
enum class Phase
{
    CreateScene,
    ActivateScene,
    BindSwapchain,
    RegisterFeatures,
    AddShadowFeatures,
    AddForwardFeature,
    AddSkinningFeature,
    CompileShaders,
    UploadMaterials,
    UploadMeshes,
    WaitMeshUpload,
    CreateInstances,
    CreateLight,
    Render
};

template <class Reply>
static RenderRequest<Reply>
acceptedUploadOrFailure(UploadSubmitResult<Reply> submitted)
{
    if (!submitted)
        return RenderRequestFactory<Reply>::makeImmediateFailure({});
    return std::move(submitted.value());
}

int
main()
{
    std::cout << "=== Skeletal Mesh Walk Test ===\n";

    std::vector<lux::rdesc::Vertex> ground_v, box_v;
    std::vector<uint32_t> ground_i, box_i;
    buildGroundPlane(ground_v, ground_i);
    buildBendyBox(box_v, box_i);

    auto ground_mesh = lux::rdesc::Mesh{
        std::move(ground_v),
        std::move(ground_i),
        lux::math::AABB(Eigen::Vector3f(-6, 0, -6), Eigen::Vector3f(6, 0, 6))};
    auto box_mesh = lux::rdesc::Mesh{
        std::move(box_v),
        std::move(box_i),
        lux::math::AABB(Eigen::Vector3f(-1, 0, -1), Eigen::Vector3f(1, kBoxHeight, 1))};

    // Materials are GRAPH materials now (rdesc::Material retired). Author one graph
    // per distinct colour and compile its FORWARD frag to SPIR-V up front (CPU only).
    //
    // Lit (PBR) ground so it RECEIVES the skinned column's animated shadow — Unlit
    // doesn't sample the shadow atlas, so the ground must stay PBR (roughness 0.85).
    // The orange box is Unlit.
    auto ground_cb = lux::mgtest::compileGraphPass(
        lux::mgtest::makeColorGraph(
            0.25f,
            0.5f,
            0.25f,
            lux::rdesc::EMaterialShadingModel::PbrMetallicRoughness,
            /*metallic=*/0.0f,
            /*roughness=*/0.85f
        ),
        lux::rdesc::EMaterialPass::Forward
    );
    if (!ground_cb)
    {
        std::cerr << "ground graph -> SPIR-V failed: " << ground_cb.error() << "\n";
        return 1;
    }
    auto box_cb = lux::mgtest::compileGraphPass(
        lux::mgtest::makeColorGraph(0.9f, 0.5f, 0.15f, lux::rdesc::EMaterialShadingModel::Unlit),
        lux::rdesc::EMaterialPass::Forward
    );
    if (!box_cb)
    {
        std::cerr << "box graph -> SPIR-V failed: " << box_cb.error() << "\n";
        return 1;
    }
    std::vector<uint32_t> ground_spv = std::move(ground_cb->spirv);
    std::vector<uint32_t> box_spv = std::move(box_cb->spirv);
    std::vector<std::byte> ground_info = std::move(ground_cb->info_bytes);
    std::vector<std::byte> box_info = std::move(box_cb->info_bytes);

    const lux::rdesc::Skeleton skeleton = buildTwoBoneSkeleton();

    Eigen::Vector3f eye(8.f, 5.f, 8.f), target(0.f, 2.5f, 0.f), up(0.f, 1.f, 0.f);
    Eigen::Matrix4f V = buildViewMatrix(eye, target, up);
    Eigen::Matrix4f P = buildProjMatrix(60.f * 3.14159265f / 180.f, 1280.f / 720.f, 0.1f, 100.f);

    auto channel = RenderProgramChannel<>::create();
    auto control_channel = RenderControlChannel<>::create();
    auto upload_channel = RenderUploadChannel<>::create();
    auto sync = std::make_shared<RenderChannelSync>();
    auto surface_exts = getVulkanExtensions();
    lux::window::LuxWindow window(1280, 720, "Skeletal Mesh Walk Test");

    std::atomic<bool> server_ready{false}, server_failed{false};
    std::thread server_thread([&] {
        GeneralRenderServer server(channel, control_channel, upload_channel, sync);
        ServerConfig cfg;
        cfg.instance_extensions = surface_exts;
        if (auto r = server.init(std::move(cfg)); !r)
        {
            std::cerr << "[Server] init failed: " << formatRenderError(renderErrorRegistry(), r.error()) << "\n";
            server_failed.store(true);
            server_ready.store(true);
            return;
        }
        if (auto r = server.attachToWindow(window); !r)
        {
            std::cerr << "[Server] attach failed: " << formatRenderError(renderErrorRegistry(), r.error()) << "\n";
            server_failed.store(true);
            server_ready.store(true);
            return;
        }
        server_ready.store(true);
        while (server.tick())
        {
        }
    }
    );

    while (!server_ready.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (server_failed.load())
    {
        std::cerr << "Server init failed (no Vulkan?). Skipping.\n";
        sync->requestStop();
        server_thread.join();
        return 0;
    }

    RenderProgramSession session(channel, sync);
    RenderControlSession control(control_channel, sync);
    RenderUploadSession upload(upload_channel, sync);
    lux::render::testing::DirectRenderUploadClient upload_client{upload};

    Phase phase = Phase::CreateScene;
    RenderRequest<SceneCreatedReply> scene_req;
    RenderRequest<GenericOkReply> active_req;
    RenderRequest<ViewCreatedReply> view_req;
    RenderRequest<FeatureTypeRegisteredReply> fwd_reg, shadow_reg, mesh_shadow_reg, skin_reg, light_reg, material_reg,
        mesh_stack_reg, view_cam_reg;
    RenderRequest<FeatureAddedReply> fwd_feat, shadow_feat, mesh_shadow_feat, skin_feat, light_feat, material_feat,
        mesh_stack_feat, view_cam_feat;
    SkinningOperationIds skin_ops{};        // feature-scoped bone-upload op-ids (SkinningProxy)
    ViewCameraOperationIds view_cam_ops{};  // feature-scoped per-view camera op-ids (ViewCameraProxy)
    LightOperationIds light_ops{};          // feature-scoped light CRUD op-ids (LightProxy)
    MeshStackOperationIds mesh_stack_ops{}; // feature-scoped instance-CRUD op-ids (MeshStackProxy)
    MaterialOperationIds material_ops{};    // feature-scoped material-upload op-ids (MaterialProxy)
    RenderRequest<ShaderCompiledReply> ground_shader_req, box_shader_req;
    RenderRequest<MaterialUploadedReply> ground_mat_req, box_mat_req;
    RenderRequest<MeshUploadedReply> ground_mesh_req, box_mesh_req;
    RenderRequest<MeshInstanceSlotReply> ground_inst_req, box_inst_req;

    RMeshHandle box_mesh_handle{};
    RenderObjectHandle box_object{};
    auto t0 = std::chrono::steady_clock::now();
    uint64_t frame = 0;

    while (!window.shouldClose())
    {
        window.pollEvents();
        session.beginFrame();
        switch (phase)
        {
        case Phase::CreateScene:
            scene_req = control.createScene("SkelScene");
            break;
        case Phase::ActivateScene:
            active_req = control.setActiveScene(scene_req.tryResult()->get().scene_id, true);
            view_req = control.addView(scene_req.tryResult()->get().scene_id, {1280, 720}, "MainView");
            break;
        case Phase::BindSwapchain:
            control.bindSwapchain(scene_req.tryResult()->get().scene_id, view_req.tryResult()->get().view);
            break;
        case Phase::RegisterFeatures:
            // StandardViewCamera owns the per-view camera matrices (frustum +
            // view/proj). It MUST attach before every camera consumer (Forward /
            // ShadowMap / MeshShadow), so register it up front.
            view_cam_reg = control.registerFeatureType(kViewCameraFeatureFactory);
            fwd_reg = control.registerFeatureType(kForwardMeshFeatureFactory);
            // Light is registered with the other features; it MUST attach before
            // ShadowMapFeature (which caches a raw LightResources* at attach).
            light_reg = control.registerFeatureType(kLightFeatureFactory);
            // StandardMaterialFeature owns the global material stack; it MUST attach
            // before StandardMeshStack (addMeshInstance reads the material slot) and
            // every material consumer. Register it ahead of mesh-stack.
            material_reg = control.registerFeatureType(kMaterialFeatureFactory);
            // StandardMeshStackFeature owns the standard 3D mesh-stack scene
            // resources (no longer built in RenderScene's ctor); it MUST attach
            // before every mesh consumer (Skinning / ShadowMap / MeshShadow /
            // ForwardMesh) or the meshes don't render. No comm config.
            mesh_stack_reg = control.registerFeatureType(kMeshStackFeatureFactory);
            shadow_reg = control.registerFeatureType(kShadowMapFeatureFactory);
            mesh_shadow_reg = control.registerFeatureType(kMeshShadowFeatureFactory);
            skin_reg = control.registerFeatureType(kSkinningFeatureFactory);
            break;
        case Phase::AddShadowFeatures: {
            // StandardViewCamera MUST attach before every camera consumer
            // (Forward / ShadowMap / MeshShadow), so add it first. No comm
            // config — empty struct param.
            lux::render::ViewCameraCommTag view_cam_cfg{};
            view_cam_feat = control.addFeature(
                scene_req.tryResult()->get().scene_id,
                view_cam_reg.tryResult()->get().feature_type_id,
                view_cam_cfg
            );
            // Light MUST attach before ShadowMapFeature (shadow caches a raw
            // LightResources* at attach time).
            lux::render::LightCommTag light_cfg{};
            light_feat = control.addFeature(
                scene_req.tryResult()->get().scene_id,
                light_reg.tryResult()->get().feature_type_id,
                light_cfg
            );
            // StandardMaterialFeature owns the global material stack
            // (builtin shading models + MaterialResources). It MUST attach before
            // StandardMeshStack (addMeshInstance reads the material slot) and every
            // material consumer. No comm config — empty struct param.
            lux::render::MaterialCommTag material_cfg{};
            material_feat = control.addFeature(
                scene_req.tryResult()->get().scene_id,
                material_reg.tryResult()->get().feature_type_id,
                material_cfg
            );
            // StandardMeshStackFeature MUST attach before every mesh consumer
            // (Skinning / ShadowMap / MeshShadow / ForwardMesh) or meshes don't
            // render. No comm config — empty struct param.
            lux::render::MeshStackCommTag mesh_stack_cfg{};
            mesh_stack_feat = control.addFeature(
                scene_req.tryResult()->get().scene_id,
                mesh_stack_reg.tryResult()->get().feature_type_id,
                mesh_stack_cfg
            );
            ShadowMapCommConfig smc{};
            shadow_feat = control.addFeature(
                scene_req.tryResult()->get().scene_id,
                shadow_reg.tryResult()->get().feature_type_id,
                smc
            );
            MeshShadowCommConfig msmc{};
            mesh_shadow_feat = control.addFeature(
                scene_req.tryResult()->get().scene_id,
                mesh_shadow_reg.tryResult()->get().feature_type_id,
                msmc
            );
            break;
        }
        case Phase::AddForwardFeature: {
            ForwardMeshCommConfig cc{};
            cc.comm_config_version = kForwardMeshCommConfigVersion;
            cc.descriptor_layout_version = kForwardMeshDescriptorLayoutVersion;
            fwd_feat = control.addFeature(
                scene_req.tryResult()->get().scene_id,
                fwd_reg.tryResult()->get().feature_type_id,
                cc
            );
            break;
        }
        case Phase::AddSkinningFeature: {
            // SkinningFeature has no comm config — empty struct param.
            lux::render::SkinningCommConfig e{};
            skin_feat = control.addFeature(
                scene_req.tryResult()->get().scene_id,
                skin_reg.tryResult()->get().feature_type_id,
                e
            );
            // Skinning is feature-scoped now — read its dynamic bone-upload op-ids
            // from the registration reply (core protocol no longer names skinning).
            skin_ops =
                SkinningOperationIds::fromOps(skin_reg.tryResult()->get().ops, skin_reg.tryResult()->get().op_count);
            // Light + mesh-stack op-ids: capture HERE (their RegisterFeatures
            // replies are long ready by this phase), NOT in RegisterFeatures
            // itself — there result() is read before the frame is submitted, so
            // the reply hasn't arrived and the ops would be kInvalidTypeId.
            light_ops =
                LightOperationIds::fromOps(light_reg.tryResult()->get().ops, light_reg.tryResult()->get().op_count);
            mesh_stack_ops = MeshStackOperationIds::fromOps(
                mesh_stack_reg.tryResult()->get().ops,
                mesh_stack_reg.tryResult()->get().op_count
            );
            material_ops = MaterialOperationIds::fromOps(
                material_reg.tryResult()->get().ops,
                material_reg.tryResult()->get().op_count
            );
            view_cam_ops = ViewCameraOperationIds::fromOps(
                view_cam_reg.tryResult()->get().ops,
                view_cam_reg.tryResult()->get().op_count
            );
            break;
        }
        case Phase::CompileShaders: {
            const auto ground_bytes = std::span<const std::byte>{
                reinterpret_cast<const std::byte*>(ground_spv.data()),
                ground_spv.size() * sizeof(uint32_t)};
            ground_shader_req =
                control.compileShader(ground_bytes, std::span<const std::byte>{ground_info.data(), ground_info.size()});
            const auto box_bytes = std::span<const std::byte>{
                reinterpret_cast<const std::byte*>(box_spv.data()),
                box_spv.size() * sizeof(uint32_t)};
            box_shader_req =
                control.compileShader(box_bytes, std::span<const std::byte>{box_info.data(), box_info.size()});
            break;
        }
        case Phase::UploadMaterials: {
            // Forward scene: graph frag goes in the FORWARD arg, gbuffer arg empty.
            GraphMaterialData ground_gd{};
            ground_mat_req = acceptedUploadOrFailure(uploadGraphMaterial(
                MaterialUploadClient(upload_client.client(), material_ops),
                ground_gd,
                ShaderHandle{},
                ground_shader_req.tryResult()->get().shader)
            );
            GraphMaterialData box_gd{};
            box_mat_req = acceptedUploadOrFailure(uploadGraphMaterial(
                MaterialUploadClient(upload_client.client(), material_ops),
                box_gd,
                ShaderHandle{},
                box_shader_req.tryResult()->get().shader)
            );
            break;
        }
        case Phase::UploadMeshes:
            ground_mesh_req = acceptedUploadOrFailure(lux::render::uploadMesh(
                lux::render::MeshStackUploadClient(upload_client.client(), mesh_stack_ops),
                ground_mesh)
            );
            box_mesh_req = acceptedUploadOrFailure(lux::render::uploadMesh(
                lux::render::MeshStackUploadClient(upload_client.client(), mesh_stack_ops),
                box_mesh)
            );
            break;
        case Phase::WaitMeshUpload:
            break; // give the async upload a frame
        case Phase::CreateInstances: {
            float gx[16];
            std::memset(gx, 0, sizeof(gx));
            gx[0] = gx[5] = gx[10] = gx[15] = 1.f;
            ground_inst_req = addTransientMeshInstance(
                MeshStackProxy(session, mesh_stack_ops),
                scene_req.tryResult()->get().scene_id,
                ground_mesh_req.tryResult()->get().handle,
                ground_mat_req.tryResult()->get().handle,
                gx
            );
            box_mesh_handle = box_mesh_req.tryResult()->get().handle;
            box_inst_req = addTransientMeshInstance(
                MeshStackProxy(session, mesh_stack_ops),
                scene_req.tryResult()->get().scene_id,
                box_mesh_handle,
                box_mat_req.tryResult()->get().handle,
                gx,
                kInstanceFlagCastShadow | kInstanceFlagReceiveShadow | kInstanceFlagVisible,
                EGeometryKind::SkinnedMesh
            );
            break;
        }
        case Phase::CreateLight: {
            DirectionalLightDesc dl{};
            dl.direction = Eigen::Vector3f(-0.5f, -1.f, -0.3f).normalized();
            dl.color = Eigen::Vector3f(1, 1, 1);
            dl.intensity = 1.f;
            // 回执(灯光句柄)本用例用不到 —— 它只验证灯建得起来、画面亮起来。
            (void)
                lightCreate(LightProxy(session, light_ops), scene_req.tryResult()->get().scene_id, LightDescriptor{dl});
            // Resolve instance handles now that earlier requests are ready.
            const RenderObjectHandle ground_object = ground_inst_req.tryResult()->get().object;
            box_object = box_inst_req.tryResult()->get().object;
            MeshStackProxy(session, mesh_stack_ops)
                .makeInstanceVisibleForView(
                    {.scene_id = scene_req.tryResult()->get().scene_id,
                     .view = view_req.tryResult()->get().view,
                     .object = ground_object}
                );
            MeshStackProxy(session, mesh_stack_ops)
                .makeInstanceVisibleForView(
                    {.scene_id = scene_req.tryResult()->get().scene_id,
                     .view = view_req.tryResult()->get().view,
                     .object = box_object}
                );
            break;
        }
        case Phase::Render: {
            const float time = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();

            // Animate bone 1: oscillating rotation about Z (the box sways).
            const float angle = std::sin(time * 1.5f) * 0.6f;
            lux::rdesc::Pose pose;
            pose.bone_local.resize(2);
            pose.bone_local[0] = Eigen::Affine3f::Identity();
            pose.bone_local[1] = Eigen::Affine3f(Eigen::AngleAxisf(angle, Eigen::Vector3f::UnitZ()));

            std::vector<Eigen::Matrix4f> palette;
            lux::animation::buildSkinningMatrices(pose, skeleton, palette);

            lux::render::UploadBonePalettePayload skin_wire{};
            skin_wire.scene_id = scene_req.tryResult()->get().scene_id;
            skin_wire.object = box_object;
            skin_wire.mesh = box_mesh_handle;
            skin_wire.bone_count = static_cast<uint32_t>(palette.size());
            SkinningProxy(session, skin_ops)
                .uploadBonePalette(
                    skin_wire,
                    std::span<const std::byte>{
                        reinterpret_cast<const std::byte*>(palette.data()),
                        palette.size() * 64u},
                    16u
                );

            viewCameraUpdateTransient(
                ViewCameraProxy(session, view_cam_ops),
                scene_req.tryResult()->get().scene_id,
                view_req.tryResult()->get().view,
                V.data(),
                P.data(),
                eye.data()
            );
            break;
        }
        }
        session.trySubmitFrame();
        session.waitAndPumpReplies();
        control.pumpReplies();
        upload.pumpReplies();

        // Advance the phase machine until Render, then stay there.
        if (phase != Phase::Render)
            phase = static_cast<Phase>(static_cast<int>(phase) + 1);
        ++frame;
    }

    std::cout << "Closed after " << frame << " frames.\n";
    sync->requestStop();
    server_thread.join();
    return 0;
}
