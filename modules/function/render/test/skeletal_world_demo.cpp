// ============================================================================
//  skeletal_world_demo.cpp — real rigged-model end-to-end (ECS World path)
//
//  Loads a REAL skeletal model from a command-line path and drives it through
//  the full ECS → renderer stack:
//
//    ModelSerDeser (Assimp) -> Mesh/Skeleton/Clip/Material assets
//      -> ECS entity {SkeletalMeshComponent + AnimatorComponent + Transform}
//      -> World tick: AnimationSystem (samplePose + buildSkinningMatrices)
//      -> RenderableSystem (async .then upload/instance + uploadBoneBatch)
//      -> SkinningFeature compute -> vertex pool -> Forward/Shadow draws
//
//  Unlike skeletal_mesh_walk_test (hand-built 2-bone box, direct RenderSession),
//  this exercises the gameplay World + RenderableSystem with a real loaded asset.
//
//  ⚠ W5a KNOWN DEGRADATION: this demo's floor + the imported model's materials are
//  builtin closure MaterialAssets (EAssetType::MATERIAL). W5a retired the builtin
//  render-material path, so RenderableBridgeContext::ensureMaterial now returns null
//  for type MATERIAL and the static mesh bridge skips those instances — the geometry
//  renders INVISIBLE. The shipping editor is unaffected (its AssetImporter converts
//  imported materials to MaterialAssets at import; DemoSceneTemplate uses the
//  white GRAPH material). This standalone demo will render again once W5b makes the
//  importer produce graph materials (or it's updated to bake a graph material here).
//
//  Usage:  skeletal_world_demo <model_path> [uniform_scale]
//    e.g.  skeletal_world_demo CesiumMan.glb            (Khronos sample, CC-BY)
//          skeletal_world_demo Walking.fbx 0.01         (Mixamo, cm -> m)
//  The model is NOT committed to the repo — pass a local/LFS path.
//
//  Camera:  LMB-drag orbit | RMB/MMB-drag pan | scroll or W/S zoom | R reset.
//
//  NOTE: RenderableSystem does not yet upload a material's referenced TEXTURES
//  (only the material descriptor), so a textured PBR material would sample unbound
//  bindless slots and render black. Until that lands, the demo overrides imported
//  materials with a solid PBR colour (kSolidColorMaterial) so the animation is
//  clearly visible.
//
//  WINDOWED: opens a window and renders until closed (close manually).
// ============================================================================

#include <lux/engine/render/comm/client/RenderSession.hpp>
#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/render/comm/RenderProtocol.hpp>

#include <lux/engine/render/renderer/features/forward/ForwardMeshOperation.hpp>
#include <lux/engine/render/renderer/features/shadow/ShadowMapOperation.hpp>
#include <lux/engine/render/renderer/features/shadow/MeshShadowOperation.hpp>
#include <lux/engine/render/renderer/features/material/MaterialOperation.hpp>  // kStandardMaterialFeatureFactory (standard material stack; no config; attaches before StandardMeshStack)
#include <lux/engine/render/renderer/features/meshstack/MeshStackOperation.hpp>  // kStandardMeshStackFeatureFactory (standard 3D mesh stack; no config)
#include <lux/engine/render/renderer/features/skinning/SkinningOperation.hpp>
#include <lux/engine/render/renderer/features/grid/GridOperation.hpp>
#include <lux/engine/render/renderer/features/view_camera/ViewCameraOperation.hpp>  // StandardViewCamera (per-view camera op; replaces RenderSession::updateView)

#include <lux/engine/gameplay/world/World.hpp>
#include <lux/engine/gameplay/render_bridge/RenderableSystem.hpp>
#include <lux/engine/gameplay/3d/world/components/TransformComponent.hpp>
#include <lux/engine/gameplay/3d/world/components/WorldTransformComponent.hpp>
#include <lux/engine/gameplay/3d/world/components/MeshComponent.hpp>
#include <lux/engine/gameplay/3d/world/components/SkeletalMeshComponent.hpp>
#include <lux/engine/gameplay/3d/world/components/AnimatorComponent.hpp>

#include <lux/engine/asset/AssetManager.hpp>
#include <lux/engine/asset/ModelSerDeser.hpp>
#include <lux/engine/asset/ModelAsset.hpp>
#include <lux/engine/asset/MeshAsset.hpp>      // mesh bounds (up-axis + framing)
#include <lux/engine/asset/MaterialAsset.hpp>  // solid-colour material override
#include <lux/engine/math/AABB.hpp>

