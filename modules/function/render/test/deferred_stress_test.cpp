// ============================================================================
//  deferred_stress_test.cpp
//
//  Deferred rendering stress test:
//    - 400 cubes in a 20×20 grid
//    - 64 orbiting point lights (deferred shading)
//    - 1 directional light shadow (default CSM ON, runtime CSM toggle)
//    - Grid + Skybox features
//    - Uses RenderServer server-side pre-initialization
// ============================================================================

#include <lux/engine/render/comm/client/RenderSession.hpp>
#include "RenderTask.hpp"            // relocated test-only coroutine support
#include "RenderTaskScheduler.hpp"
#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/render/comm/RenderProtocol.hpp>

#include <lux/engine/render/renderer/features/deffer/DeferredGBufferOperation.hpp>
#include <lux/engine/render/renderer/features/deffer/DeferredLightingOperation.hpp>
#include <lux/engine/render/renderer/features/shadow/ShadowMapOperation.hpp>
#include <lux/engine/render/renderer/features/shadow/ShadowQualityParams.hpp>
#include <lux/engine/render/renderer/features/FeatureParamsOperation.hpp>   // FeatureParamsProxy
#include <lux/engine/render/renderer/features/shadow/MeshShadowOperation.hpp>
#include <lux/engine/render/renderer/features/sky_box/SkyboxOperation.hpp>
#include <lux/engine/render/renderer/features/grid/GridOperation.hpp>
// LightFeature now owns scene light DATA; deferred/shadow CONSUME it. Brings in
// LightDescriptor / DirectionalLightDesc / PointLightDesc / UpdateLightPayload /
// LightCreatedReply / toUpdateLightPayload / LightProxy / LightOperationIds /
// kLightFeatureFactory.
#include <lux/engine/render/renderer/features/light/LightOperation.hpp>
#include <lux/engine/render/renderer/features/material/MaterialOperation.hpp> // kStandardMaterialFeatureFactory
#include <lux/engine/render/renderer/features/meshstack/MeshStackOperation.hpp> // kStandardMeshStackFeatureFactory
#include <lux/engine/render/renderer/features/view_camera/ViewCameraOperation.hpp> // kStandardViewCameraFeatureFactory / ViewCameraProxy

#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/description/Vertex.hpp>
#include <lux/engine/render/resources/material/GraphMaterialData.hpp>

#include "graph_test_helpers.hpp"

#include <lux/engine/asset/AssetManager.hpp>
#include <lux/engine/asset/TextureSerDeser.hpp>
#include <lux/engine/asset/TextureAsset.hpp>

#include <lux/engine/window/LuxWindow.hpp>
#include <GLFW/glfw3.h>

#include <test_time_asset_path.hpp>

#include <Eigen/Geometry>   // Vector3f::cross (buildViewMatrix)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace lux::render;
namespace fs = std::filesystem;

// ── Constants ───────────────────────────────────────────────────────────

static constexpr int   kGridSide         = 20;
static constexpr float kCubeSpacing      = 3.0f;
static constexpr float kCubeHalf         = 0.4f;
static constexpr int   kNumPointLights   = 6;
static constexpr float kLightOrbitRadius = 30.0f;
static constexpr float kLightHeight      = 7.0f;
static constexpr float kLightSpeed       = 0.3f;
static constexpr float kCamRadius        = 25.0f;
static constexpr float kCamHeight        = 30.0f;
static constexpr float kCamSpeed         = 0.1f;
static constexpr float kPi               = 3.14159265f;

// ── Single-lane investigation overrides (set in main from argv; read by stressTask) ──
//   --grid N   : override the cube grid side (default kGridSide). --grid 1 => 1 cube.
//   --no-floor : skip the floor instance. `--grid 1 --no-floor` => a single DEFERRED
//                draw-lane, to localize forward-specific vs shared-base behaviour.
//   --evsm     : use the EVSM shadow technique instead of the default PCF.
//                EVSM is bias-free but blur-bound (~22 ms/frame here, see
//                doc/evsm-status.md); PCF is the default for performance.
static int  g_grid_side = kGridSide;
static bool g_no_floor  = false;
static bool g_use_evsm  = false;

// ── Floor plane geometry ────────────────────────────────────────────────

static void buildFloorPlane(std::vector<lux::rdesc::Vertex> &verts,
                            std::vector<uint32_t> &indices,
                            float half_extent = 30.0f)
{
    using V3 = Eigen::Vector3f;
    using V2 = Eigen::Vector2f;

    V3 n(0, 1, 0);
    V3 t(1, 0, 0);
    V3 bt(0, 0, 1);
    float uv_scale = half_extent / 2.0f;

    V3 corners[4] = {
        {-half_extent, 0, -half_extent},
        { half_extent, 0, -half_extent},
        { half_extent, 0,  half_extent},
        {-half_extent, 0,  half_extent}
    };
    V2 uvs[4] = {
        {0, 0}, {uv_scale, 0}, {uv_scale, uv_scale}, {0, uv_scale}
    };

    auto base = static_cast<uint32_t>(verts.size());
    for (int i = 0; i < 4; ++i)
        verts.push_back({corners[i], n, t, uvs[i], bt});
    // Wind CCW so cross(v1-v0, v2-v0) = (0,1,0) — front face when viewed from above.
    indices.insert(indices.end(), {base, base+2, base+1, base, base+3, base+2});
}

// ── Cube geometry ───────────────────────────────────────────────────────

