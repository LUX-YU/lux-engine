// ============================================================================
//  thumbnail_render_gpu_test.cpp
//
//  End-to-end test of the editor thumbnail subsystem on a real GPU — the
//  SceneRuntime-hosted pipeline (装配归属 ADR 工作线三批 1):
//
//    ThumbnailService::initialize  → offscreen target + private SceneRuntime
//                                    (manual preview pack + previewProfile()
//                                     + key-light/camera ENTITIES)
//    requestThumbnail(id, type)    → job = world entities wearing
//                                    MeshComponent{mesh_id, material_id}
//    tick() loop                   → resolver uploads, mesh subsystem builds the
//                                    instance, MeshInstanceReadyComponent arms,
//                                    readbackTargetAsync → encode → display tex
//
//  Verified per type:
//    TEXTURE  → CPU pixels ride through (provider check: lossless 1:1), service
//               produces a Ready PNG set;
//    MESH     → builtin cube renders lit + non-uniform;
//    MODEL    → model (cube + white material) renders lit + non-uniform;
//    MATERIAL → builtin sphere wearing the white graph material renders;
//    MATERIAL_INSTANCE → the same sphere inherits the parent material and
//                        produces non-black pixels through the instance path.
//
//  Self-hosts the render server thread + registerStandardRenderFeatures (the
//  same plan the hosts replay). Builtins come from EditorBuiltins::registerInto
//  (falls back to programmatic geometry when the on-disk content is missing).
//
//  Self-checking: 0 = PASS, 1 = FAIL, 0 (skip) if no Vulkan device.
//  NOT in ctest — build green + manual run is the gate.
// ============================================================================

#include <lux/engine/editor/app/LuxEditor.hpp>   // EditorRenderInfra
#include <lux/engine/runtime/render/scene/StandardFeaturePlan.hpp>   // registerStandardRenderFeatures
#include <lux/engine/editor/thumbnail/ThumbnailService.hpp>
#include <lux/engine/editor/thumbnail/ThumbnailSpecProvider.hpp>
#include <lux/engine/editor/thumbnail/ThumbnailSet.hpp>
#include <lux/engine/authoring/assets/FlowGraphSerDeser.hpp>
#include <lux/engine/editor/thumbnail/ImageCodec.hpp>
#include <lux/engine/editor/content/EditorBuiltins.hpp>
#include <lux/engine/editor/import/AssetImporter.hpp>
#include <lux/engine/authoring/assets/LooseAssetProvider.hpp>
#include "thumbnail/MaterialPreviewHost.hpp"

#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/resource/asset/AssetHeaderProbe.hpp>
#include <lux/engine/resource/asset/storage/AssetVfs.hpp>
#include <lux/engine/resource/asset/texture/TextureAsset.hpp>
#include <lux/engine/resource/asset/model/ModelAsset.hpp>
#include <lux/engine/resource/asset/material/MaterialAsset.hpp>
#include <lux/engine/resource/asset/material/MaterialInstanceAsset.hpp>
#include <lux/engine/authoring/assets/material/MaterialGraph.hpp>
#include <lux/engine/authoring/assets/material/Nodes.hpp>
#include <lux/engine/toolchain/asset/material/MaterialGraphCompiler.hpp>
#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/ecs/render/RenderResourceEvents.hpp>
#include <lux/engine/meta/Meta.hpp>

#include <lux/engine/runtime/render/scene/ResidencyAssembly.hpp>          // 驻留三件套(裁决二)

#include <lux/engine/description/Texture.hpp>

#include <lux/engine/runtime/render/scene/testing/AsyncTestServices.hpp>
#include <lux/engine/events/DomainEvents.hpp>
#include <lux/engine/runtime/frame/FrameCoordinator.hpp>
#include <lux/engine/runtime/frame/MainCloseDriver.hpp>

#include <lux/engine/function/render/client/RenderFrameSession.hpp>
#include <lux/engine/function/render/client/RenderControlSession.hpp>
#include <lux/engine/function/render/client/RenderUploadSession.hpp>
#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/function/render/client/RenderProtocol.hpp>

