// ============================================================================
//  thumbnail_render_gpu_test.cpp
//
//  End-to-end test of the editor thumbnail subsystem (T4 PreviewScene + T5
//  renderers) on a real GPU. Brings up the render server + a PreviewScene,
//  registers one asset of each kind, and drives the type→renderer registry to
//  produce thumbnails:
//
//    TEXTURE        → CPU downscale of the asset's pixels (verified lossless 1:1)
//    MESH           → lit cube rendered in the PreviewScene + readback
//    MODEL          → the model's mesh(es) rendered + readback
//    MATERIAL → built-in sphere wearing the graph material + readback
//
//  W5: the editor renders graph materials only (rdesc::Material retired). The
//  material asset is a MATERIAL baked in memory from a neutral PBR graph,
//  and the capture path compiles its forward frag + uploadGraphMaterial (its own
//  PSO, R1) instead of the old uploadMaterial closure path.
//
//  Self-checking: 0 = PASS, 1 = FAIL, 0 (skip) if no Vulkan device.
// ============================================================================

#include <lux/engine/editor/thumbnail/PreviewScene.hpp>
#include <lux/engine/editor/thumbnail/ThumbnailSpecProvider.hpp>
#include <lux/engine/editor/thumbnail/ThumbnailSet.hpp>
#include <lux/engine/editor/thumbnail/ImageCodec.hpp>
#include <lux/engine/editor/content/BuiltinGeometry.hpp>

#include <lux/engine/asset/AssetManager.hpp>
#include <lux/engine/asset/MeshAsset.hpp>
#include <lux/engine/asset/MaterialAsset.hpp>
#include <lux/engine/asset/TextureAsset.hpp>
#include <lux/engine/asset/ModelAsset.hpp>

#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/description/Texture.hpp>
#include <lux/engine/description/Vertex.hpp>
#include <lux/engine/description/ShaderInfo.hpp>

#include <lux/engine/description/material_graph/MaterialGraph.hpp>   // complete type for makeNeutralPbrGraph
#include "../src/import/MaterialGraphBake.hpp"           // makeNeutralPbrGraph + compileGraphToPayload

#include <lux/engine/render/comm/client/RenderSession.hpp>
#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/render/comm/RenderProtocol.hpp>
#include <lux/engine/render/resources/material/GraphMaterialData.hpp>
#include <lux/engine/math/AABB.hpp>

#include <lux/engine/window/LuxWindow.hpp>
#include <GLFW/glfw3.h>

#include <uuid.h>
#include <Eigen/Core>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace lux::editor;

static int g_fail = 0;
static void check(bool c, const char* n)
{
    std::printf(c ? "  [PASS] %s\n" : "  [FAIL] %s\n", n);
    if (!c) ++g_fail;
}

static std::vector<const char*> vkExts()
{
    glfwInit();
    uint32_t n = 0;
    const char** e = glfwGetRequiredInstanceExtensions(&n);
    return std::vector<const char*>(e, e + n);
}

static std::unique_ptr<lux::asset::AssetInfo> makeInfo(const char* uuid, lux::asset::EAssetType t)
{
    auto info  = std::make_unique<lux::asset::AssetInfo>();
    info->id   = uuids::uuid::from_string(uuid).value_or(lux::asset::asset_id_t{});
    info->type = t;
    return info;
}

// Drive frames (blocking) until @p req resolves — handles deferred replies.
template <class T>
static T awaitDriven(lux::render::RenderSession& s, lux::render::RenderRequest<T> req)
{
    s.submitFrame(/*blocking=*/true);
    s.waitAndPumpReplies();
    while (!req.isReady())
    {
        s.beginFrame({});
        s.submitFrame(/*blocking=*/true);
        s.waitAndPumpReplies();
    }
    return req.result();
}

static lux::render::RMeshHandle uploadMeshBlocking(lux::render::RenderSession& s, const lux::rdesc::Mesh& m,
    lux::render::MeshStackOperationIds mesh_stack_ops)
{
    s.beginFrame({});
    const auto r = awaitDriven(s, lux::render::MeshStackProxy(s, mesh_stack_ops).uploadMesh(m));
    return r.status == 0 ? r.handle : lux::render::RMeshHandle{};
}