#include <lux/engine/window/LuxWindow.hpp>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace lux::render;

// ── Matrix helpers (same as skeletal_mesh_walk_test) ────────────────────────
static Eigen::Matrix4f buildViewMatrix(const Eigen::Vector3f& eye,
                                       const Eigen::Vector3f& target,
                                       const Eigen::Vector3f& up)
{
    Eigen::Vector3f f = (target - eye).normalized();
    Eigen::Vector3f s = f.cross(up).normalized();
    Eigen::Vector3f u = s.cross(f);
    Eigen::Matrix4f V = Eigen::Matrix4f::Identity();
    V(0,0)=s.x(); V(0,1)=s.y(); V(0,2)=s.z(); V(0,3)=-s.dot(eye);
    V(1,0)=u.x(); V(1,1)=u.y(); V(1,2)=u.z(); V(1,3)=-u.dot(eye);
    V(2,0)=-f.x();V(2,1)=-f.y();V(2,2)=-f.z();V(2,3)= f.dot(eye);
    return V;
}

static Eigen::Matrix4f buildProjMatrix(float fov_rad, float aspect, float near_z, float far_z)
{
    float t = std::tan(fov_rad * 0.5f);
    Eigen::Matrix4f P = Eigen::Matrix4f::Zero();
    P(0,0)=1.f/(aspect*t); P(1,1)=-1.f/t;
    P(2,2)=-far_z/(far_z-near_z); P(2,3)=-(far_z*near_z)/(far_z-near_z);
    P(3,2)=-1.f;
    return P;
}

static std::vector<const char*> getVulkanExtensions()
{
    glfwInit();
    uint32_t count = 0;
    const char** exts = glfwGetRequiredInstanceExtensions(&count);
    std::vector<const char*> r;
    for (uint32_t i = 0; i < count; ++i) r.emplace_back(exts[i]);
    return r;
}

// ── Interactive orbit camera ────────────────────────────────────────────────
//   LMB-drag : orbit     RMB/MMB-drag : pan     scroll or W/S : zoom
//   arrows   : orbit     R : reset            P : print eye/target
// Polled each frame from GLFW (scroll needs a callback → file-scope accumulator).
namespace { double g_scroll_accum = 0.0; }
static void scrollCallback(GLFWwindow*, double /*xoff*/, double yoff) { g_scroll_accum += yoff; }

struct OrbitCamera
{
    Eigen::Vector3f target{0.f, 0.f, 0.f};
    float yaw{0.f}, pitch{0.f}, dist{4.f};
    Eigen::Vector3f home_target{0.f, 0.f, 0.f};
    float home_dist{4.f};
    bool   have_last{false};
    double last_x{0.0}, last_y{0.0};

    Eigen::Vector3f eye() const
    {
        const float cp = std::cos(pitch), sp = std::sin(pitch);
        const float cy = std::cos(yaw),   sy = std::sin(yaw);
        return target + dist * Eigen::Vector3f(cp * sy, sp, cp * cy);
    }

