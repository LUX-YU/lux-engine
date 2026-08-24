// ============================================================================
//  textured_material_test.cpp  —  G4: live BINDLESS TEXTURE sampling proof
//
//  Proves the GLSL backend's payoff that the MLIR backend could NOT do: a graph
//  material that SAMPLES A TEXTURE renders correctly through the engine's real
//  deferred pipeline.
//
//  Design: a red/green checker texture is uploaded and referenced via the GRAPH
//  material's own slot table (GraphMaterialData.tex_bindless[0] -> GraphMaterialGPU
//  .tex[0].resource_index, which luxSampleTexture consumes). A real graph --
//  Input(UV0) -> SampleTexture(0) -> SwizzleNode(.rgb) -> BaseColor -- is lowered
//  (lowerToIR) and compiled; the SwizzleNode bridges the texture's vec4 to the vec3
//  attribute. The material carries its OWN frag (R1 per-material PSO, no Config
//  family override) and samples the engine's set-2 bindless array at the per-material
//  index. A readback showing BOTH hues can only happen if the bindless texture was
//  sampled with UV variation -- a constant material could not produce two colors.
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

// Real graph path: Input(UV0) -> SampleTexture(slot 0) -> SwizzleNode(.rgb) -> BaseColor (PBR).
// The SwizzleNode bridges the texture's vec4 'rgba' to the vec3 BaseColor attribute.
static bool buildTexturedGraphFrag(std::vector<uint32_t>& spirv,
                                   std::vector<std::byte>& info_bytes,
                                   std::string&            err)
{
    using namespace lux::rdesc;

    MaterialGraph g;  // shading_model = PbrMetallicRoughness (default)
    g.texture_slots.push_back(TextureSlotDecl{ "base_color" });

    node_id uv = g.addNode(std::make_unique<InputNode>());
    static_cast<InputNode*>(g.node(uv))->input = rdesc::EMaterialInput::UV0;

    node_id smp = g.addNode(std::make_unique<SampleTextureNode>());
    static_cast<SampleTextureNode*>(g.node(smp))->texture_slot = 0;
    g.connect(uv, 0, smp, 0);    // UV0 -> SampleTexture.uv

    node_id sw = g.addNode(std::make_unique<SwizzleNode>());  // default vec4 -> vec3 (.xyz)
    g.connect(smp, 0, sw, 0);    // SampleTexture.rgba -> Swizzle.in

    node_id o = g.addNode(std::make_unique<OutputSurfaceNode>());
    g.connect(sw, 0, o, static_cast<uint32_t>(rdesc::EMaterialAttribute::BaseColor));  // vec3 -> BaseColor

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
    std::cout << "=== textured_material_test (G4) ===\n";

    std::vector<uint32_t> frag_spirv;
    std::vector<std::byte> frag_info;
    std::string gerr;
    if (!buildTexturedGraphFrag(frag_spirv, frag_info, gerr))
    {
        std::cerr << "graph -> SPIR-V failed: " << gerr << "\n";
        return 1;
    }
    std::cout << "[0] textured graph frag generated (" << frag_spirv.size() << " words)\n";

    constexpr uint32_t W = 256, H = 256;

    auto channel = RenderFrameChannel<>::create();
    auto control_channel = RenderControlChannel<>::create();
    auto upload_channel = RenderUploadChannel<>::create();
    auto sync    = std::make_shared<RenderChannelSync>();
    auto exts    = getVulkanExtensions();

    lux::window::LuxWindow window(static_cast<int>(W), static_cast<int>(H), "textured_material_test");

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

    RenderControlSession::CreateSceneConfig scfg{};
    scfg.name             = "GraphTexScene";
    scfg.lit_color_format = lux::rdesc::ETextureFormat::RGBA8_SRGB;  // LDR readback target
    s.beginFrame({});
    const auto scene = await(control, w, control.createScene(scfg));
    s.beginFrame({});
    await(control, w, control.setActiveScene(scene.scene_id, true));
    s.beginFrame({});
    const auto view = await(control, w, control.addView(scene.scene_id, {W, H}, "GraphTexView"));
    s.beginFrame({});
    const auto target = await(control, w, control.createOffscreenRenderTarget({W, H}));
    s.beginFrame({});
    control.setLayer(target.target, 0, scene.scene_id, view.view);
    s.trySubmitFrame();
    s.waitAndPumpReplies();

    // ---- Upload an 8x8 RED/GREEN CHECKER texture ----
    // A checker (vs a single split) guarantees both hues fall within the camera's
    // visible UV range on the sphere, so a correct UV-varying sample shows both.
    constexpr int TW = 64, TH = 64;
    std::vector<std::byte> tex_pixels(static_cast<std::size_t>(TW) * TH * 4);
    for (int y = 0; y < TH; ++y)
        for (int x = 0; x < TW; ++x)
        {
            auto* p = reinterpret_cast<uint8_t*>(&tex_pixels[(static_cast<std::size_t>(y) * TW + x) * 4]);
            const bool red = ((((x * 8) / TW) + ((y * 8) / TH)) & 1) == 0;
            p[0] = red ? 255 : 0;    // R
            p[1] = red ? 0   : 255;  // G
            p[2] = 0;                // B
            p[3] = 255;              // A
        }
    auto texture_submit = upload.tryCreateTexture2D(
        lux::cxx::SharedBytes<>::copyOf(tex_pixels),
        TW, TH, 4, EPixelFormat::RGBA8_SRGB, /*generate_mips=*/false);
    if (!texture_submit)
    {
        std::cerr << "texture upload admission failed\n";
        sync->requestStop();
        server_thread.join();
        return 1;
    }
    const auto tex = await(upload, w, std::move(*texture_submit));
    std::cout << "[2] texture uploaded (status=" << tex.status
              << ", bindless index=" << tex.handle.index << ")\n";

    // ---- Compile + register the textured graph frag ----
    const auto frag_bytes = std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(frag_spirv.data()),
        frag_spirv.size() * sizeof(uint32_t)};
    s.beginFrame({});
    const auto compiled = await(control, w, control.compileShader(
        frag_bytes, std::span<const std::byte>{frag_info.data(), frag_info.size()}));
    std::cout << "[3] compileShader(textured graph frag) status=" << compiled.status << "\n";

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

    s.beginFrame({});
    const auto material_reg = await(control, w, control.registerFeatureType(lux::render::kMaterialFeatureFactory));
    MaterialOperationIds material_ops = MaterialOperationIds::fromOps(material_reg.ops, material_reg.op_count);
    lux::render::MaterialCommTag material_cfg{};
    s.beginFrame({});
    await(control, w, control.addFeature(scene.scene_id, material_reg.feature_type_id, material_cfg));

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

    // NO Config family override: R1 routes the material's own frag via its variant
    // bucket (uploadGraphMaterial below).
    DeferredGBufferCommConfig gbcc{};
    gbcc.comm_config_version       = kDeferredGBufferCommConfigVersion;
    gbcc.descriptor_layout_version = kDeferredGBufferDescriptorLayoutVersion;
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
    std::cout << "[4] deferred features added (no Config override; graph material carries its frag)\n";

    // Graph material carrying the texture's bindless index in its OWN slot table.
    // GraphMaterialData.tex_bindless[0] -> GraphMaterialGPU.tex[0].resource_index is
    // exactly what the emitted graph frag samples through luxSampleTexture. Carries its own frag
    // -> R1 per-material PSO.
    GraphMaterialData gmat{};
    gmat.tex_bindless[0] = tex.handle.index;
    gmat.tex_mask        = 1u;
    s.beginFrame({});
    const auto mat = await(upload, w, uploadGraphMaterial(MaterialUploadClient(upload_client.client(), material_ops), gmat, compiled.shader, ShaderHandle{}));

    const rdesc::Mesh sphere = buildSphereMesh(0.5f, 64, 32);
    s.beginFrame({});
    const auto mesh = await(upload, w, lux::render::uploadMesh(lux::render::MeshStackUploadClient(upload_client.client(), mesh_stack_ops), sphere));
    std::cout << "[5] graph material(status=" << mat.status << ") + mesh(status=" << mesh.status << ")\n";

    float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    s.beginFrame({});
    const auto inst = await(s, w, addTransientMeshInstance(MeshStackProxy(s, mesh_stack_ops), scene.scene_id, mesh.handle, mat.handle, identity));
    s.beginFrame({});
    MeshStackProxy(s, mesh_stack_ops).makeInstanceVisibleForView({.scene_id = scene.scene_id, .view = view.view, .object = inst.object});

    DirectionalLightDesc dl{};
    dl.direction = Eigen::Vector3f(-0.3f, -0.4f, -0.85f).normalized();
    dl.color     = Eigen::Vector3f(1.f, 1.f, 1.f);
    dl.intensity = 3.0f;
    dl.flags     = LIGHT_FLAG_CAST_SHADOW;
    await(s, w, lightCreate(LightProxy(s, light_ops), scene.scene_id, LightDescriptor{dl}));

    const Eigen::Vector3f center(0.f, 0.f, 0.f);
    const float radius = 0.5f * std::sqrt(3.f);
    const float fov    = 45.f * 3.14159265f / 180.f;
    const float dist   = (radius / std::sin(fov * 0.5f)) * 1.25f;
    const Eigen::Vector3f eye = center + Eigen::Vector3f(0.0f, 0.2f, 1.0f).normalized() * dist;
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
    std::cout << "[6] rendered warm-up frames\n";

    std::vector<std::uint8_t> px(static_cast<std::size_t>(W) * H * 4, 0);
    s.beginFrame({});
    const auto rb = await(control, w, control.readbackTarget(target.target, px.data(), px.size()));
    std::cout << "[7] readback status=" << rb.status << " " << rb.width << "x" << rb.height << "\n";

    // Pixels are BGRA8. Count red-dominant + green-dominant lit pixels.
    std::size_t bright = 0, red_dom = 0, green_dom = 0;
    for (std::size_t i = 0; i + 4 <= px.size(); i += 4)
    {
        const int b = px[i], g = px[i+1], r = px[i+2];
        const int luma = (114 * b + 587 * g + 299 * r) / 1000;
        if (luma <= 40) continue;
        ++bright;
        if (r > g + 30 && r > b + 30) ++red_dom;
        if (g > r + 30 && g > b + 30) ++green_dom;
    }
    std::cout << "  bright=" << bright << "  red_dominant=" << red_dom
              << "  green_dominant=" << green_dom << "\n";

    int fails = 0;
    auto check = [&](bool cond, const char* name)
    { std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << "\n"; if (!cond) ++fails; };

    check(compiled.status == 0,             "textured graph frag compiled to a server ShaderHandle");
    check(rb.status == 0,                    "readback status == 0");
    check(rb.width == W && rb.height == H,   "dimensions match the view");
    check(bright > 300,                      "textured sphere is visible");
    check(red_dom > 100,                     "texture RED region sampled (left half)");
    check(green_dom > 100,                   "texture GREEN region sampled (right half)");
    check(red_dom > 100 && green_dom > 100,
          "TWO distinct hues => bindless texture sampled with UV variation (graph SampleTexture works)");

    sync->requestStop();
    server_thread.join();

    std::cout << "=== textured_material_test " << (fails == 0 ? "PASSED" : "FAILED")
              << " (fails=" << fails << ") ===\n";
    return fails == 0 ? 0 : 1;
}