// Bring up a baked GRAPH material (blocking): compile its FORWARD frag (the only
// pass the preview renders) + uploadGraphMaterial with its own PSO (R1). Mirrors
// ThumbnailService's async path, blocking for the test.
static lux::render::RMaterialHandle uploadGraphMaterialBlocking(
    lux::render::RenderSession& s, const lux::asset::MaterialData& payload,
    lux::render::MaterialOperationIds material_ops)
{
    if (payload.forward_spirv.empty()) return {};
    const auto info = lux::rdesc::ShaderInfo::serialize(payload.forward_info);
    const auto spv  = std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(payload.forward_spirv.data()),
        payload.forward_spirv.size() * sizeof(std::uint32_t)};
    s.beginFrame({});
    const auto compiled = awaitDriven(s, s.compileShader(
        spv, std::span<const std::byte>{info.data(), info.size()}));
    if (compiled.status != 0 || compiled.shader.is_null()) return {};

    lux::render::GraphMaterialData gd{};
    // Param lanes + render-state are DERIVED from the authoring graph (the SSOT).
    const auto& pslots = payload.graph.param_slots;
    gd.param_count = static_cast<std::uint32_t>(
        pslots.size() < lux::render::GraphMaterialData::kMaxParams
            ? pslots.size() : lux::render::GraphMaterialData::kMaxParams);
    for (std::uint32_t i = 0; i < lux::render::GraphMaterialData::kMaxParams; ++i)
        for (int k = 0; k < 4; ++k)
            gd.params[i][k] = (i < pslots.size()) ? pslots[i].dflt[k] : 0.0f;
    s.beginFrame({});
    const auto r = awaitDriven(s, lux::render::MaterialProxy(s, material_ops).uploadGraphMaterial(
        gd, lux::render::ShaderHandle{}, compiled.shader,
        static_cast<std::uint32_t>(payload.graph.render_state.alpha_mode),
        payload.graph.render_state.double_sided));
    return r.status == 0 ? r.handle : lux::render::RMaterialHandle{};
}

// Resolve a spec's instances to GPU handles (blocking upload; the resident
// sphere/default-grey for null fields) and capture the preview into raw BGRA8.
// Mirrors what ThumbnailService does asynchronously, but blocking for the test.
static std::vector<std::byte> captureSpec(lux::asset::AssetManager& mgr,
                                          lux::render::RenderSession& s,
                                          PreviewScene& preview, const ThumbnailSpec& spec)
{
    std::vector<PreviewScene::PreviewInstance> insts;
    for (const auto& in : spec.instances)
    {
        const auto mh = in.mesh_data ? uploadMeshBlocking(s, *in.mesh_data, preview.meshStackOps()) : preview.sphereMesh();
        lux::render::RMaterialHandle mat = preview.defaultMaterial();
        if (!in.graph_material_id.is_nil())
            if (const auto* ga = mgr.fetchAssetAs<lux::asset::MaterialAsset>(in.graph_material_id);
                ga && ga->data())
                mat = uploadGraphMaterialBlocking(s, *ga->data(), preview.materialOps());
        insts.push_back({mh, mat});
    }
    return preview.capture(insts, spec.bounds);
}