    void update(lux::window::LuxWindow& window, float dt)
    {
        GLFWwindow* w = window.handle();
        double mx = 0.0, my = 0.0;
        glfwGetCursorPos(w, &mx, &my);

        const bool lmb = glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_LEFT)   == GLFW_PRESS;
        const bool pan = glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_RIGHT)  == GLFW_PRESS
                      || glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;

        if ((lmb || pan) && !have_last) { last_x = mx; last_y = my; have_last = true; }
        const float ddx = static_cast<float>(mx - last_x);
        const float ddy = static_cast<float>(my - last_y);
        last_x = mx; last_y = my;
        if (!(lmb || pan)) have_last = false;

        if (lmb) { yaw -= ddx * 0.005f; pitch -= ddy * 0.005f; }

        if (glfwGetKey(w, GLFW_KEY_LEFT)  == GLFW_PRESS) yaw   += 1.5f * dt;
        if (glfwGetKey(w, GLFW_KEY_RIGHT) == GLFW_PRESS) yaw   -= 1.5f * dt;
        if (glfwGetKey(w, GLFW_KEY_UP)    == GLFW_PRESS) pitch += 1.5f * dt;
        if (glfwGetKey(w, GLFW_KEY_DOWN)  == GLFW_PRESS) pitch -= 1.5f * dt;
        pitch = std::clamp(pitch, -1.55f, 1.55f);

        if (pan && !lmb)
        {
            const Eigen::Vector3f e   = eye();
            const Eigen::Vector3f fwd = (target - e).normalized();
            const Eigen::Vector3f rgt = fwd.cross(Eigen::Vector3f(0, 1, 0)).normalized();
            const Eigen::Vector3f cup = rgt.cross(fwd);
            const float s = dist * 0.0015f;
            target += (-ddx * s) * rgt + (ddy * s) * cup;
        }

        if (g_scroll_accum != 0.0) { dist *= std::pow(0.9f, static_cast<float>(g_scroll_accum)); g_scroll_accum = 0.0; }
        if (glfwGetKey(w, GLFW_KEY_W) == GLFW_PRESS || glfwGetKey(w, GLFW_KEY_EQUAL) == GLFW_PRESS) dist *= std::pow(0.5f, dt);
        if (glfwGetKey(w, GLFW_KEY_S) == GLFW_PRESS || glfwGetKey(w, GLFW_KEY_MINUS) == GLFW_PRESS) dist *= std::pow(2.0f, dt);
        dist = std::clamp(dist, 0.02f, 2000.f);

        if (glfwGetKey(w, GLFW_KEY_R) == GLFW_PRESS) { target = home_target; dist = home_dist; yaw = 0.f; pitch = 0.f; }

        static bool p_prev = false;
        const bool p_now = glfwGetKey(w, GLFW_KEY_P) == GLFW_PRESS;
        if (p_now && !p_prev) {
            const Eigen::Vector3f e = eye();
            std::cout << "  [CAM] eye=(" << e.transpose() << ") target=(" << target.transpose()
                      << ") dist=" << dist << "\n";
        }
        p_prev = p_now;
    }
};

// ── Phases (setup before the steady-state Render loop) ───────────────────────
enum class Phase {
    CreateScene, ActivateScene, BindSwapchain, RegisterFeatures, AddShadowFeatures,
    AddForwardFeature, AddSkinningFeature, AddGridFeature, AddRenderSystem, Render
};