static void buildCube(std::vector<lux::rdesc::Vertex> &verts,
                      std::vector<uint32_t> &indices,
                      float half = 0.5f)
{
    using V3 = Eigen::Vector3f;
    using V2 = Eigen::Vector2f;

    struct FaceInfo { V3 n, t, bt; V3 corners[4]; };
    FaceInfo faces[6] = {
        {{ 0, 0, 1}, { 1, 0, 0}, {0, 1, 0}, {{-half,-half, half},{ half,-half, half},{ half, half, half},{-half, half, half}}},
        {{ 0, 0,-1}, {-1, 0, 0}, {0, 1, 0}, {{ half,-half,-half},{-half,-half,-half},{-half, half,-half},{ half, half,-half}}},
        {{ 1, 0, 0}, { 0, 0,-1}, {0, 1, 0}, {{ half,-half, half},{ half,-half,-half},{ half, half,-half},{ half, half, half}}},
        {{-1, 0, 0}, { 0, 0, 1}, {0, 1, 0}, {{-half,-half,-half},{-half,-half, half},{-half, half, half},{-half, half,-half}}},
        {{ 0, 1, 0}, { 1, 0, 0}, {0, 0, 1}, {{-half, half, half},{ half, half, half},{ half, half,-half},{-half, half,-half}}},
        {{ 0,-1, 0}, { 1, 0, 0}, {0, 0,-1}, {{-half,-half,-half},{ half,-half,-half},{ half,-half, half},{-half,-half, half}}},
    };
    V2 uvs[4] = {{0,0},{1,0},{1,1},{0,1}};

    verts.reserve(24);
    indices.reserve(36);
    for (auto &f : faces) {
        auto base = static_cast<uint32_t>(verts.size());
        for (int i = 0; i < 4; ++i)
            verts.push_back({f.corners[i], f.n, f.t, uvs[i], f.bt});
        indices.insert(indices.end(), {base, base+1, base+2, base, base+2, base+3});
    }
}

// ── Matrix helpers ──────────────────────────────────────────────────────

static void setTranslation(float m[16], float x, float y, float z)
{
    std::memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.f;
    m[12] = x; m[13] = y; m[14] = z;
}

/// Build a column-major 4×4 matrix: rotation around Y then translation.
static void setTransformYRot(float m[16], float x, float y, float z, float angle_rad)
{
    float c = std::cos(angle_rad), s = std::sin(angle_rad);
    std::memset(m, 0, 16 * sizeof(float));
    m[0] = c;   m[2] = -s;
    m[5] = 1.f;
    m[8] = s;   m[10] = c;
    m[15] = 1.f;
    m[12] = x;  m[13] = y;  m[14] = z;
}

/// Convert HSV (h in [0,1], s,v in [0,1]) to RGB.
static Eigen::Vector3f hsvToRgb(float h, float s, float v)
{
    float c = v * s;
    float x = c * (1.f - std::fabs(std::fmod(h * 6.f, 2.f) - 1.f));
    float m = v - c;
    Eigen::Vector3f rgb;
    int hi = static_cast<int>(h * 6.f) % 6;
    switch (hi) {
    case 0: rgb = {c, x, 0}; break;
    case 1: rgb = {x, c, 0}; break;
    case 2: rgb = {0, c, x}; break;
    case 3: rgb = {0, x, c}; break;
    case 4: rgb = {x, 0, c}; break;
    default: rgb = {c, 0, x}; break;
    }
    return rgb + Eigen::Vector3f(m, m, m);
}

static Eigen::Matrix4f buildViewMatrix(const Eigen::Vector3f &eye,
                                       const Eigen::Vector3f &target,
                                       const Eigen::Vector3f &up)
{
    Eigen::Vector3f f = (target - eye).normalized();
    Eigen::Vector3f s = f.cross(up).normalized();
    Eigen::Vector3f u = s.cross(f);
    Eigen::Matrix4f V = Eigen::Matrix4f::Identity();
    V(0,0) = s.x(); V(0,1) = s.y(); V(0,2) = s.z(); V(0,3) = -s.dot(eye);
    V(1,0) = u.x(); V(1,1) = u.y(); V(1,2) = u.z(); V(1,3) = -u.dot(eye);
    V(2,0) = -f.x(); V(2,1) = -f.y(); V(2,2) = -f.z(); V(2,3) = f.dot(eye);
    return V;
}

static Eigen::Matrix4f buildProjMatrix(float fov_rad, float aspect,
                                       float near_z, float far_z)
{
    float tanHalf = std::tan(fov_rad * 0.5f);
    Eigen::Matrix4f P = Eigen::Matrix4f::Zero();
    P(0,0) =  1.f / (aspect * tanHalf);
    P(1,1) = -1.f / tanHalf;
    P(2,2) = -far_z / (far_z - near_z);
    P(2,3) = -(far_z * near_z) / (far_z - near_z);
    P(3,2) = -1.f;
    return P;
}

// ── Vulkan extensions ───────────────────────────────────────────────────

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

// ── Server pre-init result (shared between threads) ─────────────────────

struct ServerInitResult
{
    RenderSceneId scene_id{};
    ViewHandle    view{};
    uint32_t      view_cam_tid{0};
    uint32_t      light_tid{0};
    uint32_t      material_tid{0};
    uint32_t      mesh_stack_tid{0};
    uint32_t      gbuf_tid{0};
    uint32_t      lighting_tid{0};
    uint32_t      shadowmap_tid{0};
    uint32_t      meshshadow_tid{0};
    uint32_t      skybox_tid{0};
    uint32_t      grid_tid{0};
    SkyboxOperationIds    skybox_ops{};
    GridOperationIds      grid_ops{};
    TypeId        shadow_params_op{kInvalidTypeId}; // ShadowMap's reflected setParams op (FeatureParamsProxy)
    ViewCameraOperationIds view_cam_ops{}; // feature-scoped per-view camera update op (ViewCameraProxy)
    LightOperationIds     light_ops{};   // feature-scoped light CRUD ops (LightProxy)
    MeshStackOperationIds mesh_stack_ops{}; // feature-scoped mesh-instance ops (MeshStackProxy)
    MaterialOperationIds  material_ops{}; // feature-scoped material upload ops (MaterialProxy)
};

// ── Coroutine task ──────────────────────────────────────────────────────