#include <lux/engine/window/LuxWindow.hpp>

#include <uuid.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
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
    const auto exts = lux::window::LuxWindow::requiredVulkanInstanceExtensions();
    return {exts.begin(), exts.end()};
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

// Decode the finished set's biggest PNG into RGBA8 for pixel checks.
static std::vector<std::byte> decodeBest(const ThumbnailSet& set, std::uint32_t size)
{
    const ThumbnailImage* best = set.best(size, size);
    if (!best) return {};
    auto decoded = decodePngRgba8(best->bytes);
    return decoded ? std::move(decoded->rgba8) : std::vector<std::byte>{};
}

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // unbuffered: progress survives a hang
    std::printf("=== thumbnail_render_gpu_test (SceneRuntime pipeline) ===\n");

    auto channel = lux::render::RenderFrameChannel<>::create();
    auto control_channel = lux::render::RenderControlChannel<>::create();
    auto upload_channel = lux::render::RenderUploadChannel<>::create();
    auto sync    = std::make_shared<lux::render::RenderChannelSync>();
    auto exts    = vkExts();

    lux::window::LuxWindow window(64, 64, "thumbnail_render_gpu_test");

    lux::meta::meta_module_init();
    lux::ecs::ComponentTypeCatalog components;
    check(
        lux::ecs::registerGeneratedComponents(components).has_value(),
        "generated component schemas registered"
    );

    std::atomic<bool> ready{false}, failed{false};
    std::atomic<int> validation_errors{0};
    std::atomic<int> output_not_consumed_warnings{0};
    lux::editor::EditorRenderInfra infra;   // filled on the server thread (same-thread contract)
    std::thread server_thread([&]
    {
        lux::render::GeneralRenderServer server(
            channel, control_channel, upload_channel, sync);
        lux::render::ServerConfig cfg;
        cfg.instance_extensions = exts;
        cfg.enable_validation = true;
        cfg.validation_error_counter = &validation_errors;
        cfg.validation_message_sink =
            [&output_not_consumed_warnings](
                std::uint32_t,
                std::string_view message
            )
            {
                if (message.find("Shader-OutputNotConsumed")
                    != std::string_view::npos)
                    output_not_consumed_warnings.fetch_add(
                        1,
                        std::memory_order_relaxed
                    );
            };
        if (auto r = server.init(std::move(cfg)); !r) { failed = true; ready = true; return; }
        if (auto r = server.attachToWindow(window); !r) { failed = true; ready = true; return; }
        // Same feature plan the editor uses — the thumbnail SceneRuntime
        // assembles its preview set from this catalog via previewProfile().
        lux::runtime::registerStandardRenderFeatures(
            server, infra.feature_catalog, infra.feature_plan);
        ready = true;
        while (server.tick()) {}
    });
    while (!ready.load()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (failed.load())
    {
        std::printf("Server init failed (no Vulkan?). Skipping.\n");
        sync->requestStop(); server_thread.join();
        return 0;
    }

    int rc = 0;
    {
        lux::render::RenderFrameSession session(channel, sync);
        lux::render::RenderControlSession control(control_channel, sync);
        lux::render::RenderUploadSession upload(upload_channel, sync);
        infra.control = &control;

        // ── Assets: editor builtins (sphere / cube / white + preview grey
        //    materials — programmatic fallback when disk content is missing)
        //    + a checker texture + a model wrapping cube+white. ─────────────
        // Match the editor product seam exactly. In particular, imported
        // graph-material v3 files are Authoring documents: the authoring
        // catalog promotes their inline graph into the v4 auxiliary payload
        // while the runtime-only catalog correctly rejects that old image.
        lux::asset::AssetManager mgr{
            lux::authoring::authoringAssetCodecCatalog()};
        {
            EditorBuiltins builtins;
            check(builtins.registerInto(mgr), "EditorBuiltins::registerInto");
        }
        auto id = [](const char* s){ return uuids::uuid::from_string(s).value(); };
        const auto cube_id = id(lux::engine::content::kBuiltinCubeMeshIdStr);
        const auto sphere_id = id(lux::engine::content::kBuiltinSphereMeshIdStr);
        const auto white_id = id(
            lux::engine::content::kBuiltinWhitePbrMaterialIdStr);

        const char* kTexId   = "00000000-0000-4000-8000-a00000000003";
        const char* kModelId = "00000000-0000-4000-8000-a00000000004";
        const char* kMaterialInstanceId =
            "00000000-0000-4000-8000-a00000000005";
        const char* kBlackMaterialId =
            "00000000-0000-4000-8000-a00000000006";
        const char* kBlackMaterialInstanceId =
            "00000000-0000-4000-8000-a00000000007";

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
            auto built = lux::rdesc::Texture::copyOf(ti, texpix);
            if (!built) return 11;
            auto tex = std::make_unique<lux::rdesc::Texture>(
                std::move(*built));
            auto info  = std::make_unique<lux::asset::AssetInfo>();
            info->id   = id(kTexId);
            info->type = lux::asset::EAssetType::TEXTURE;
            auto a = std::make_unique<lux::asset::TextureAsset>(std::move(info));
            a->setData(std::move(tex));
            mgr.registerAsset(std::move(a));
        }
        {   // model referencing the builtin cube mesh + white graph material
            auto info  = std::make_unique<lux::asset::AssetInfo>();
            info->id   = id(kModelId);
            info->type = lux::asset::EAssetType::MODEL;
            auto a = std::make_unique<lux::asset::ModelAsset>(std::move(info));
            a->addMeshAssetId(cube_id);
            a->addMaterialAssetId(white_id);
            mgr.registerAsset(std::move(a));
        }
        {   // material instance inheriting the builtin white graph material
            auto data = std::make_unique<lux::asset::MaterialInstanceData>();
            data->parent_material_id = white_id;
            auto info  = std::make_unique<lux::asset::AssetInfo>();
            info->id   = id(kMaterialInstanceId);
            info->type = lux::asset::EAssetType::MATERIAL_INSTANCE;
            auto a = std::make_unique<lux::asset::MaterialInstanceAsset>(
                std::move(info)
            );
            a->setData(std::move(data));
            mgr.registerAsset(std::move(a));
        }
        {   // deliberately black material + instance: exercises PreviewGrey fallback
            lux::rdesc::MaterialGraph black_graph;
            black_graph.shading_model =
                lux::rdesc::EMaterialShadingModel::Unlit;
            auto constant = std::make_unique<lux::rdesc::ConstantNode>();
            constant->value_type = lux::rdesc::EMatValueType::Vec3;
            const auto constant_id = black_graph.addNode(std::move(constant));
            const auto output_id = black_graph.addNode(
                std::make_unique<lux::rdesc::OutputSurfaceNode>()
            );
            black_graph.connect(
                constant_id,
                0,
                output_id,
                static_cast<std::uint32_t>(
                    lux::rdesc::EMaterialAttribute::BaseColor
                )
            );
            auto payload = lux::toolchain::compileGraphToPayload(
                black_graph,
                {}
            );
            if (!payload)
                return 12;

            auto material_info = std::make_unique<lux::asset::AssetInfo>();
            material_info->id = id(kBlackMaterialId);
            material_info->type = lux::asset::EAssetType::MATERIAL;
            mgr.registerAsset(std::make_unique<lux::asset::MaterialAsset>(
                std::move(material_info),
                std::make_unique<lux::asset::MaterialData>(
                    std::move(*payload)
                )
            ));

            auto instance_data =
                std::make_unique<lux::asset::MaterialInstanceData>();
            instance_data->parent_material_id = id(kBlackMaterialId);
            auto instance_info = std::make_unique<lux::asset::AssetInfo>();
            instance_info->id = id(kBlackMaterialInstanceId);
            instance_info->type =
                lux::asset::EAssetType::MATERIAL_INSTANCE;
            auto instance =
                std::make_unique<lux::asset::MaterialInstanceAsset>(
                    std::move(instance_info)
                );
            instance->setData(std::move(instance_data));
            mgr.registerAsset(std::move(instance));
        }

        // Optional field-reproduction input. This keeps the regular fixture
        // hermetic, while allowing the exact project material + instance that
        // failed in AssetBrowser to ride the same ThumbnailService in a GPU
        // test:
        //   thumbnail_render_gpu_test <material.luxasset> <instance.luxasset>
        std::optional<std::pair<lux::asset::asset_id_t,
                                lux::asset::asset_id_t>> field_materials;
        if (argc == 3)
        {
            const auto material = lux::asset::readAssetHeader(argv[1]);
            const auto instance = lux::asset::readAssetHeader(argv[2]);
            check(
                !material.id.is_nil() &&
                    material.magic == lux::asset::asset_magic_number_of<
                        lux::asset::EAssetType::MATERIAL>::value,
                "field graph-material header is valid"
            );
            check(
                !instance.id.is_nil() &&
                    instance.magic == lux::asset::asset_magic_number_of<
                        lux::asset::EAssetType::MATERIAL_INSTANCE>::value,
                "field material-instance header is valid"
            );
            if (!material.id.is_nil() && !instance.id.is_nil())
            {
                field_materials = std::pair{
                    material.id,
                    instance.id
                };
                // Imported graph materials live below
                // Content/Models/<model>/ while instances commonly live in
                // Content/Materials/. Derive the product mount root instead of
                // assuming both field files are exactly one directory deep.
                auto content_root =
                    std::filesystem::path{argv[1]}.parent_path();
                while (!content_root.empty() &&
                       content_root.filename() != "Content")
                {
                    content_root = content_root.parent_path();
                }
                auto provider =
                    std::make_shared<lux::authoring::LooseAssetProvider>(
                        content_root
                    );
                check(
                    provider->rescan() != 0u,
                    "field project loose provider indexes assets"
                );
                auto vfs = std::make_shared<lux::asset::AssetVfs>();
                check(
                    vfs->mount({"/Game", std::move(provider), 0})
                        != lux::asset::kInvalidMountId,
                    "field project VFS mounts /Game"
                );
                mgr.setVfs(std::move(vfs));
                auto manager = std::shared_ptr<lux::asset::AssetManager>(
                    &mgr,
                    [](lux::asset::AssetManager*) noexcept {}
                );
                const auto mounted = lux::editor::registerContentFolder(
                    content_root,
                    std::move(manager),
                    lux::editor::ELoadMode::Shells
                );
                check(
                    mounted != 0u,
                    "field project shell closure mounts"
                );
            }
        }

        // ── Provider-level spec checks (pure CPU, new id-shaped contract) ──
        {
            auto registry = makeDefaultThumbnailSpecProviders();

            auto* tr = registry.get(lux::asset::EAssetType::TEXTURE);
            check(tr != nullptr, "texture provider registered");
            const auto tspec = tr
                ? tr->buildSpec(mgr, sphere_id, id(kTexId))
                : ThumbnailSpec{};
            check(tspec.valid && tspec.has_cpu_pixels, "texture spec has cpu pixels");
            check(tspec.cpu_width == 64 && tspec.cpu_height == 64, "texture spec dims 64x64");
            check(tspec.rgba8 == texpix, "texture spec is lossless 1:1 copy of source");

            auto* mr = registry.get(lux::asset::EAssetType::MESH);
            check(mr != nullptr, "mesh provider registered");
            const auto mspec = mr
                ? mr->buildSpec(mgr, sphere_id, cube_id)
                : ThumbnailSpec{};
            check(mspec.valid && mspec.instances.size() == 1, "mesh spec has 1 instance");
            check(!mspec.instances.empty()
                      && mspec.instances[0].mesh_asset_id == cube_id
                      && mspec.instances[0].material_asset_id.is_nil(),
                  "mesh spec carries the mesh id + nil material (PreviewGrey substitution is the service's)");

            auto* gr = registry.get(lux::asset::EAssetType::MATERIAL);
            check(gr != nullptr, "graph-material provider registered");
            const auto gspec = gr
                ? gr->buildSpec(mgr, sphere_id, white_id)
                : ThumbnailSpec{};
            check(gspec.valid && gspec.instances.size() == 1, "graph-material spec has 1 instance");
            check(!gspec.instances.empty()
                      && gspec.instances[0].mesh_asset_id == sphere_id
                      && gspec.instances[0].material_asset_id == white_id,
                  "graph-material spec = builtin sphere + the material id");

            auto* ir = registry.get(lux::asset::EAssetType::MATERIAL_INSTANCE);
            check(ir != nullptr, "material-instance provider registered");
            const auto ispec = ir
                ? ir->buildSpec(
                    mgr,
                    sphere_id,
                    id(kMaterialInstanceId)
                )
                : ThumbnailSpec{};
            check(
                ispec.valid && ispec.instances.size() == 1,
                "material-instance spec has 1 instance"
            );
            check(
                !ispec.instances.empty()
                    && ispec.instances[0].mesh_asset_id == sphere_id
                    && ispec.instances[0].material_asset_id
                        == id(kMaterialInstanceId),
                "material-instance spec = builtin sphere + the instance id"
            );

            auto* dr = registry.get(lux::asset::EAssetType::MODEL);
            check(dr != nullptr, "model provider registered");
            const auto dspec = dr
                ? dr->buildSpec(mgr, sphere_id, id(kModelId))
                : ThumbnailSpec{};
            check(dspec.valid && dspec.instances.size() == 1, "model spec has 1 instance");
            check(!dspec.instances.empty()
                      && dspec.instances[0].mesh_asset_id == cube_id
                      && dspec.instances[0].material_asset_id == white_id,
                  "model spec resolves sub-mesh + material ids");
        }

        // ── The service end-to-end: private SceneRuntime + job entities ────
        lux::runtime::testing::AsyncTestServices async(mgr, upload, sync);
        check(async.valid(), "async test services assembled");
        lux::events::DomainEvents close_events;
        auto& close_pump = close_events.createPump("thumbnail-close");
        lux::runtime::FrameCoordinator frame_coordinator(
            session,
            control,
            close_pump,
            async.runtime()
        );
        lux::runtime::MainCloseDriver close_driver(
            frame_coordinator,
            async.runtime()
        );
        infra.events = &close_events;
        infra.frame_pump = &close_pump;
        infra.close_driver = &close_driver;
        infra.components = &components;
        infra.upload = async.uploadClient();
        // 驻留三件套(裁决二):测试宿主也自己建一份,经 infra 指针分发。
        std::vector<lux::ecs::RenderResourceFailed> residency_failures;
        lux::runtime::ResidencyAssembly residency(
            control,
            async.uploadClient(),
            mgr,
            infra.feature_catalog,
            async.assetClient(),
            async.runtime(),
            [&residency_failures](const lux::ecs::RenderResourceFailed& failure)
            {
                residency_failures.push_back(failure);
                std::fprintf(
                    stderr,
                    "[ThumbnailTest] residency failure domain=%u stage=%u: %s\n",
                    static_cast<unsigned>(failure.domain),
                    static_cast<unsigned>(failure.failure.stage),
                    failure.failure.reason.c_str()
                );
            }
        );
        infra.residency = &residency;
        ThumbnailService ts(
            mgr,
            session,
            infra,
            async.assetClient(),
            async.runtime()
        );
        check(ts.initialize(256), "ThumbnailService::initialize (SceneRuntime bring-up)");
        MaterialPreviewHost material_preview(
            mgr,
            session,
            infra,
            async.assetClient(),
            async.runtime()
        );
        check(
            material_preview.initialize(256),
            "MaterialPreviewHost::initialize (SceneRuntime bring-up)"
        );

        // Drive editor-shaped frames until @p pred (or a frame budget runs out).
        const auto drive = [&](int frames, const auto& pred)
        {
            for (int f = 0; f < frames; ++f)
            {
                if (pred()) return true;
                auto frame = frame_coordinator.begin();
                if (!frame) return false;
                frame.beforeMain(
                    [&]
                    {
                        ts.tick();
                        material_preview.tick();
                    }
                );
                frame.beforeEvents([] {});
                frame.record([] {});
            }
            return pred();
        };

        const auto waitThumb = [&](const lux::asset::asset_id_t& asset,
                                   lux::asset::EAssetType type, const char* label)
            -> const ThumbnailSet*
        {
            (void)ts.requestThumbnail(asset, type);
            // Budget: generously above the service's own 600-frame watchdog so a
            // stuck job FAILS (readySet stays null) instead of hanging the test.
            (void)drive(1000, [&]{ return ts.readySet(asset) != nullptr; });
            const auto* set = ts.readySet(asset);
            if (set == nullptr)
            {
                const auto snapshot = residency.closeSnapshot();
                std::fprintf(
                    stderr,
                    "[ThumbnailTest] residency rows loading=%zu uploading=%zu "
                    "ready=%zu failed=%zu mesh_replies=%zu material_replies=%zu\n",
                    snapshot.rows_loading,
                    snapshot.rows_uploading,
                    snapshot.rows_ready,
                    snapshot.rows_failed,
                    snapshot.mesh_replies,
                    snapshot.material_replies
                );
            }
            else
            {
                check(
                    ts.requestThumbnail(asset, type) != ImTextureID{0},
                    "ready thumbnail publishes an ImTextureID"
                );
            }
            std::printf("  [%s] %s\n", set ? "ready" : "MISSING", label);
            return set;
        };

        // MESH — builtin cube in PreviewGrey.
        if (const auto* set = waitThumb(cube_id, lux::asset::EAssetType::MESH, "mesh thumbnail"))
        {
            const auto px = decodeBest(*set, 128);
            check(!px.empty(), "mesh thumbnail decodes");
            const auto a = analyzeRaw(px);
            check(!a.uniform, "mesh thumbnail is non-uniform (cube rendered)");
            check(a.bright > 50, "mesh thumbnail has lit pixels");
            std::printf("    mesh bright=%zu\n", a.bright);
        }
        else check(false, "mesh thumbnail became ready");

        // MODEL — cube + white material.
        if (const auto* set = waitThumb(id(kModelId), lux::asset::EAssetType::MODEL, "model thumbnail"))
        {
            const auto px = decodeBest(*set, 128);
            const auto a  = analyzeRaw(px);
            check(!px.empty() && !a.uniform, "model thumbnail is non-uniform");
            check(a.bright > 50, "model thumbnail has lit pixels");
        }
        else check(false, "model thumbnail became ready");

        // MATERIAL — builtin sphere wearing the white graph material.
        if (const auto* set = waitThumb(white_id, lux::asset::EAssetType::MATERIAL, "graph-material thumbnail"))
        {
            const auto px = decodeBest(*set, 128);
            const auto a  = analyzeRaw(px);
            check(!px.empty() && !a.uniform, "graph-material thumbnail is non-uniform (sphere rendered)");
            check(a.bright > 50, "graph-material thumbnail has lit pixels");
            std::printf("    graph-material bright=%zu\n", a.bright);
        }
        else check(false, "graph-material thumbnail became ready");

        // MATERIAL_INSTANCE — the same parent shader/PSO, with a distinct
        // effective material slot produced by the instance resolver.
        if (const auto* set = waitThumb(
                id(kMaterialInstanceId),
                lux::asset::EAssetType::MATERIAL_INSTANCE,
                "material-instance thumbnail"
            ))
        {
            const auto px = decodeBest(*set, 128);
            const auto a  = analyzeRaw(px);
            check(
                !px.empty() && !a.uniform,
                "material-instance thumbnail is non-uniform (sphere rendered)"
            );
            check(
                a.bright > 50,
                "material-instance thumbnail has lit pixels"
            );
            std::printf("    material-instance bright=%zu\n", a.bright);
        }
        else check(false, "material-instance thumbnail became ready");

        // A legitimately black material produces the same empty first capture
        // as a broken/stale material. The retry must bind PreviewGrey, not a nil
        // material handle (which the render server rejects as status 2).
        if (const auto* set = waitThumb(
                id(kBlackMaterialInstanceId),
                lux::asset::EAssetType::MATERIAL_INSTANCE,
                "black material-instance fallback thumbnail"
            ))
        {
            const auto px = decodeBest(*set, 128);
            const auto a = analyzeRaw(px);
            check(
                !px.empty() && !a.uniform && a.bright > 50,
                "empty material capture retries with PreviewGrey"
            );
        }
        else check(false, "black material-instance fallback became ready");

        if (field_materials)
        {
            if (const auto* set = waitThumb(
                    field_materials->first,
                    lux::asset::EAssetType::MATERIAL,
                    "field graph-material thumbnail"
                ))
            {
                const auto px = decodeBest(*set, 128);
                const auto a = analyzeRaw(px);
                check(
                    !px.empty() && !a.uniform && a.bright > 50,
                    "field graph-material produces a visible thumbnail"
                );
            }
            else check(false, "field graph-material thumbnail became ready");

            if (const auto* set = waitThumb(
                    field_materials->second,
                    lux::asset::EAssetType::MATERIAL_INSTANCE,
                    "field material-instance thumbnail"
                ))
            {
                const auto px = decodeBest(*set, 128);
                const auto a = analyzeRaw(px);
                check(
                    !px.empty() && !a.uniform && a.bright > 50,
                    "field material-instance produces a visible thumbnail"
                );
            }
            else check(false, "field material-instance thumbnail became ready");
        }

        // TEXTURE — CPU path through the same job machine (no scene render).
        if (const auto* set = waitThumb(id(kTexId), lux::asset::EAssetType::TEXTURE, "texture thumbnail"))
        {
            const auto px = decodeBest(*set, 64);
            const auto a  = analyzeRaw(px);
            check(!px.empty() && !a.uniform, "texture thumbnail is non-uniform");
        }
        else check(false, "texture thumbnail became ready");

        // LIVE MATERIAL PREVIEW — register a temporary copy of the baked
        // builtin material, patch the persistent sphere, and require the
        // standard resolver + instance path to adopt that exact replacement.
        const auto* white_asset = mgr.fetchAsset(white_id);
        const auto* white_material = white_asset != nullptr
            ? white_asset->as<lux::asset::MaterialAsset>()
            : nullptr;
        check(
            white_material != nullptr && white_material->data() != nullptr,
            "builtin material payload available for live preview"
        );
        if (white_material != nullptr && white_material->data() != nullptr)
        {
            material_preview.setGraphContent(
                std::make_unique<lux::asset::MaterialData>(
                    *white_material->data()
                )
            );
            check(
                drive(600, [&] { return material_preview.contentReady(); }),
                "live material preview adopts the replacement material"
            );
        }

        // ── Teardown in editor order: GPU release while the thread serves,
        //    then stop, then drop the cache. ─────────────────────────────────
        check(material_preview.releaseGpu(), "material preview releases GPU state");
        material_preview.shutdown();
        (void)ts.releaseGpu();
        const auto residency_close = close_driver.close(residency);
        check(
            residency_close && residency_close->clean(),
            "residency closes before the render server stops"
        );
        sync->requestStop();
        server_thread.join();
        check(
            validation_errors.load(std::memory_order_relaxed) == 0,
            "validation reports zero errors"
        );
        check(
            output_not_consumed_warnings.load(std::memory_order_relaxed) == 0,
            "fixed mesh vertex superset emits no validation warning spam"
        );
        ts.shutdown();
        rc = g_fail == 0 ? 0 : 1;
    }

    std::printf("=== thumbnail_render_gpu_test %s (fails=%d) ===\n",
                rc == 0 ? "PASSED" : "FAILED", g_fail);
    return rc;
}