int main(int argc, char** argv)
{
    std::cout << "=== Skeletal World Demo (real rigged model, ECS path) ===\n";

    // ---- CLI args: <model_path> [uniform_scale] ----
    if (argc < 2)
    {
        std::cout << "Usage: skeletal_world_demo <model_path> [uniform_scale]\n"
                     "  e.g. skeletal_world_demo CesiumMan.glb\n"
                     "       skeletal_world_demo Walking.fbx 0.01\n"
                     "  (model is NOT in the repo — pass a local/LFS path; e.g. Khronos\n"
                     "   CesiumMan.glb, CC-BY 4.0.)\n";
        return 0;  // graceful: nothing to do without a model
    }
    const std::filesystem::path model_path = argv[1];
    float model_scale = (argc >= 3) ? std::strtof(argv[2], nullptr) : 1.0f;
    if (!(model_scale > 0.0f)) model_scale = 1.0f;

    // ---- Single-lane investigation toggles (see hidden-sleeping-chipmunk.md).
    //   --no-ground    : skip the static ground plane (drops the static draw-lane).
    //   --force-static : render the model via the STATIC path (MeshComponent) instead
    //                    of the skinned path — turns the skinned lane into a static one.
    //   --dup-model K  : create K copies of the model (same mesh => still 1 MDC lane,
    //                    instance_count=K) to separate "lane count" from "instance count".
    // Flags may appear anywhere after argv[1]; a numeric argv[2] is still the scale.
    bool opt_no_ground    = false;
    bool opt_force_static = false;
    int  opt_dup_model    = 1;
    bool opt_churn        = false;  // After ~4 sec, destroy all tracked entities — verifies the refcount→destroy path
    for (int i = 2; i < argc; ++i)
    {
        const std::string a = argv[i];
        if      (a == "--no-ground")    opt_no_ground = true;
        else if (a == "--force-static") opt_force_static = true;
        else if (a == "--dup-model" && i + 1 < argc)
        {
            opt_dup_model = std::atoi(argv[++i]);
            if (opt_dup_model < 1) opt_dup_model = 1;
        }
        else if (a == "--churn") opt_churn = true;
    }
    std::cout << "  [opts] no_ground=" << opt_no_ground
              << " force_static=" << opt_force_static
              << " dup_model=" << opt_dup_model
              << " churn="     << opt_churn << "\n";

    if (!std::filesystem::exists(model_path))
    {
        std::cerr << "Model not found: " << model_path << "\n";
        return 1;
    }

    // ---- Load the model (CPU only; before any Vulkan) ----
    auto mgr = std::make_shared<lux::asset::AssetManager>();
    lux::asset::ModelSerDeser model_ser(mgr);
    std::cout << "Importing model: " << model_path << " (scale " << model_scale << ")\n";
    auto import_result = model_ser.importFromFile(model_path);
    if (!import_result)
    {
        std::cerr << "importFromFile failed (error "
                  << static_cast<int>(import_result.error()) << ").\n";
        return 1;
    }
    const auto* model = static_cast<const lux::asset::ModelAsset*>(import_result.value().first);
    const auto& mesh_ids = model->meshAssetIds();
    const auto& mat_ids  = model->materialAssetIds();
    const bool  has_skel = model->skeletonAssetId().has_value();
    const lux::asset::asset_id_t skel_id =
        has_skel ? *model->skeletonAssetId() : lux::asset::asset_id_t{};
    const lux::asset::asset_id_t clip_id =
        model->animationClipAssetIds().empty() ? lux::asset::asset_id_t{}
                                               : model->animationClipAssetIds().front();

    std::cout << "  meshes=" << mesh_ids.size()
              << " materials=" << mat_ids.size()
              << " skeleton=" << (has_skel ? "yes" : "no")
              << " clips=" << model->animationClipAssetIds().size() << "\n";
    if (mesh_ids.empty()) { std::cerr << "Model has no meshes.\n"; return 1; }
    if (!has_skel)
        std::cout << "  (no skeleton — rendering as STATIC mesh; pass a rigged model to animate)\n";

    // ---- Up-axis correction from the model's combined bounds ----
    // Many rigged assets (e.g. CesiumMan.glb via Assimp) arrive Z-up while our
    // camera is Y-up, which would lay the character end-on to the camera. Pick the
    // longest AABB axis as "up" and rotate it onto +Y; the same bounds frame the
    // camera. (For arbitrary models this is a heuristic; the CLI scale + camera
    // controls cover the rest.)
    lux::math::AABB model_box;
    for (const auto& mid : mesh_ids)
        if (auto* ma = mgr->fetchAssetAs<lux::asset::MeshAsset>(mid);
            ma && ma->data() && ma->data()->bounds)
            model_box.merge(*ma->data()->bounds);

    Eigen::Quaternionf up_fix = Eigen::Quaternionf::Identity();
    if (model_box.isValid())
    {
        const Eigen::Vector3f ext = model_box.extents();
        int up_axis = 1;                                                     // assume Y-up
        if (ext.z() >= ext.x() && ext.z() >= ext.y())      up_axis = 2;      // Z tallest
        else if (ext.x() >= ext.y() && ext.x() >= ext.z()) up_axis = 0;      // X tallest
        if (up_axis == 2)
            up_fix = Eigen::Quaternionf(Eigen::AngleAxisf(-1.57079633f, Eigen::Vector3f::UnitX())); // Z-up -> Y-up
        else if (up_axis == 0)
            up_fix = Eigen::Quaternionf(Eigen::AngleAxisf( 1.57079633f, Eigen::Vector3f::UnitZ())); // X-up -> Y-up
        std::cout << "  up-axis=" << (up_axis == 0 ? "X" : up_axis == 1 ? "Y" : "Z")
                  << (up_axis == 1 ? " (no rotation)" : " (rotated onto +Y)") << "\n";
    }

    // ---- Solid-colour material override (debug toggle) ----
    // #57 wired texture upload through RenderableSystem, so imported materials now
    // show their real textures. Flip to true to fall back to a flat PBR colour
    // (e.g. to isolate a material/texture issue).
    constexpr bool kSolidColorMaterial = false;
    if (kSolidColorMaterial)
    {
        for (const auto& mid : mat_ids)
        {
            auto* ma = mgr->fetchAssetAs<lux::asset::MaterialAsset>(mid);
            if (!ma || !ma->data()) continue;
            *ma->data() = lux::rdesc::Material::makePbrMetallicRoughness();
            if (auto* pbr = ma->data()->asPbrMetallicRoughness())
            {
                pbr->base_color.value = Eigen::Vector3f(0.85f, 0.62f, 0.40f);
                pbr->metallic.value   = 0.0f;
                pbr->roughness.value  = 0.7f;
            }
        }
    }

    // ---- Window + render server (separate thread) + session ----
    auto channel = RenderProgramChannel<>::create();
    auto sync = std::make_shared<RenderChannelSync>();
    auto surface_exts = getVulkanExtensions();
    lux::window::LuxWindow window(1280, 720, "Skeletal World Demo");
    glfwSetScrollCallback(window.handle(), scrollCallback);   // scroll-to-zoom

    std::atomic<bool> server_ready{false}, server_failed{false};
    std::thread server_thread([&]{
        GeneralRenderServer server(channel, sync);
        ServerConfig cfg; cfg.instance_extensions = surface_exts;
        if (auto r = server.init(std::move(cfg)); !r) {
            std::cerr << "[Server] init failed: " << r.error().message() << "\n";
            server_failed.store(true); server_ready.store(true); return;
        }
        if (auto r = server.attachToWindow(window); !r) {
            std::cerr << "[Server] attach failed: " << r.error().message() << "\n";
            server_failed.store(true); server_ready.store(true); return;
        }
        server_ready.store(true);
        while (server.tick()) {}
    });

    while (!server_ready.load()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (server_failed.load()) {
        std::cerr << "Server init failed (no Vulkan?). Skipping.\n";
        sync->requestStop(); server_thread.join(); return 0;
    }

    RenderSession session(channel, sync);

    // ---- ECS World (auto-wires Transform/Camera/Animation systems) + entities.
    // RenderableSystem is added later (it needs the scene_id + view, created
    // asynchronously below).
    lux::gameplay::World world(*mgr);
    std::vector<lux::meta::entity_id> churn_entities;  // --churn: tracked entities to destroy at frame ~240
    for (int dup = 0; dup < opt_dup_model; ++dup)      // --dup-model creates K copies
    for (std::size_t i = 0; i < mesh_ids.size(); ++i)
    {
        auto e = world.createEntity();
        if (opt_churn) churn_entities.push_back(e);

        // Transform + WorldTransform are BOTH required: TransformSystem only
        // updates entities that already have a WorldTransformComponent, and
        // RenderableSystem reads it.
        auto& tf = world.emplace<lux::gameplay::d3::TransformComponent>(e);
        tf.rotation = up_fix;                              // stand the model upright (Y-up)
        tf.scale    = Eigen::Vector3f::Constant(model_scale);
        if (dup > 0)                                       // offset extra copies so they don't fully overlap
            tf.position = Eigen::Vector3f(static_cast<float>(dup) * 1.5f, 0.f, 0.f);
        world.emplace<lux::gameplay::d3::WorldTransformComponent>(e);

        const lux::asset::asset_id_t mat_id =
            (i < mat_ids.size()) ? mat_ids[i] : lux::asset::asset_id_t{};

        if (has_skel && !opt_force_static)   // --force-static routes via the static path
        {
            auto& sk = world.emplace<lux::gameplay::d3::SkeletalMeshComponent>(e);
            sk.mesh_asset_id     = mesh_ids[i];
            sk.material_asset_id = mat_id;
            sk.skeleton_asset_id = skel_id;
            auto& an = world.emplace<lux::gameplay::d3::AnimatorComponent>(e);
            an.clip_asset_id = clip_id;   // nil clip -> bind pose
            an.speed         = 1.0f;
        }
        else
        {
            auto& mc = world.emplace<lux::gameplay::d3::MeshComponent>(e);
            mc.mesh_asset_id     = mesh_ids[i];
            mc.material_asset_id = mat_id;
        }
    }
    // ---- Static ground plane (gives the character a floor + animated shadow,
    // and a spatial reference). Built procedurally + registered as Mesh/Material
    // assets, then driven through the same ECS/RenderableSystem static path. ----
    if (!opt_no_ground)                                   // --no-ground drops this static lane
    {
        using V3 = Eigen::Vector3f; using V2 = Eigen::Vector2f;
        constexpr float kHalf = 8.0f;
        auto floor_mesh = std::make_unique<lux::rdesc::Mesh>();
        lux::rdesc::Vertex a{}, b{}, c{}, d{};
        a.position = {-kHalf, 0.f, -kHalf}; b.position = {kHalf, 0.f, -kHalf};
        c.position = { kHalf, 0.f,  kHalf}; d.position = {-kHalf, 0.f,  kHalf};
        a.uv = {0.f,0.f}; b.uv = {1.f,0.f}; c.uv = {1.f,1.f}; d.uv = {0.f,1.f};
        for (auto* v : {&a,&b,&c,&d}) { v->normal = {0.f,1.f,0.f}; v->tangent = {1.f,0.f,0.f}; v->bitangent = {0.f,0.f,1.f}; }
        floor_mesh->vertices = {a, b, c, d};
        floor_mesh->indices  = {0, 2, 1, 0, 3, 2};   // front face up (CCW from above; matches normal +Y)
        floor_mesh->bounds   = lux::math::AABB(V3{-kHalf, -0.01f, -kHalf}, V3{kHalf, 0.01f, kHalf});
        auto fmesh = mgr->createAsset<lux::asset::MeshAsset>(std::move(floor_mesh));
        const lux::asset::asset_id_t floor_mesh_id = fmesh->id();
        mgr->registerAsset(std::move(fmesh));

        auto floor_mat = std::make_unique<lux::rdesc::Material>(lux::rdesc::Material::makePbrMetallicRoughness());
        if (auto* pbr = floor_mat->asPbrMetallicRoughness()) {
            pbr->base_color.value = Eigen::Vector3f(0.35f, 0.37f, 0.40f);
            pbr->metallic.value   = 0.0f;
            pbr->roughness.value  = 0.9f;
        }
        // Single-quad ground: make it two-sided so it's visible from below too
        // (otherwise back-face cull makes the floor vanish when the camera dips
        // under it). Routes this material to the no-cull pipeline variant.
        floor_mat->state().double_sided = true;
        auto fmat = mgr->createAsset<lux::asset::MaterialAsset>(std::move(floor_mat));
        const lux::asset::asset_id_t floor_mat_id = fmat->id();
        mgr->registerAsset(std::move(fmat));

        auto fe = world.createEntity();
        if (opt_churn) churn_entities.push_back(fe);
        world.emplace<lux::gameplay::d3::TransformComponent>(fe);          // identity (floor at y=0)
        world.emplace<lux::gameplay::d3::WorldTransformComponent>(fe);
        auto& fmc = world.emplace<lux::gameplay::d3::MeshComponent>(fe);
        fmc.mesh_asset_id     = floor_mesh_id;
        fmc.material_asset_id = floor_mat_id;
    }

    std::cout << "  created " << (mesh_ids.size() * static_cast<std::size_t>(opt_dup_model))
              << " model entit"
              << ((mesh_ids.size() * static_cast<std::size_t>(opt_dup_model)) == 1 ? "y" : "ies")
              << (opt_no_ground ? " (no ground plane)" : " + 1 ground plane")
              << (opt_force_static ? " [forced static path]" : "")
              << " in the World.\n";

    // ---- Interactive orbit camera, framed from the (rotated, scaled) bounds ----
    const Eigen::Vector3f up(0.f, 1.f, 0.f);
    Eigen::Vector3f center(0.f, 0.9f, 0.f);
    float view_dist = 3.5f;
    if (model_box.isValid())
    {
        center = up_fix * (model_box.center() * model_scale);   // rotated, scaled centroid
        const float height = model_box.extents().maxCoeff() * model_scale;
        view_dist = std::max(height * 1.8f, 1.0f);
    }
    OrbitCamera cam;
    cam.target = cam.home_target = center;
    cam.dist   = cam.home_dist   = view_dist;
    const Eigen::Matrix4f P = buildProjMatrix(60.f * 3.14159265f / 180.f, 1280.f/720.f, 0.05f, 200.f);
    std::cout << "  Camera: LMB-drag orbit | RMB/MMB-drag pan | scroll or W/S zoom | R reset\n";

    Phase phase = Phase::CreateScene;
    RenderRequest<SceneCreatedReply> scene_req;
    RenderRequest<GenericOkReply> active_req;
    RenderRequest<ViewCreatedReply> view_req;
    RenderRequest<FeatureTypeRegisteredReply> fwd_reg, shadow_reg, mesh_shadow_reg, skin_reg, grid_reg, material_reg, mesh_stack_reg, view_cam_reg;
    RenderRequest<FeatureAddedReply> fwd_feat, shadow_feat, mesh_shadow_feat, skin_feat, grid_feat, material_feat, mesh_stack_feat, view_cam_feat;
    ViewCameraOperationIds view_cam_ops{};   // feature-scoped per-view camera op-ids (ViewCameraProxy)

    auto t_prev = std::chrono::steady_clock::now();
    uint64_t frame = 0;

    while (!window.shouldClose())
    {
        window.pollEvents();
        session.beginFrame();
        switch (phase)
        {
        case Phase::CreateScene:
            scene_req = session.createScene("WorldScene"); break;
        case Phase::ActivateScene:
            active_req = session.setActiveScene(scene_req.result().scene_id, true);
            view_req = session.addView(scene_req.result().scene_id, {1280,720}, "MainView"); break;
        case Phase::BindSwapchain:
            session.bindSwapchain(scene_req.result().scene_id, view_req.result().view); break;
        case Phase::RegisterFeatures:
            // StandardViewCamera owns the per-view camera matrices (frustum +
            // view/proj). It MUST attach before every camera consumer (Forward /
            // ShadowMap / MeshShadow), so register it up front.
            view_cam_reg = session.registerFeatureType(kStandardViewCameraFeatureFactory);
            fwd_reg = session.registerFeatureType(kForwardMeshFeatureFactory);
            shadow_reg = session.registerFeatureType(kShadowMapFeatureFactory);
            mesh_shadow_reg = session.registerFeatureType(kMeshShadowFeatureFactory);
            // StandardMaterialFeature owns the global material stack; it MUST attach
            // before StandardMeshStack (addMeshInstance reads the material slot) and
            // every material consumer. Register it ahead of mesh-stack.
            material_reg = session.registerFeatureType(kStandardMaterialFeatureFactory);
            mesh_stack_reg = session.registerFeatureType(kStandardMeshStackFeatureFactory);
            skin_reg = session.registerFeatureType(kSkinningFeatureFactory);
            grid_reg = session.registerFeatureType(kGridFeatureFactory); break;
        case Phase::AddShadowFeatures: {
            // StandardViewCamera MUST attach before every camera consumer
            // (Forward / ShadowMap / MeshShadow), so add it first. No comm
            // config — empty struct param.
            struct EmptyViewCamCfg {} view_cam_cfg{};
            view_cam_feat = session.addFeature(scene_req.result().scene_id, view_cam_reg.result().feature_type_id, view_cam_cfg);
            // StandardMaterialFeature owns the global material stack
            // (ShadingModelRegistry + MaterialResources). It MUST attach before
            // StandardMeshStack (addMeshInstance reads the material slot) and every
            // material consumer. No comm config — empty struct param.
            struct EmptyMaterialCfg {} material_cfg{};
            material_feat = session.addFeature(scene_req.result().scene_id, material_reg.result().feature_type_id, material_cfg);
            // StandardMeshStackFeature owns the standard 3D mesh-stack scene
            // resources (no longer built in RenderScene's ctor); it MUST attach
            // before every mesh consumer (ShadowMap / MeshShadow / ForwardMesh /
            // Skinning) or meshes don't render. No comm config — empty struct param.
            struct EmptyMeshStackCfg {} mesh_stack_cfg{};
            mesh_stack_feat = session.addFeature(scene_req.result().scene_id, mesh_stack_reg.result().feature_type_id, mesh_stack_cfg);
            ShadowMapCommConfig smc{};
            smc.atlas_page_resolution  = 4096;
            smc.atlas_page_count       = 12;
            smc.max_shadow_slices      = 128;
            smc.enable_directional_csm = 1;   // directional CSM on
            smc.non_directional_shadow_max_distance = 1200.0f;
            shadow_feat = session.addFeature(scene_req.result().scene_id, shadow_reg.result().feature_type_id, smc);
            MeshShadowCommConfig msmc{};
            mesh_shadow_feat = session.addFeature(scene_req.result().scene_id, mesh_shadow_reg.result().feature_type_id, msmc);
            break;
        }
        case Phase::AddForwardFeature: {
            ForwardMeshCommConfig cc{};
            cc.comm_config_version = kForwardMeshCommConfigVersion;
            cc.descriptor_layout_version = kForwardMeshDescriptorLayoutVersion;
            fwd_feat = session.addFeature(scene_req.result().scene_id, fwd_reg.result().feature_type_id, cc);
            break;
        }
        case Phase::AddSkinningFeature: {
            struct Empty {} e{};
            skin_feat = session.addFeature(scene_req.result().scene_id, skin_reg.result().feature_type_id, e);
            break;
        }
        case Phase::AddGridFeature: {
            // Reference grid at y=0 (default shaders resolved server-side) — gives
            // a spatial floor so the character's orientation/footing is obvious.
            GridCommConfig gcc{};
            grid_feat = session.addFeature(scene_req.result().scene_id, grid_reg.result().feature_type_id, gcc);
            break;
        }
        case Phase::AddRenderSystem: {
            // Scene + view are ready — wire the ECS->renderer bridge into the World.
            // Skinning is feature-scoped now: forward its dynamic bone-upload op-ids
            // (from the SkinningFeature registration reply) so the skeletal mesh bridge
            // can address uploadBoneBatch.
            auto renderable_system = std::make_unique<lux::gameplay::RenderableSystem>(
                session, *mgr, scene_req.result().scene_id, view_req.result().view);
            renderable_system->setSkinningOps(SkinningOperationIds::fromOps(
                skin_reg.result().ops, skin_reg.result().op_count));
            world.addSystem(std::move(renderable_system));
            // Capture the per-view camera op-ids now that the registration reply
            // is ready — Phase::Render drives the camera through ViewCameraProxy.
            view_cam_ops = ViewCameraOperationIds::fromOps(
                view_cam_reg.result().ops, view_cam_reg.result().op_count);
            // A directional light so the (PBR) model is lit + casts a shadow.
            // Full shadow-caster config (mirrors deferred_stress_test): without
            // CAST_SHADOW + cascades + atlas the shadow map renders nothing.
            DirectionalLightDesc dl{};
            dl.direction = Eigen::Vector3f(-0.5f, -1.f, -0.3f).normalized();
            dl.color = Eigen::Vector3f(1.f, 0.95f, 0.8f);
            dl.intensity = 1.5f;
            dl.flags = 1;                 // LF_CAST_SHADOW
            dl.shadow_map_size = 2048;
            dl.shadow_bias = 0.002f;
            dl.shadow_normal_bias = 0.05f;
            dl.cascade_count = 4;
            dl.cascade_splits = {10.f, 30.f, 70.f, 150.f};
            session.createLight(scene_req.result().scene_id, LightDescriptor{dl});
            std::cout << "  RenderableSystem wired; entering render loop.\n";
            break;
        }
        case Phase::Render: {
            const float dt = std::chrono::duration<float>(
                std::chrono::steady_clock::now() - t_prev).count();
            t_prev = std::chrono::steady_clock::now();

            // --churn: at ~frame 240 (≈4 sec), destroy all tracked entities so
            // the next world.tick → RenderableSystem reap → release + destroy fires.
            // Watch the console for "[RenderableSystem] destroyed mesh/material/
            // texture (cache now=…)" lines as the caches drain to baseline.
            static bool churn_fired = false;
            if (opt_churn && !churn_fired && frame >= 240) {
                std::cout << "  [churn] destroying " << churn_entities.size()
                          << " renderable entities at frame " << frame << "\n";
                for (auto e : churn_entities) world.destroyEntity(e);
                churn_entities.clear();
                churn_fired = true;
            }

            // Drive the ECS: Transform -> Camera -> Animation (samples the clip
            // into AnimatorComponent.skinning_matrices) -> RenderableSystem
            // (uploads/instances + uploadBoneBatch). Must sit between
            // begin/submit so RenderableSystem's command pushes are valid.
            world.tick(dt);

            cam.update(window, dt);
            const Eigen::Vector3f e = cam.eye();
            const Eigen::Matrix4f V = buildViewMatrix(e, cam.target, up);
            ViewCameraProxy(session, view_cam_ops).update(
                scene_req.result().scene_id, view_req.result().view,
                V.data(), P.data(), e.data());
            break;
        }
        }
        session.submitFrame(true);
        session.waitAndPumpReplies();   // RenderableSystem's .then continuations fire here

        if (phase != Phase::Render)
            phase = static_cast<Phase>(static_cast<int>(phase) + 1);
        ++frame;
    }

    std::cout << "Closed after " << frame << " frames.\n";
    sync->requestStop();
    server_thread.join();
    return 0;
}