static RenderTask<void> stressTask(
    RenderSession &session,
    const fs::path &asset_dir,
    lux::window::LuxWindow &window,
    const ServerInitResult &srv)
{
    // ── 1. Activate scene ───────────────────────────────────────────
    co_await session.setActiveScene(srv.scene_id, true);
    std::cout << "  Scene activated\n";

    // ── 2. Add features ─────────────────────────────────────────────
    co_await yield_frame();

    // StandardViewCameraFeature owns the per-scene 3D camera data (per-view
    // view/proj matrices + frustum). Every camera consumer (shadow / gbuffer /
    // lighting) reads ViewCameraResource, so it MUST be added FIRST — owner-first,
    // like StandardMeshStack.
    struct EmptyViewCamCfg {} view_cam_cfg{};
    auto view_cam_feat = co_await session.addFeature(srv.scene_id, srv.view_cam_tid, view_cam_cfg);
    std::cout << "  Feature: StandardViewCamera — " << (view_cam_feat.feature.valid() ? "OK" : "FAIL") << "\n";

    // LightFeature owns the scene light DATA; ShadowMap/DeferredLighting are its
    // CONSUMERS (find<LightResources>). ShadowMapFeature caches a raw
    // LightResources* at attach time, so Light MUST be added BEFORE shadow.
    struct EmptyLightCfg {} light_cfg{};
    auto light_feat = co_await session.addFeature(srv.scene_id, srv.light_tid, light_cfg);
    std::cout << "  Feature: Light — " << (light_feat.feature.valid() ? "OK" : "FAIL") << "\n";

    // StandardMaterialFeature owns the global material stack (ShadingModelRegistry +
    // MaterialResources). StandardMeshStack's addMeshInstance reads the material slot,
    // and the material consumers bind MaterialResources, so it MUST be added BEFORE
    // StandardMeshStack (and before every material consumer).
    struct EmptyMaterialCfg {} material_cfg{};
    co_await session.addFeature(srv.scene_id, srv.material_tid, material_cfg);

    // StandardMeshStack owns the scene mesh resources (InstanceResources /
    // VertexPoolRegistry); ShadowMap/MeshShadow/DeferredGBuffer CONSUME them at
    // their own attach, so it MUST be added BEFORE those mesh consumers.
    struct EmptyMeshStackCfg {} mesh_stack_cfg{};
    auto mesh_stack_feat = co_await session.addFeature(srv.scene_id, srv.mesh_stack_tid, mesh_stack_cfg);
    std::cout << "  Feature: StandardMeshStack — " << (mesh_stack_feat.feature.valid() ? "OK" : "FAIL") << "\n";

    ShadowMapCommConfig smc{};
    smc.atlas_page_resolution  = 4096;
    smc.atlas_page_count       = 12;
    smc.max_shadow_slices      = 128;
    smc.enable_directional_csm = 1; // default: CSM ON
    // Default to PCF — fast hardware-compare shadows. EVSM (bias-free but
    // blur-bound) is opt-in via --evsm; see doc/evsm-status.md for why PCF is
    // the current default.
    smc.default_technique      = g_use_evsm
        ? lux::render::EShadowTechnique::EVSM
        : lux::render::EShadowTechnique::PCF;
    smc.non_directional_shadow_max_distance = 1200.0f;
    auto shadow_feat = co_await session.addFeature(srv.scene_id, srv.shadowmap_tid, smc);
    std::cout << "  Feature: ShadowMap — " << (shadow_feat.feature.valid() ? "OK" : "FAIL") << "\n";
    std::cout << "  Shadow technique: " << (g_use_evsm ? "EVSM" : "PCF") << "\n";
    std::cout << "  Directional shadow mode: CSM ON (default)\n";

    MeshShadowCommConfig msmc{};
    msmc.comm_config_version = kMeshShadowCommConfigVersion;
    msmc.descriptor_layout_version = kMeshShadowDescriptorLayoutVersion;
    auto mesh_shadow_feat = co_await session.addFeature(srv.scene_id, srv.meshshadow_tid, msmc);
    std::cout << "  Feature: MeshShadow — " << (mesh_shadow_feat.feature.valid() ? "OK" : "FAIL") << "\n";

    DeferredGBufferCommConfig gbcc{};
    gbcc.comm_config_version = kDeferredGBufferCommConfigVersion;
    gbcc.descriptor_layout_version = kDeferredGBufferDescriptorLayoutVersion;
    auto gbuf_feat = co_await session.addFeature(srv.scene_id, srv.gbuf_tid, gbcc);
    std::cout << "  Feature: DeferredGBuffer — " << (gbuf_feat.feature.valid() ? "OK" : "FAIL") << "\n";

    DeferredLightingCommConfig dlcc{};
    dlcc.read_mode       = 0; // SAMPLED
    dlcc.enable_clustered = 1;
    dlcc.cluster_x = 16;
    dlcc.cluster_y = 9;
    dlcc.cluster_z = 24;
    dlcc.max_cluster_indices = 1'048'576;
    // C5: must match ShadowMapCommConfig::default_technique above — picks
    // which deferred_lighting.frag SPIR-V variant to bind (PCF samples D32
    // atlas via binding 5, EVSM samples RGBA16F blurred atlas via binding 9).
    dlcc.technique = g_use_evsm
        ? lux::render::EShadowTechnique::EVSM
        : lux::render::EShadowTechnique::PCF;
    auto lit_feat = co_await session.addFeature(srv.scene_id, srv.lighting_tid, dlcc);
    std::cout << "  Feature: DeferredLighting — " << (lit_feat.feature.valid() ? "OK" : "FAIL") << "\n";

    GridCommConfig gcc{};
    auto grid_feat = co_await session.addFeature(srv.scene_id, srv.grid_tid, gcc);
    std::cout << "  Feature: Grid — " << (grid_feat.feature.valid() ? "OK" : "FAIL") << "\n";

    SkyboxCommConfig skcc{};
    auto sky_feat = co_await session.addFeature(srv.scene_id, srv.skybox_tid, skcc);
    std::cout << "  Feature: Skybox — " << (sky_feat.feature.valid() ? "OK" : "FAIL") << "\n";

    // ── 2b. Upload skybox equirectangular texture ───────────────────
    co_await yield_frame();

    // Load the texture .luxasset (packed by CMake alongside shaders)
    auto tex_mgr = std::make_shared<lux::asset::AssetManager>();
    lux::asset::TextureSerDeser tex_ser(tex_mgr);
    auto tex_res = tex_ser.fromLuxAsset(asset_dir / "textures" / "blue_nebulae_1.luxasset");
    if (!tex_res)
        throw std::runtime_error("Failed to load skybox texture");

    auto* tex_asset = tex_res.value().first->as<lux::asset::TextureAsset>();
    auto& sky_tex   = *tex_asset->data();

    // Fail-fast guard so an unsupported asset format aborts the test loudly
    // rather than rendering a black skybox via the createTexture2D rdesc
    // overload's synchronous known-bad path.
    {
        EPixelFormat probe{};
        if (!lux::render::toPixelFormat(sky_tex.pixelFormat(), probe))
            throw std::runtime_error("Unsupported skybox texture pixel format");
    }

    auto sky_tex_req = [&]() -> RenderRequest<Texture2DCreatedReply>
    {
        constexpr uint32_t kMaxUploadMips = 16u;
        const uint32_t mip_count = std::min<uint32_t>(sky_tex.mipCount(), kMaxUploadMips);
        const auto* texture_bytes = static_cast<const std::byte*>(sky_tex.data());

        if (mip_count > 1u)
        {
            std::array<RenderSession::Texture2DMipLevel, kMaxUploadMips> mip_levels{};
            uint32_t valid_mips = 0;
            for (uint32_t i = 0; i < mip_count; ++i)
            {
                const auto& range = sky_tex.mipRange(i);
                if (range.size == 0)
                    break;
                if (range.offset + range.size > sky_tex.size())
                    throw std::runtime_error("Skybox mip range exceeds texture payload size");

                mip_levels[valid_mips].pixels = texture_bytes + range.offset;
                mip_levels[valid_mips].byte_count = static_cast<uint32_t>(range.size);
                mip_levels[valid_mips].width = range.width;
                mip_levels[valid_mips].height = range.height;
                ++valid_mips;
            }

            if (valid_mips > 1u)
                return session.createTexture2DMips(
                    mip_levels.data(),
                    valid_mips,
                    sky_tex.channel(),
                    sky_tex.pixelFormat(),
                    /*generate_mips=*/false);
        }

        return session.createTexture2D(
            texture_bytes,
            static_cast<uint32_t>(sky_tex.size()),
            sky_tex.width(),
            sky_tex.height(),
            sky_tex.channel(),
            sky_tex.pixelFormat(),
            /*generate_mips=*/!lux::rdesc::isCompressedFormat(sky_tex.pixelFormat()));
    }();

    while (!sky_tex_req.isReady())
        co_await yield_frame();

    auto sky_tex_reply = sky_tex_req.result();
    std::cout << "  Skybox texture uploaded (status=" << sky_tex_reply.status << ")\n";

    // Set the equirectangular texture on the skybox feature
    SkyboxProxy skybox_proxy(session, srv.skybox_ops);
    skybox_proxy.setEquirect(srv.scene_id, sky_feat.feature, sky_tex_reply.handle);

    // ── 3. Upload mesh + material ───────────────────────────────────
    co_await yield_frame();

    std::vector<lux::rdesc::Vertex> cube_verts;
    std::vector<uint32_t> cube_idx;
    buildCube(cube_verts, cube_idx, kCubeHalf);

    auto cube_mesh = lux::rdesc::Mesh{
        std::move(cube_verts), std::move(cube_idx),
        lux::math::AABB(Eigen::Vector3f(-kCubeHalf, -kCubeHalf, -kCubeHalf),
                        Eigen::Vector3f( kCubeHalf,  kCubeHalf,  kCubeHalf))};

    // ── GRAPH materials (rdesc::Material retired) ───────────────────
    // Each distinct baked colour => distinct frag => distinct per-material PSO
    // (R1). Author ONE graph + GBuffer frag per material; compile to SPIR-V on
    // the CPU, compileShader it to a server ShaderHandle, then carry that frag
    // via uploadGraphMaterial(gd, gbuffer=comp.shader, forward=ShaderHandle{}).

    // Floor plane mesh
    std::vector<lux::rdesc::Vertex> floor_verts;
    std::vector<uint32_t> floor_idx;
    constexpr float kFloorHalf = 30.0f;
    buildFloorPlane(floor_verts, floor_idx, kFloorHalf);

    auto floor_mesh = lux::rdesc::Mesh{
        std::move(floor_verts), std::move(floor_idx),
        lux::math::AABB(
            Eigen::Vector3f(-kFloorHalf, -0.01f, -kFloorHalf),
            Eigen::Vector3f( kFloorHalf,  0.01f,  kFloorHalf)
        )
    };

    // Compile the three GBuffer frags (CPU): orange PBR cubes, grey PBR floor,
    // Unlit-emissive light marker.
    auto cube_cb = lux::mgtest::compileGraphPass(
            lux::mgtest::makeColorGraph(
                0.85f, 0.45f, 0.1f,
                lux::rdesc::EMaterialShadingModel::PbrMetallicRoughness,
                /*metallic=*/0.05f, /*roughness=*/0.55f),
            lux::rdesc::EMaterialPass::GBuffer);
    if (!cube_cb)
        throw std::runtime_error("cube graph -> SPIR-V failed: " + cube_cb.error());

    auto floor_cb = lux::mgtest::compileGraphPass(
            lux::mgtest::makeColorGraph(
                0.5f, 0.5f, 0.5f,
                lux::rdesc::EMaterialShadingModel::PbrMetallicRoughness,
                /*metallic=*/0.0f, /*roughness=*/0.9f),
            lux::rdesc::EMaterialPass::GBuffer);
    if (!floor_cb)
        throw std::runtime_error("floor graph -> SPIR-V failed: " + floor_cb.error());

    // Light-marker: Unlit + bright emissive so it glows in the deferred lighting.
    auto light_cb = lux::mgtest::compileGraphPass(
            lux::mgtest::makeColorGraph(
                1.f, 1.f, 1.f,
                lux::rdesc::EMaterialShadingModel::Unlit,
                /*metallic=*/0.0f, /*roughness=*/0.6f,
                /*world_normal_input=*/false, /*emissive_scale=*/3.0f),
            lux::rdesc::EMaterialPass::GBuffer);
    if (!light_cb)
        throw std::runtime_error("light graph -> SPIR-V failed: " + light_cb.error());

    std::vector<uint32_t>  cube_spv  = std::move(cube_cb->spirv);
    std::vector<uint32_t>  floor_spv = std::move(floor_cb->spirv);
    std::vector<uint32_t>  light_spv = std::move(light_cb->spirv);
    std::vector<std::byte> cube_info  = std::move(cube_cb->info_bytes);
    std::vector<std::byte> floor_info = std::move(floor_cb->info_bytes);
    std::vector<std::byte> light_info = std::move(light_cb->info_bytes);

    const auto toBytes = [](const std::vector<uint32_t>& spv)
    {
        return std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(spv.data()),
            spv.size() * sizeof(uint32_t)};
    };

    auto cube_shader_req  = session.compileShader(
        toBytes(cube_spv),  std::span<const std::byte>{cube_info.data(),  cube_info.size()});
    auto floor_shader_req = session.compileShader(
        toBytes(floor_spv), std::span<const std::byte>{floor_info.data(), floor_info.size()});
    auto light_shader_req = session.compileShader(
        toBytes(light_spv), std::span<const std::byte>{light_info.data(), light_info.size()});

    auto mesh_req       = lux::render::MeshStackProxy(session, srv.mesh_stack_ops).uploadMesh(cube_mesh);
    auto floor_mesh_req = lux::render::MeshStackProxy(session, srv.mesh_stack_ops).uploadMesh(floor_mesh);

    // Mesh uploads + shader compiles are deferred — may need multiple frames.
    while (!mesh_req.isReady() || !floor_mesh_req.isReady()
        || !cube_shader_req.isReady() || !floor_shader_req.isReady()
        || !light_shader_req.isReady())
        co_await yield_frame();

    auto mesh_reply       = mesh_req.result();
    auto floor_mesh_reply = floor_mesh_req.result();
    auto cube_shader      = cube_shader_req.result();
    auto floor_shader     = floor_shader_req.result();
    auto light_shader     = light_shader_req.result();

    // Upload one graph material per frag (R1 per-material PSO; GBuffer => the
    // frag is the gbuffer slot, forward slot is empty).
    lux::render::GraphMaterialData cube_gd{};
    lux::render::GraphMaterialData floor_gd{};
    lux::render::GraphMaterialData light_gd{};
    // Material upload is a feature op now (MaterialProxy) — uploadGraphMaterial was
    // removed from RenderSession when materials became a feature.
    MaterialProxy mat_proxy(session, srv.material_ops);
    auto mat_req        = mat_proxy.uploadGraphMaterial(cube_gd,  cube_shader.shader,  lux::render::ShaderHandle{});
    auto floor_mat_req  = mat_proxy.uploadGraphMaterial(floor_gd, floor_shader.shader, lux::render::ShaderHandle{});
    auto light_cube_mat_req = mat_proxy.uploadGraphMaterial(light_gd, light_shader.shader, lux::render::ShaderHandle{});

    while (!mat_req.isReady() || !floor_mat_req.isReady()
        || !light_cube_mat_req.isReady())
        co_await yield_frame();

    auto mat_reply        = mat_req.result();
    auto floor_mat_reply  = floor_mat_req.result();
    auto light_cube_mat_reply = light_cube_mat_req.result();

    std::cout << "  Mesh + material uploaded (mesh=" << mesh_reply.status
              << ", mat=" << mat_reply.status << ")\n";
    std::cout << "  Floor uploaded (mesh=" << floor_mesh_reply.status
              << ", mat=" << floor_mat_reply.status << ")\n";

    // ── 4. Create cube instances (400) ──────────────────────────────
    co_await yield_frame();

    // Mesh-instance CRUD now goes through the feature-scoped MeshStackProxy
    // (addMeshInstance / makeInstanceVisibleForView / updateTransforms were
    // removed from RenderSession when the mesh stack became a feature).
    MeshStackProxy mesh_proxy(session, srv.mesh_stack_ops);

    const float center = (g_grid_side - 1) * kCubeSpacing * 0.5f;
    constexpr float kModelLift = 2.0f;  // raise everything up

    struct CubeInfo { RenderObjectHandle object; float base_x, base_z; int ix, iz; };
    std::vector<CubeInfo> cube_infos;
    cube_infos.reserve(g_grid_side * g_grid_side);

    for (int z = 0; z < g_grid_side; ++z)
    {
        for (int x = 0; x < g_grid_side; ++x)
        {
            float px = x * kCubeSpacing - center;
            float pz = z * kCubeSpacing - center;
            float xform[16];
            setTranslation(xform, px, kCubeHalf + kModelLift, pz);
            auto req = mesh_proxy.addMeshInstance(
                srv.scene_id,
                mesh_reply.handle, mat_reply.handle, xform);
            auto reply = co_await req;
            cube_infos.push_back({reply.object, px, pz, x, z});
            mesh_proxy.makeInstanceVisibleForView(srv.scene_id, srv.view, reply.object);
        }
    }

    // Floor instance (also lifted) — --no-floor drops this second draw-lane
    if (!g_no_floor)
    {
        float xform[16];
        setTranslation(xform, 0.f, kModelLift, 0.f);
        auto floor_inst = mesh_proxy.addMeshInstance(
            srv.scene_id,
            floor_mesh_reply.handle, floor_mat_reply.handle, xform);
        auto floor_reply = co_await floor_inst;
        mesh_proxy.makeInstanceVisibleForView(srv.scene_id, srv.view, floor_reply.object);
    }

    co_await yield_frame();
    std::cout << "  Cubes created: " << (g_grid_side * g_grid_side)
              << (g_no_floor ? " (no floor)" : " + 1 floor") << "\n";

    // ── 5. Create lights ────────────────────────────────────────────
    // Directional light (shadow caster)
    DirectionalLightDesc dl{};
    {
        float dx = -0.5f, dy = -1.f, dz = -0.3f;
        float len = std::sqrt(dx*dx + dy*dy + dz*dz);
        dl.direction = Eigen::Vector3f(dx/len, dy/len, dz/len);
    }
    dl.color              = Eigen::Vector3f(1.f, 0.95f, 0.8f);
    dl.intensity          = 1.5f;
    dl.flags              = 1;   // LF_CAST_SHADOW
    // Directional CSM covers a large world area per cascade, so its texel
    // density (texels-per-world-unit) is far lower than a point/spot light's
    // focused cube face — that, not an EVSM bug, is why the directional shadow
    // looks softer. Two density levers: (1) a higher per-cascade resolution,
    // (2) tighter cascade splits so the orbit-camera's mid-field geometry lands
    // in a smaller (denser) cascade instead of the wide 30–70 band.
    dl.shadow_map_size    = 4096;
    dl.shadow_bias        = 0.002f;
    // dl.shadow_normal_bias = 0.05f;  // L4: dead field; rasterizer slope-scale + receiver-plane bias replace it.
    dl.cascade_count      = 4;
    dl.cascade_splits     = {12.f, 30.f, 60.f, 120.f};

    // Light CRUD now goes through the feature-scoped LightProxy (createLight /
    // updateLights were removed from RenderSession when light became a feature).
    LightProxy light_proxy(session, srv.light_ops);

    auto dir_light_req = light_proxy.createLight(srv.scene_id, LightDescriptor{dl});

    std::vector<RenderRequest<LightCreatedReply>> pl_reqs;
    pl_reqs.reserve(kNumPointLights);

    for (int i = 0; i < kNumPointLights; ++i)
    {
        float angle  = 2.f * kPi * i / kNumPointLights;
        int   ring   = i % 3;  // 0 = inner, 1 = mid, 2 = outer
        float radius = kLightOrbitRadius * (0.3f + 0.35f * ring);
        float height = kLightHeight /* * (0.5f + 0.4f * ring)*/;

        PointLightDesc pl{};
        pl.position = Eigen::Vector3f(
            radius * std::cos(angle), height, radius * std::sin(angle));

        // Each light gets a unique hue
        float hue = std::fmod(static_cast<float>(i) / kNumPointLights + ring * 0.17f, 1.f);
        pl.color     = hsvToRgb(hue, 0.85f, 1.0f);
        pl.intensity = 10.0f;
        pl.range     = 100.0f;
        // All point lights are enabled as shadow candidates (Top-K budget selects casters).
        pl.flags            = 1u;
        pl.shadow_map_size  = 2048;
        pl.shadow_bias      = 0.002f;
        pl.shadow_normal_bias = 0.01f;

        pl_reqs.push_back(light_proxy.createLight(srv.scene_id, LightDescriptor{pl}));
    }

    co_await yield_frame();

    auto dir_reply = co_await dir_light_req;
    std::cout << "  Directional light: " << (dir_reply.status == 0 ? "OK" : "FAIL") << "\n";

    std::vector<RLightHandle> light_handles;
    light_handles.reserve(kNumPointLights);
    for (auto &req : pl_reqs)
    {
        auto reply = co_await req;
        light_handles.push_back(reply.handle);
    }
    std::cout << "  Point lights created: " << light_handles.size() << "\n";

    // ── 6b. Visible light cubes (Unlit + emissive) ──────────────────
    constexpr float kLightCubeHalf = 0.25f;

    std::vector<lux::rdesc::Vertex> lc_verts;
    std::vector<uint32_t> lc_idx;
    buildCube(lc_verts, lc_idx, kLightCubeHalf);
    auto lc_mesh = lux::rdesc::Mesh{
        std::move(lc_verts), std::move(lc_idx),
        lux::math::AABB(Eigen::Vector3f(-kLightCubeHalf, -kLightCubeHalf, -kLightCubeHalf),
                        Eigen::Vector3f( kLightCubeHalf,  kLightCubeHalf,  kLightCubeHalf))};
    auto lc_mesh_req = lux::render::MeshStackProxy(session, srv.mesh_stack_ops).uploadMesh(lc_mesh);
    while (!lc_mesh_req.isReady())
        co_await yield_frame();
    auto lc_mesh_reply = lc_mesh_req.result();

    std::vector<RenderObjectHandle> light_cube_slots;
    light_cube_slots.reserve(kNumPointLights);
    for (int i = 0; i < kNumPointLights; ++i)
    {
        // Place at initial light position
        int   ring   = i % 3;
        float radius = kLightOrbitRadius * (0.3f + 0.35f * ring);
        float height = kLightHeight;
        float angle  = 2.f * kPi * i / kNumPointLights;

        float xform[16];
        setTranslation(xform,
            radius * std::cos(angle), height, radius * std::sin(angle));
        constexpr uint32_t kLightCubeFlags = kInstanceFlagReceiveShadow | kInstanceFlagVisible;
        auto req = mesh_proxy.addMeshInstance(
            srv.scene_id,
            lc_mesh_reply.handle, light_cube_mat_reply.handle, xform, kLightCubeFlags);
        auto reply = co_await req;
        light_cube_slots.push_back(reply.object);
        mesh_proxy.makeInstanceVisibleForView(srv.scene_id, srv.view, reply.object);
    }
    std::cout << "  Light cubes created: " << light_cube_slots.size() << "\n";

    // ── 6. Render loop ──────────────────────────────────────────────

    auto start = std::chrono::steady_clock::now();
    auto prev  = start;
    Eigen::Vector3f target(0.f, 1.f + 2.f, 0.f);  // look at lifted scene
    Eigen::Vector3f up(0.f, 1.f, 0.f);
    int prev_w = 1280, prev_h = 720;
    Eigen::Matrix4f P = buildProjMatrix(60.f * kPi / 180.f,
        static_cast<float>(prev_w) / static_cast<float>(prev_h), 0.1f, 200.f);

    uint64_t frame_count = 0;
    double   fps_accum   = 0.0;
    auto     fps_timer   = std::chrono::steady_clock::now();
    bool directional_csm_enabled = true;
    bool csm_toggle_key_down_prev = false;
    // Persistent reflected snapshot pushed via FeatureParamsProxy: presets and the
    // CSM toggle both edit this and re-push the WHOLE struct (the param seam is a
    // full-snapshot model, not the old "0 = keep current" partial payloads).
    ShadowQualityParams shadow_params{};

    std::cout << "\n  === Rendering — ESC exit | 1-4 quality | 5 toggle CSM ===\n\n";

    while (!window.shouldClose())
    {
        auto now = std::chrono::steady_clock::now();
        float elapsed = std::chrono::duration<float>(now - start).count();
        double dt = std::chrono::duration<double>(now - prev).count();
        prev = now;
        dt = std::min(dt, 0.1);

        // ── Dynamic aspect ratio ──────────────────────────────────
        {
            auto ws = window.size();
            if (ws.width > 0 && ws.height > 0
                && (ws.width != prev_w || ws.height != prev_h))
            {
                prev_w = ws.width;
                prev_h = ws.height;
                P = buildProjMatrix(60.f * kPi / 180.f,
                    static_cast<float>(prev_w) / static_cast<float>(prev_h),
                    0.1f, 200.f
                );
            }
        }

        // Orbiting camera
        float cam_angle = elapsed * kCamSpeed;
        Eigen::Vector3f eye(
            kCamRadius * std::cos(cam_angle),
            kCamHeight,
            kCamRadius * std::sin(cam_angle));
        Eigen::Matrix4f V = buildViewMatrix(eye, target, up);

        ViewCameraProxy(session, srv.view_cam_ops).update(srv.scene_id, srv.view, V.data(), P.data(), eye.data());

        // ── Animate cubes (batched) ───────────────────────────────────
        std::vector<TransformWriteEntry> xform_batch;
        xform_batch.resize(cube_infos.size());
        for (size_t idx = 0; idx < cube_infos.size(); ++idx)
        {
            auto& ci = cube_infos[idx];
            float phase = 0.5f * (ci.base_x + ci.base_z) + elapsed * 2.f;
            float bob   = 0.15f * std::sin(phase);
            float rot   = elapsed * (0.8f + 0.3f * std::sin(ci.ix * 0.7f + ci.iz * 1.1f));

            float xform[16];
            setTransformYRot(xform, ci.base_x,
                             kCubeHalf + kModelLift + bob,
                             ci.base_z, rot);
            xform_batch[idx].object = ci.object;
            std::memcpy(xform_batch[idx].transform, xform, sizeof(xform));
        }
        mesh_proxy.updateTransforms(xform_batch);

        // ── Update point light positions (orbit, distinct colours) ───
        std::vector<UpdateLightPayload> light_batch;
        light_batch.reserve(kNumPointLights);
        std::vector<TransformWriteEntry> lc_xform_batch;
        lc_xform_batch.reserve(kNumPointLights);

        for (int i = 0; i < kNumPointLights; ++i)
        {
            int   ring   = i % 3;
            float radius = kLightOrbitRadius * (0.3f + 0.35f * ring);
            float height = kLightHeight/* * (0.5f + 0.4f * ring) + kModelLift*/;
            float speed  = kLightSpeed /* * (1.0f + 0.25f * ring)*/;

            float base_angle = 2.f * kPi * i / kNumPointLights;
            float angle = base_angle + elapsed * speed;

            PointLightDesc pl{};
            pl.position = Eigen::Vector3f(
                radius * std::cos(angle), height, radius * std::sin(angle));

            float hue = std::fmod(static_cast<float>(i) / kNumPointLights + ring * 0.17f, 1.f);
            pl.color     = hsvToRgb(hue, 0.85f, 1.0f);
            pl.intensity = 10.0f;
            pl.range     = 60.0f;
            // Keep all point lights enabled; shadow selection remains Top-K.
            pl.flags            = 1u;
            pl.shadow_map_size  = 2048;
            pl.shadow_bias      = 0.002f;
            pl.shadow_normal_bias = 0.01f;

            light_batch.push_back(toUpdateLightPayload(srv.scene_id, light_handles[i], LightDescriptor{pl}));

            // Collect light-cube transform for batched update
            TransformWriteEntry lc_tw{};
            lc_tw.object = light_cube_slots[i];
            float lc_xform[16];
            setTranslation(lc_xform, pl.position.x(), pl.position.y(), pl.position.z());
            std::memcpy(lc_tw.transform, lc_xform, sizeof(lc_xform));
            lc_xform_batch.push_back(lc_tw);
        }
        light_proxy.updateLights(light_batch);
        mesh_proxy.updateTransforms(lc_xform_batch);

        co_await yield_frame();

        // FPS counter (every 2 seconds)
        ++frame_count;
        fps_accum += dt;
        double fps_elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - fps_timer).count();
        if (fps_elapsed >= 2.0)
        {
            double avg_fps = frame_count / fps_accum;
            std::cout << "  FPS: " << static_cast<int>(avg_fps)
                      << "  (frames=" << frame_count << ")\n";
            frame_count = 0;
            fps_accum   = 0.0;
            fps_timer   = std::chrono::steady_clock::now();
        }

        // ESC to exit
        if (glfwGetKey(window.handle(), GLFW_KEY_ESCAPE) == GLFW_PRESS)
            break;

        // ── Shadow quality presets (keys 1-4) ────────────────────────
        // 1 = Low (512, 16, 20m)  2 = Medium (1024, 32, 40m)
        // 3 = High (2048, 48, 60m)  4 = Ultra (4096, 64, 100m)
        if (srv.shadow_params_op != kInvalidTypeId)
        {
            struct QualityPreset
            {
                int key;
                uint32_t res;
                uint32_t slices;
                float max_distance;
                const char* name;
            };
            static constexpr QualityPreset presets[] = {
                {GLFW_KEY_1,  512, 16, 20.0f, "Low"},
                {GLFW_KEY_2, 1024, 32, 40.0f, "Medium"},
                {GLFW_KEY_3, 2048, 48, 60.0f, "High"},
                {GLFW_KEY_4, 4096, 64, 100.0f, "Ultra"},
            };
            for (auto& p : presets)
            {
                if (glfwGetKey(window.handle(), p.key) == GLFW_PRESS)
                {
                    shadow_params.atlas_page_resolution = p.res;
                    shadow_params.max_shadow_slices     = p.slices;
                    shadow_params.non_directional_shadow_max_distance = p.max_distance;
                    FeatureParamsProxy(session).setParams(
                        srv.scene_id, shadow_feat.feature, srv.shadow_params_op,
                        &shadow_params, sizeof(shadow_params));
                    std::cout << "  Shadow quality → " << p.name
                              << " (" << p.res << ", " << p.slices
                              << " slices, maxDist=" << p.max_distance << ")\n";
                    break;
                }
            }
        }

        // ── Directional CSM toggle (key 5, edge-triggered) ───────────
        const bool csm_toggle_key_down = (glfwGetKey(window.handle(), GLFW_KEY_5) == GLFW_PRESS);
        if (srv.shadow_params_op != kInvalidTypeId &&
            csm_toggle_key_down && !csm_toggle_key_down_prev)
        {
            directional_csm_enabled = !directional_csm_enabled;

            shadow_params.enable_directional_csm = directional_csm_enabled ? 1u : 0u;
            FeatureParamsProxy(session).setParams(
                srv.scene_id, shadow_feat.feature, srv.shadow_params_op,
                &shadow_params, sizeof(shadow_params));

            std::cout << "  Directional shadow mode → "
                      << (directional_csm_enabled ? "CSM ON (multi-cascade)" : "CSM OFF (single-slice)")
                      << " [target dir_cascade_count="
                      << (directional_csm_enabled ? "light-config" : "1")
                      << "]\n";
        }
        csm_toggle_key_down_prev = csm_toggle_key_down;
    }

    co_return;
}