// Non-uniform (something drawn) + lit-pixel count over tightly-packed 4-byte px.
struct RawAnalysis { bool uniform{true}; std::size_t bright{0}; };
static RawAnalysis analyzeRaw(const std::vector<std::byte>& px)
{
    RawAnalysis a;
    for (std::size_t i = 4; a.uniform && i + 4 <= px.size(); i += 4)
        if (px[i] != px[0] || px[i+1] != px[1] || px[i+2] != px[2])
            a.uniform = false;
    for (std::size_t i = 0; i + 4 <= px.size(); i += 4)
    {
        const int mx = std::max({std::to_integer<int>(px[i]),
                                 std::to_integer<int>(px[i+1]),
                                 std::to_integer<int>(px[i+2])});
        if (mx > 40) ++a.bright;
    }
    return a;
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // unbuffered: progress survives a hang
    std::printf("=== thumbnail_render_gpu_test ===\n");

    auto channel = lux::render::RenderProgramChannel<>::create();
    auto sync    = std::make_shared<lux::render::RenderChannelSync>();
    auto exts    = vkExts();

    lux::window::LuxWindow window(64, 64, "thumbnail_render_gpu_test");

    std::atomic<bool> ready{false}, failed{false};
    std::thread server_thread([&]
    {
        lux::render::GeneralRenderServer server(channel, sync);
        lux::render::ServerConfig cfg;
        cfg.instance_extensions = exts;
        if (auto r = server.init(std::move(cfg)); !r) { failed = true; ready = true; return; }
        if (auto r = server.attachToWindow(window); !r) { failed = true; ready = true; return; }
        ready = true;
        try { while (server.tick()) {} }
        catch (const std::exception& e) { std::fprintf(stderr, "[Server] %s\n", e.what()); }
    });
    while (!ready.load()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (failed.load())
    {
        std::printf("Server init failed (no Vulkan?). Skipping.\n");
        sync->requestStop(); server_thread.join();
        return 0;
    }

    lux::render::RenderSession session(channel, sync);

    PreviewScene preview(session);
    check(preview.setup(256), "PreviewScene::setup");
    if (!preview.ready())
    {
        sync->requestStop(); server_thread.join();
        std::printf("=== thumbnail_render_gpu_test FAILED (preview setup) ===\n");
        return 1;
    }

    lux::asset::AssetManager mgr;
    auto registry = makeDefaultThumbnailSpecProviders();

    // ── Register one asset of each kind ─────────────────────────────────
    const char* kMeshId  = "00000000-0000-4000-8000-a00000000001";
    const char* kMatId   = "00000000-0000-4000-8000-a00000000002";
    const char* kTexId   = "00000000-0000-4000-8000-a00000000003";
    const char* kModelId = "00000000-0000-4000-8000-a00000000004";

    {   // cube mesh
        std::vector<lux::rdesc::Vertex> v; std::vector<std::uint32_t> idx;
        buildCubeGeometry(v, idx, 0.5f);
        lux::rdesc::Mesh mesh{std::move(v), std::move(idx),
            lux::math::AABB(Eigen::Vector3f(-0.5f,-0.5f,-0.5f), Eigen::Vector3f(0.5f,0.5f,0.5f))};
        auto a = std::make_unique<lux::asset::MeshAsset>(makeInfo(kMeshId, lux::asset::EAssetType::MESH));
        a->setData(std::make_unique<lux::rdesc::Mesh>(std::move(mesh)));
        mgr.registerAsset(std::move(a));
    }
    {   // orange GRAPH material (baked in memory from a neutral PBR graph)
        auto payload_exp = compileGraphToPayload(
            makeNeutralPbrGraph(0.85f, 0.35f, 0.1f, /*metallic=*/0.1f, /*roughness=*/0.45f),
            /*slot_texture_ids=*/{});
        check(payload_exp.has_value(), "orange graph material bakes");
        auto payload = std::make_unique<lux::asset::MaterialData>(
            payload_exp ? std::move(*payload_exp) : lux::asset::MaterialData{});
        auto a = std::make_unique<lux::asset::MaterialAsset>(
            makeInfo(kMatId, lux::asset::EAssetType::MATERIAL));
        a->setData(std::move(payload));
        mgr.registerAsset(std::move(a));
    }
    // 64x64 RGBA8 two-colour texture (kept alive for the whole test — the
    // Texture references / copies from this buffer).
    std::vector<std::byte> texpix(static_cast<std::size_t>(64) * 64 * 4);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
        {
            const std::size_t i = (static_cast<std::size_t>(y) * 64 + x) * 4;
            const bool left = x < 32;
            texpix[i+0] = std::byte(left ? 220 : 30);
            texpix[i+1] = std::byte(40);
            texpix[i+2] = std::byte(left ? 30 : 220);
            texpix[i+3] = std::byte(255);
        }
    {
        lux::rdesc::TextureInfo ti{};
        ti.width = 64; ti.height = 64; ti.channel = 4;
        ti.pixel_format = lux::rdesc::ETexturePixelFormat::RGBA8_UNORM;
        ti.mip_count = 1; ti.layers = 1;
        ti.copy = true; ti.owns_data = true;
        auto tex = std::make_unique<lux::rdesc::Texture>(ti, texpix.data(), texpix.size());
        auto a = std::make_unique<lux::asset::TextureAsset>(makeInfo(kTexId, lux::asset::EAssetType::TEXTURE));
        a->setData(std::move(tex));
        mgr.registerAsset(std::move(a));
    }
    {   // model referencing the cube mesh + orange material
        auto a = std::make_unique<lux::asset::ModelAsset>(makeInfo(kModelId, lux::asset::EAssetType::MODEL));
        a->addMeshAssetId(uuids::uuid::from_string(kMeshId).value());
        a->addMaterialAssetId(uuids::uuid::from_string(kMatId).value());
        mgr.registerAsset(std::move(a));
    }

    auto id = [](const char* s){ return uuids::uuid::from_string(s).value(); };

    // W2b: buildSpec now takes a request-load callback for data-less shells.
    // These assets are registered eagerly (data present), so it is never fired.
    const ThumbnailLoadFn noLoad = [](const lux::asset::asset_id_t&){};

    // ── TEXTURE: spec carries source pixels (CPU path, no scene render) ──
    {
        auto* r = registry.get(lux::asset::EAssetType::TEXTURE);
        check(r != nullptr, "texture renderer registered");
        const auto spec = r ? r->buildSpec(mgr, preview, id(kTexId), noLoad) : ThumbnailSpec{};
        check(spec.valid && spec.has_cpu_pixels, "texture spec has cpu pixels");
        check(spec.cpu_width == 64 && spec.cpu_height == 64, "texture spec dims 64x64");
        check(spec.rgba8 == texpix, "texture spec is lossless 1:1 copy of source");
    }

    // ── MESH: spec → 1 instance; blocking capture renders a lit cube ──
    {
        auto* r = registry.get(lux::asset::EAssetType::MESH);
        check(r != nullptr, "mesh renderer registered");
        const auto spec = r ? r->buildSpec(mgr, preview, id(kMeshId), noLoad) : ThumbnailSpec{};
        check(spec.valid && spec.instances.size() == 1, "mesh spec has 1 instance");
        check(spec.instances.size() == 1 && spec.instances[0].mesh_data != nullptr,
              "mesh spec carries mesh data");
        const auto bgra = captureSpec(mgr, session, preview, spec);
        check(!bgra.empty(), "mesh capture non-empty");
        const auto a = analyzeRaw(bgra);
        check(!a.uniform, "mesh thumbnail is non-uniform (cube rendered)");
        check(a.bright > 200, "mesh thumbnail has lit pixels");
        std::printf("    mesh bright=%zu\n", a.bright);
    }

    // ── MODEL: spec → >=1 instance; capture renders ──
    {
        auto* r = registry.get(lux::asset::EAssetType::MODEL);
        check(r != nullptr, "model renderer registered");
        const auto spec = r ? r->buildSpec(mgr, preview, id(kModelId), noLoad) : ThumbnailSpec{};
        check(spec.valid && !spec.instances.empty(), "model spec non-empty");
        const auto bgra = captureSpec(mgr, session, preview, spec);
        const auto a = analyzeRaw(bgra);
        check(!a.uniform, "model thumbnail is non-uniform");
        check(a.bright > 200, "model thumbnail has lit pixels");
    }

    // ── MATERIAL: spec → resident sphere + graph material id; capture ──
    {
        auto* r = registry.get(lux::asset::EAssetType::MATERIAL);
        check(r != nullptr, "graph-material renderer registered");
        const auto spec = r ? r->buildSpec(mgr, preview, id(kMatId), noLoad) : ThumbnailSpec{};
        check(spec.valid && spec.instances.size() == 1, "graph-material spec has 1 instance");
        check(spec.instances.size() == 1 && spec.instances[0].mesh_data == nullptr,
              "graph-material spec uses the resident sphere (null mesh_data)");
        check(spec.instances.size() == 1 && !spec.instances[0].graph_material_id.is_nil(),
              "graph-material spec carries a material id");
        const auto bgra = captureSpec(mgr, session, preview, spec);
        const auto a = analyzeRaw(bgra);
        check(!a.uniform, "graph-material thumbnail is non-uniform (sphere rendered)");
        check(a.bright > 200, "graph-material thumbnail has lit pixels");
        std::printf("    graph-material bright=%zu\n", a.bright);
    }

    sync->requestStop();
    server_thread.join();

    std::printf("=== thumbnail_render_gpu_test %s (fails=%d) ===\n",
                g_fail == 0 ? "PASSED" : "FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