// ── main ────────────────────────────────────────────────────────────────

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)   // single-lane overrides
    {
        const std::string a = argv[i];
        if      (a == "--no-floor") g_no_floor = true;
        else if (a == "--evsm")     g_use_evsm = true;
        else if (a == "--grid" && i + 1 < argc)
        {
            g_grid_side = std::atoi(argv[++i]);
            if (g_grid_side < 1) g_grid_side = 1;
        }
    }

    std::cout << "=== Deferred Rendering Stress Test ===\n"
              << "  " << (g_grid_side * g_grid_side) << " cubes"
              << (g_no_floor ? " (no floor)" : " + 1 floor") << ", "
              << kNumPointLights << " point lights + 1 directional\n\n";

    fs::path asset_dir(asset_path);

    // ── Channel + sync + window ─────────────────────────────────────

    auto channel = RenderProgramChannel<>::create();
    auto sync    = std::make_shared<RenderChannelSync>();

    auto surface_exts = getVulkanExtensions();
    lux::window::LuxWindow window(1280, 720, "Deferred Stress Test");

    // ── Server thread (with pre-initialization) ─────────────────────

    std::atomic<bool> server_ready{false};
    std::atomic<bool> server_failed{false};
    ServerInitResult  srv_init{};

    std::thread server_thread([&]
    {
        GeneralRenderServer server(channel, sync);

        ServerConfig cfg;
        cfg.instance_extensions = surface_exts;
        cfg.enable_vsync        = false;
        cfg.enable_validation   = false;
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

        // ── Pre-initialize: register factories ──────────────────────
        // StandardViewCameraFeature owns the per-scene 3D camera data (per-view
        // view/proj matrices + frustum). Register it first so its op-ids are
        // available for the ViewCameraProxy, and add it to the scene BEFORE every
        // camera consumer (see stressTask — owner-first).
        {
            auto reply = server.addFeatureFactory(kStandardViewCameraFeatureFactory);
            srv_init.view_cam_tid = reply.feature_type_id;
            srv_init.view_cam_ops = ViewCameraOperationIds::fromOps(reply.ops, reply.op_count);
        }
        // LightFeature owns scene light DATA; deferred-lighting/shadow CONSUME it
        // (find<LightResources>). Register it first so its op-ids are available for
        // the LightProxy, and add it to the scene BEFORE shadow (see stressTask).
        {
            auto light_reply = server.addFeatureFactory(kLightFeatureFactory);
            srv_init.light_tid = light_reply.feature_type_id;
            srv_init.light_ops = LightOperationIds::fromOps(light_reply.ops, light_reply.op_count);
        }
        // StandardMaterialFeature owns the global material stack (ShadingModelRegistry
        // + MaterialResources). It MUST be registered + added BEFORE StandardMeshStack
        // (addMeshInstance reads the material slot) and all material consumers.
        {
            auto reply = server.addFeatureFactory(kStandardMaterialFeatureFactory);
            srv_init.material_tid = reply.feature_type_id;
            srv_init.material_ops = MaterialOperationIds::fromOps(reply.ops, reply.op_count);
        }
        {
            auto reply = server.addFeatureFactory(kStandardMeshStackFeatureFactory);
            srv_init.mesh_stack_tid = reply.feature_type_id;
            srv_init.mesh_stack_ops = MeshStackOperationIds::fromOps(reply.ops, reply.op_count);
        }
        srv_init.gbuf_tid       = server.addFeatureFactory(kDeferredGBufferFeatureFactory).feature_type_id;
        srv_init.lighting_tid   = server.addFeatureFactory(kDeferredLightingFeatureFactory).feature_type_id;
        {
            auto reply = server.addFeatureFactory(kShadowMapFeatureFactory);
            srv_init.shadowmap_tid = reply.feature_type_id;
            srv_init.shadow_params_op = reply.op_count > 0 ? reply.ops[0] : kInvalidTypeId;
        }
        srv_init.meshshadow_tid = server.addFeatureFactory(kMeshShadowFeatureFactory).feature_type_id;
        {
            auto reply = server.addFeatureFactory(kSkyboxFeatureFactory);
            srv_init.skybox_tid  = reply.feature_type_id;
            srv_init.skybox_ops  = SkyboxOperationIds::fromOps(reply.ops, reply.op_count);
        }
        {
            auto reply = server.addFeatureFactory(kGridFeatureFactory);
            srv_init.grid_tid    = reply.feature_type_id;
            srv_init.grid_ops    = GridOperationIds::fromOps(reply.ops, reply.op_count);
        }

        std::cout << "  Feature factories registered (type_ids: light="
                  << srv_init.light_tid << ", " << srv_init.gbuf_tid << ", "
                  << srv_init.lighting_tid << ", "
                  << srv_init.shadowmap_tid << ", " << srv_init.meshshadow_tid << ", "
                  << srv_init.skybox_tid << ", " << srv_init.grid_tid << ")\n";

        // ── Pre-initialize: create scene + view ─────────────────────
        auto scene = server.createScene("DeferredStress", {}, lux::common::ETextureFormat::RGBA8_SRGB);
        srv_init.scene_id = scene.scene_id;
        std::cout << "  Scene created\n";

        auto view_result = server.createView({
            .scene_id     = scene.scene_id,
            .extent       = {1280, 720},
            .name         = "MainView",
        });
        if (view_result)
        {
            auto bind_result = server.bindSwapchain(scene.scene_id, *view_result, server.swapchainLayout());
            if (!bind_result)
            {
                std::cerr << "[Server] BindSwapchain failed\n";
                server_failed.store(true, std::memory_order_release);
                server_ready.store(true, std::memory_order_release);
                return;
            }
            srv_init.view = *view_result;
            std::cout << "  Swapchain view created\n";
        }
        else
        {
            std::cerr << "[Server] CreateView failed\n";
            server_failed.store(true, std::memory_order_release);
            server_ready.store(true, std::memory_order_release);
            return;
        }

        server_ready.store(true, std::memory_order_release);
        while (server.tick()) {}
    });

    while (!server_ready.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    if (server_failed.load())
    {
        std::cerr << "Server init failed (no Vulkan?). Exiting.\n";
        sync->requestStop();
        server_thread.join();
        return 1;
    }

    // ── Client session + coroutine ─────────────────────────────────

    RenderSession session(channel, sync);
    RenderTaskScheduler scheduler(session);

    auto task = stressTask(session, asset_dir, window, srv_init);
    scheduler.run(
        std::move(task),
        [&](RenderSession &) -> bool
        {
            window.pollEvents();
            return !window.shouldClose();
        });

    // ── Shutdown ────────────────────────────────────────────────────
    sync->requestStop();
    if (server_thread.joinable())
        server_thread.join();

    std::cout << "\n=== Deferred Stress Test Complete ===\n";
    return 0;
}
