#include <lux/engine/editor/app/LuxEditor.hpp>
#include <lux/engine/editor/app/EditorActions.hpp>
#include <lux/engine/editor/panels/AssetBrowser.hpp>
#include <lux/engine/editor/content/EditorBuiltins.hpp>
#include <lux/engine/editor/thumbnail/ThumbnailService.hpp>
#include <lux/engine/editor/scene/EditorScene.hpp>
#include <lux/engine/editor/panels/HierarchyPanel.hpp>
#include <lux/engine/editor/AssetRegistry.hpp>
#include <lux/engine/editor/EditorTextureCache.hpp>
#include "panels/MaterialGraphPanel.hpp"   // private (engine/editor/src) — material-graph node editor
#include "panels/FlowGraphPanel.hpp"       // private (engine/editor/src) — the first GraphKit host
#include "app/EditorMenuBar.hpp"           // private (engine/editor/src) — extracted main menu bar
#include "app/ImportController.hpp"        // private (engine/editor/src) — import subsystem (modal + ops)
#include "app/SceneController.hpp"         // private (engine/editor/src) — scene lifecycle + state
#include "app/ProjectController.hpp"       // private (engine/editor/src) — project lifecycle + state
#include "app/RenderThreadOrchestrator.hpp" // private (engine/editor/src) — render thread + server bring-up
#include <lux/engine/editor/panels/LuaConsole.hpp>
#include <lux/engine/editor/project/Project.hpp>
#include <lux/engine/editor/scene/Scene.hpp>

#include <lux/engine/input/ActionMapper.hpp>
#include <lux/engine/input/InputContext.hpp>
#include <lux/engine/input/InputContextStack.hpp>
#include <lux/engine/window/InputSnapshot.hpp>

#include <lux/engine/execution/EngineExecutor.hpp>   // engine async runtime

#include <lux/engine/asset/AssetManager.hpp>
#include <lux/engine/asset/ModelAsset.hpp>
#include <lux/engine/editor/content/ModelMaterialResolve.hpp>   // resolveModelSubmeshes
#include <lux/engine/asset/MaterialInstanceAsset.hpp>
#include <lux/engine/asset/MaterialInstanceSerDeser.hpp>

#include <filesystem>
#include <lux/pack/d3/world/components/AnimatorComponent.hpp>
#include <lux/engine/ecs/components/HierarchyComponent.hpp>
#include <lux/pack/d3/world/components/MeshComponent.hpp>
#include <lux/engine/ecs/components/NameComponent.hpp>
#include <lux/pack/d3/world/components/SkeletalMeshComponent.hpp>
#include <lux/pack/d3/world/components/TransformComponent.hpp>
#include <lux/pack/d3/world/components/WorldTransformComponent.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/ui/ImGuiLuxWidgets.hpp>
#include <lux/engine/editor/panels/InspectorPanel.hpp>
#include <lux/engine/editor/panels/SceneFeatureSettingPanel.hpp>
#include <lux/engine/ui/Panel.hpp>
#include <lux/engine/ui/SceneViewportPanel.hpp>
#include <lux/engine/ui/UIRenderServer.hpp>
#include <lux/engine/ui/UIRenderSession.hpp>
#include <lux/engine/ui/UISystem.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>
#include <lux/engine/window/LuxWindow.hpp>

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <optional>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>

namespace lux::editor
{
    namespace
    {
        // Stable, empty registry held in BSS so the InspectorPanel always
        // has a valid registry pointer to bind to — even when there's no
        // live scene (between unloadScene and the next loadScene). Without
        // this, `setRegistry` would have nothing safe to point at after
        // the EditorScene's World is destroyed.
        entt::registry& emptyRegistry() noexcept
        {
            static entt::registry r;
            return r;
        }
    } // namespace

    LuxEditor::LuxEditor(EditorConfig config)
        : config_(std::move(config))
    {
    }

    LuxEditor::~LuxEditor()
    {
        shutdown();
    }

    bool LuxEditor::init()
    {
        if (initialised_)
            return true;

        // 1. Reflection registry — drains the self-registering chain so any
        //    LUX_CLASS-annotated component built into the editor process is
        //    available to InspectorPanel from the first frame.
        lux::meta::meta_module_init();

        // 2. GLFW + window.
        glfw_ = std::make_unique<lux::window::GlfwRuntime>();
        if (!glfw_->valid())
        {
            std::fprintf(stderr, "[LuxEditor] glfwInit failed\n");
            return false;
        }

        window_ = std::make_unique<lux::window::LuxWindow>(
            config_.width, config_.height, config_.title
        );

        // 3. ImGui-driven UI system (creates the ImGui context).
        ui_system_ = std::make_unique<lux::ui::UISystem>(*window_);

        // 4. Process-wide AssetManager — survives scene swaps.
        asset_mgr_ = std::make_shared<lux::asset::AssetManager>();

        // 4a. Register editor builtin assets (cube / plane / white PBR /
        //     skybox texture) so demo entities and any drag-drop targets
        //     can refer to them via stable UUIDs.
        builtins_ = std::make_unique<EditorBuiltins>();
        if (!builtins_->registerInto(*asset_mgr_))
        {
            std::fprintf(stderr, "[LuxEditor] EditorBuiltins::registerInto failed\n");
            return false;
        }

        // 4b. Engine async runtime. Holds the CPU pool + main-thread sync point;
        //     its requestLoad orchestrates background open+decode of absent
        //     assets, injecting the decoded data into the registered shell at the
        //     next drainMain() (run() pumps it each frame). Created here so its
        //     requestLoad closure is available when loadScene builds an EditorScene.
        executor_ = std::make_unique<lux::exec::EngineExecutor>(*asset_mgr_);

        // 5. Default panels — created BEFORE the scene so the bring-up
        //    frame-pumping draws into a populated UI dockspace and the
        //    viewport panel exists in time for setTextureID at the end
        //    of bringUp.
        const auto browser_root = config_.project_root.empty()
            ? std::filesystem::current_path()
            : config_.project_root;

        asset_browser_ = std::make_unique<AssetBrowser>(
            "Asset Browser", asset_mgr_
        );

        asset_browser_->setWorkingDirectory(browser_root);

        // Panels read shared editor state from the StateRegistry each frame:
        // Inspector and Hierarchy both ensure + cache the `Selection`. Bind it
        // to the empty fallback registry until a scene loads. LuxEditor holds the
        // canonical shared_ptr so the Selection persists for the whole session.
        selection_ = states_.ensure<Selection>();
        selection_->select(&emptyRegistry(), entt::null);

        // Editor-wide content-change broadcast: producers (import / create-
        // instance / save-material / CLI --import) emit content_changed; the host
        // refreshes BOTH the browser (filesystem walk) and the registry (picker
        // index) in one place — so no producer can refresh only one and leave the
        // other stale (the bug each call site used to risk). Slots read the
        // members at emit time (guarded); held in panel_conns_ (disconnected
        // before events_ dies).
        events_ = states_.ensure<EditorEvents>();
        panel_conns_.add(events_->content_changed.connect(
            [this](const ContentChanged&)
            {
                if (asset_registry_) asset_registry_->refresh();
                if (asset_browser_)  asset_browser_->rescan();
            }));

        inspector_panel_ = std::make_unique<InspectorPanel>(
            "Inspector", states_
        );

        viewport_panel_ = std::make_unique<lux::ui::SceneViewportPanel>(
            "Scene Viewport", std::array<float, 2>{1280.f, 720.f}
        );

        hierarchy_panel_ = std::make_unique<HierarchyPanel>(
            "Hierarchy", states_
        );

        // Per-scene render-feature settings: lists the scene's features and
        // enumerates the selected feature's reflected param struct (spine:
        // Tonemap), committing edits via that feature's own op. setTarget() is
        // called once the render server is up + feature handles are known (after
        // createRenderInfra populates render_infra_).
        scene_feature_setting_panel_ = std::make_unique<SceneFeatureSettingPanel>(
            "SceneFeatureSetting"
        );

        // Selection is shared via the StateRegistry now: Hierarchy writes it on
        // click, Inspector reads it each frame — no panel-to-panel callback.

        // Inspector auto-discovers components from `ComponentTypeRegistry` —
        // every type marked `LUX_COMPONENT(...)` in its declaring module
        // (engine modules + future plugin DLLs) appears here without any
        // editor-side wiring. Display labels come from `display_name=...` on
        // the class annotation, or the short class name as a fallback.

        // Register every panel as a toggleable window (stable id -> live Panel*).
        // registerWindow adds it to the UISystem AND records it in panel_registry_,
        // so the Window menu + the per-project visibility config pick it up with no
        // further wiring. The ids are the config-facing keys (stable across renames
        // of the human-readable titles).
        registerWindow("viewport",       viewport_panel_.get());
        registerWindow("hierarchy",      hierarchy_panel_.get());
        registerWindow("inspector",      inspector_panel_.get());
        registerWindow("scene-settings", scene_feature_setting_panel_.get());
        registerWindow("asset-browser",  asset_browser_.get());

        // Material-graph node editor (compiles MatIR -> GLSL -> SPIR-V in-panel).
        auto mg_panel = std::make_unique<lux::editor::MaterialGraphPanel>("Material Graph");
        material_graph_panel_typed_ = mg_panel.get();   // cache concrete type, no later RTTI
        material_graph_panel_ = std::move(mg_panel);
        registerWindow("material-graph", material_graph_panel_.get());

        // FlowForge graph editor — the first GraphKit-hosted panel (the old
        // unwired NodeEditorPanel is retired; canvas/links/palette/undo all
        // come from the shared framework).
        flow_graph_panel_ = std::make_unique<lux::editor::FlowGraphPanel>("Flow Graph");
        registerWindow("flow-graph", flow_graph_panel_.get());

        // Lua script console — registers the generated gameplay bindings
        // (lux.* table) in its ctor; the Milestone-D demo surface.
        // (member is type-erased to ui::Panel — bind the asset API while we
        // still hold the concrete type)
        auto lua_console = std::make_unique<lux::editor::LuaConsole>("Script Editor");
        lua_console->setAssetManager(asset_mgr_); // lux.find_asset / asset_exists / find_assets
        lua_console_ = std::move(lua_console);
        registerWindow("script-editor", lua_console_.get());

        // Import subsystem — owns the Import Options modal + the model/texture
        // import operations; on confirm it defers the heavy import through the
        // action queue. Constructed before the menu bar, which paints its modal
        // each frame.
        import_controller_ = std::make_unique<ImportController>(*this);

        // Main menu bar — drawn inside `UISystem::newFrame` between
        // ImGui::NewFrame and the dockspace, so it sits at the top of the
        // OS window and is not nested in any panel scope. It paints the Import
        // Options modal at its tail.
        menu_bar_ = std::make_unique<EditorMenuBar>(*this);
        ui_system_->setMainMenuBarHook([this]{ menu_bar_->paint(); });

        // Toast overlay — painted after panels, above everything.
        ui_system_->setOverlayHook([this]{ toasts_.paint(); });

        // OS file drop → import. GLFW fires this on the main thread during
        // pollEvents(); we only enqueue, so the heavy import runs from the
        // drained pending-action queue (outside the ImGui frame) like the
        // menu-driven path. One action per file so partial failures isolate.
        // The connection is stored (and disconnected in shutdown() before
        // window_ dies) — a discarded ScopedConnection would unsubscribe at
        // once.
        file_drop_conn_ = window_->on_file_drop.connect(
            [this](const lux::window::FileDropEvent& ev)
            {
                for (const auto& p : ev.paths)
                    pending_actions_.emplace_back(
                        [this, p]{ importExternalAsset(p); });
            });

        // 6. Render thread + comm channel + server bring-up. The orchestrator
        //    owns the channel / sync / thread + ready handshake; start() spawns
        //    the render thread, blocks until the server is up (or fails), and
        //    fills render_infra_ + imgui_ops_ (passed by reference) before it
        //    returns true.
        render_thread_orch_ = std::make_unique<RenderThreadOrchestrator>(
            *window_, config_, render_infra_, imgui_ops_);
        if (!render_thread_orch_->start())
            return false;   // orchestrator already logged the failure + joined

        // 7. Main-thread render session — uses the ops captured by the server.
        session_ = std::make_unique<lux::ui::UIRenderSession>(
            render_thread_orch_->channel(), render_thread_orch_->sync(), imgui_ops_);

        // Wire the rendering-settings panel now that the session + feature
        // handles are live (render_infra_ is populated by the time server_ready
        // flips). The panel pushes live param edits via the session.
        if (scene_feature_setting_panel_)
        {
            scene_feature_setting_panel_->setTarget(
                session_.get(), render_infra_.scene_id, &render_infra_.feature_registry);
            // Scene tab edits the current scene's SceneSettingsComponent in place
            // (returned as void* to keep the panel free of gameplay types). The
            // component is ensured (created if absent) on access; EditorScene's tick
            // dirty-applies edits to streaming + the render SpatialCull cull mirror.
            scene_feature_setting_panel_->setSceneSettingsAccessor(
                [this]() -> void*
                {
                    auto* scene = currentScene();
                    return (scene && scene->isLive())
                        ? static_cast<void*>(&scene->ensureSceneSettings())
                        : nullptr;
                });
        }

        // 7b. Thumbnail subsystem — build the resident PreviewScene now that
        //     the server is up and no editor frame is in flight (setup() is
        //     blocking). Wire it into the asset browser on success; on failure
        //     the browser silently falls back to procedural type glyphs.
        thumbnail_service_ = std::make_unique<ThumbnailService>(*asset_mgr_, *session_, *executor_);
        if (thumbnail_service_->initialize())
        {
            asset_browser_->setThumbnailService(thumbnail_service_.get());
        }
        else
        {
            std::fprintf(stderr,
                "[LuxEditor] ThumbnailService init failed; asset browser uses glyphs only\n");
        }

        // Project asset index — the editor's "find any asset" foundation. The
        // Inspector's asset-reference fields (material / mesh pickers + name
        // resolution), the material-graph SampleTexture picker, and future
        // global search all read it. Created unconditionally (the Inspector
        // material field is useful for builtin materials too, with or without
        // the material-graph backend). Scanned at openProject.
        asset_registry_ = std::make_unique<AssetRegistry>();
        inspector_panel_->setAssetRegistry(asset_registry_.get());
        inspector_panel_->setAssetManager(asset_mgr_.get()); // vfs pathOf tooltips

        // 7c. Live material-graph preview — a generalized PreviewScene (forward
        //     graph-frag override) brought up now (blocking setup(), no editor
        //     frame in flight). Driven each frame by material_preview_->tick();
        //     the MaterialGraphPanel displays + orbits it.
        material_preview_ = std::make_unique<PreviewScene>(*session_, executor_.get());
        // The live preview is DISPLAYED through an ImGui SceneView sentinel, so its
        // view must be a UI view (registered in the UI server's scene-view index) —
        // unlike the thumbnail PreviewScene, which is read back and stays a base
        // view. Pass an ImGuiProxy so setup() creates the view via addUIView.
        lux::ui::ImGuiProxy preview_proxy(*session_, session_->imguiOps());
        if (material_preview_->setup(512, &preview_proxy))
        {
            if (auto* mg = material_graph_panel_typed_)
                mg->setPreviewScene(material_preview_.get());
        }
        else
        {
            std::fprintf(stderr, "[LuxEditor] material preview setup failed\n");
            material_preview_.reset();
        }

        // Editor texture-upload cache. The cache uploads via the preview's
        // session, so its bindless indices are valid in the live preview.
        texture_cache_  = std::make_unique<EditorTextureCache>(*session_, asset_mgr_, *executor_);
        if (auto* mg = material_graph_panel_typed_)
        {
            mg->setAssetServices(asset_registry_.get(), texture_cache_.get(), asset_mgr_, events_.get());
            // UE-style: double-click any material in the browser -> open it in the
            // material graph editor (reopens from the baked graph).
            if (asset_browser_)
                panel_conns_.add(asset_browser_->activated.connect(
                    [mg](const AssetActivated& e)
                    {
                        if (e.type == lux::asset::EAssetType::MATERIAL)
                        {
                            mg->openAsset(e.id);
                            mg->setVisible(true);   // bring the editor into view
                        }
                    }));

            // UE-style: right-click a graph material -> author a Material Instance of
            // it (parent ref, no overrides yet). The instance shares the parent's
            // shader/PSO; its overrides are edited later (param/texture inspector).
            if (asset_browser_)
                panel_conns_.add(asset_browser_->create_instance_requested.connect(
                    [this](const CreateInstanceRequested& ev)
                    {
                        const auto& parent = ev.parent;
                        if (!asset_mgr_ || !currentProject() || parent.is_nil()) return;

                        std::string name = "MaterialInstance";
                        if (const auto* pinfo = asset_mgr_->queryInfo(parent); pinfo)
                            name = std::string(pinfo->display_name) + "_Inst";

                        auto data = std::make_unique<lux::asset::MaterialInstanceData>();
                        data->parent_material_id = parent;
                        auto asset = asset_mgr_->createAsset<lux::asset::MaterialInstanceAsset>(
                            std::move(data));
                        if (auto* mi = asset->mutableInfo())
                        {
                            const std::size_t n =
                                std::min(name.size(), sizeof(mi->display_name) - 1);
                            std::memcpy(mi->display_name, name.data(), n);
                            mi->display_name[n] = '\0';
                        }
                        const auto id = asset->id();
                        if (!asset_mgr_->registerAsset(std::move(asset))) return;

                        // Pick a free filename (X_Inst, X_Inst_1, …) so authoring two
                        // instances of one parent never clobbers an existing file.
                        const auto dir = currentProject()->contentRoot() / "Materials";
                        std::error_code mkec; std::filesystem::create_directories(dir, mkec);
                        std::filesystem::path dest;
                        for (int i = 0; ; ++i)
                        {
                            const std::string fn = name + (i == 0 ? "" : "_" + std::to_string(i));
                            dest = dir / (fn + ".luxasset");
                            std::error_code ec;
                            if (!std::filesystem::exists(dest, ec)) break;
                        }
                        lux::asset::MaterialInstanceSerDeser ser(asset_mgr_);
                        ser.exportAsLuxAsset(id, dest);

                        events_->content_changed.emit({});   // refresh browser + registry
                    }));
        }

        // 8. Viewport pick + resize forwarding is connected once in the
        //    SceneController ctor (its slots guard on the live scene), so there
        //    is no per-scene (re)wiring here anymore. The asset-drop path stays
        //    on the host (it calls spawnModelEntity / currentProject).
        //
        // M3 stage C: when the user drags an asset from the AssetBrowser
        // onto the viewport image, spawn / apply it. Currently only the
        // MODEL family is handled — everything else logs a hint and falls
        // through, so non-model drags don't silently no-op. The drop
        // position is ignored for now (model lands at world origin); a
        // future patch can ray-cast against an editor ground plane to drop
        // exactly where the cursor was.
        panel_conns_.add(viewport_panel_->asset_dropped.connect(
            [this](const lux::ui::ViewportAssetDropped& e)
            {
                const auto& payload = e.payload;
                if (!currentScene()) return;
                const auto type =
                    static_cast<lux::asset::EAssetType>(payload.asset_type);
                if (type != lux::asset::EAssetType::MODEL ||
                    payload.is_model == 0)
                {
                    std::fprintf(stderr,
                        "[LuxEditor] dropped asset '%s' (type=%d, is_model=%d) "
                        "into viewport — only MODEL drops spawn entities for "
                        "now; drag to Inspector once that lands for the others.\n",
                        payload.display_name,
                        static_cast<int>(payload.asset_type),
                        static_cast<int>(payload.is_model));
                    return;
                }
                // Reassemble the UUID from the raw bytes and spawn.
                std::array<uint8_t, 16> ub{};
                std::memcpy(ub.data(), payload.uuid_bytes, sizeof(ub));
                const lux::asset::asset_id_t id(ub);
                const auto spawned = spawnModelEntity(id);
                std::fprintf(stderr,
                    "[LuxEditor] asset-drop spawned entity %u from '%s'\n",
                    static_cast<uint32_t>(spawned), payload.display_name);
            }));

        // 9. Editor input layer — ActionMapper + a single default
        //    InputContext carrying the M2 viewport bindings (WASD for fly,
        //    LMB / RMB / MMB for camera modes + picking, F / End hotkeys).
        //    Owned by LuxEditor; ticked in run(). EditorScene reads it
        //    each frame for camera control. Built BEFORE the initial scene
        //    so EditorScene::tick can rely on it from frame 0.
        action_mapper_ = std::make_unique<lux::input::ActionMapper>();
        input_stack_   = std::make_unique<lux::input::InputContextStack>();
        actions::registerAll(*action_mapper_);
        if (auto* ctx = actions::editorContext())
            input_stack_->push(ctx);

        // 10. Initial scene is NOT brought up here. Caller (main.cpp / the
        //     File menu) decides which scene the editor should open first
        //     via `openProject` / `loadScene`. Auto-loading a demo scene
        //     inside init() would create a tiny window where the demo
        //     scene's async upload replies are still in flight while
        //     `closeProject` already starts tearing the scene down — those
        //     stale replies would then crash on a freed adapter/context.
        //     Forcing the bring-up to be an explicit caller decision
        //     eliminates that race by construction.

        // Scene controller — owns the live scene + its path and the scene
        // lifecycle operations. Constructed last: every subsystem it borrows
        // (session_, asset_mgr_, ui_system_, builtins_, render_infra_, states_,
        // executor_, selection_, viewport_panel_) is live by now.
        scene_controller_ = std::make_unique<SceneController>(
            session_.get(), asset_mgr_, ui_system_.get(), builtins_.get(),
            render_infra_, states_, executor_.get(), selection_,
            viewport_panel_.get());

        // Project controller — drives scene_controller_ on open/close, so it is
        // constructed right after it (and holds a SceneController&).
        project_controller_ = std::make_unique<ProjectController>(
            *scene_controller_, ui_system_.get(), asset_mgr_,
            asset_registry_.get(), asset_browser_.get(), panel_registry_);

        initialised_ = true;
        return true;
    }

    void LuxEditor::registerWindow(std::string id, lux::ui::Panel* panel,
                                   bool default_visible)
    {
        if (!ui_system_ || !panel)
            return;
        ui_system_->addPanel(panel);
        panel_registry_.add(std::move(id), panel, default_visible);
    }

    // Project accessor — forwards to the ProjectController (which owns the open
    // project). Null-guarded so it is safe even before the controller is
    // constructed at the end of init().
    Project* LuxEditor::currentProject() noexcept
    {
        return project_controller_ ? project_controller_->currentProject() : nullptr;
    }

    const Project* LuxEditor::currentProject() const noexcept
    {
        return project_controller_ ? project_controller_->currentProject() : nullptr;
    }

    void LuxEditor::addPanel(lux::ui::Panel* panel)
    {
        if (!panel)
            return;
        // External panels become toggleable too: derive a config id from the title
        // (lower-case, spaces -> '-'). Built-ins use explicit ids via registerWindow.
        std::string base;
        base.reserve(panel->title().size());
        for (char c : panel->title())
            base.push_back(c == ' '
                ? '-'
                : static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        if (base.empty())
            base = "panel";
        // The manifest keys windows by id (first-match wins), so the id must be
        // unique — disambiguate an empty/duplicate title with a numeric suffix.
        std::string id = base;
        for (int n = 2; panel_registry_.find(id) != nullptr; ++n)
            id = std::format("{}-{}", base, n);
        registerWindow(std::move(id), panel);
    }

    // Scene accessors — forward to the SceneController (which owns the live
    // scene + its path). Null-guarded so they are safe even before the
    // controller is constructed at the end of init().
    EditorScene* LuxEditor::currentScene() noexcept
    {
        return scene_controller_ ? scene_controller_->currentScene() : nullptr;
    }

    const EditorScene* LuxEditor::currentScene() const noexcept
    {
        return scene_controller_ ? scene_controller_->currentScene() : nullptr;
    }

    const std::filesystem::path& LuxEditor::currentScenePath() const noexcept
    {
        static const std::filesystem::path kEmpty;
        return scene_controller_ ? scene_controller_->currentScenePath() : kEmpty;
    }

    // ──────────────────────────────────────────────────────────────────
    //  Project management
    // ──────────────────────────────────────────────────────────────────

    // Project lifecycle — thin forwards to the ProjectController. Guarded
    // against a null controller because closeProject() runs from shutdown(),
    // which can fire after a failed init() (before the controller is built).
    bool LuxEditor::openProject(const std::filesystem::path& luxproject_file)
    {
        return project_controller_ && project_controller_->openProject(luxproject_file);
    }

    bool LuxEditor::newProject(const std::filesystem::path& root,
                                std::string_view              project_name)
    {
        return project_controller_ && project_controller_->newProject(root, project_name);
    }

    void LuxEditor::closeProject() noexcept
    {
        if (project_controller_) project_controller_->closeProject();
    }

    bool LuxEditor::saveProject()
    {
        return project_controller_ && project_controller_->saveProject();
    }

    // ──────────────────────────────────────────────────────────────────
    //  Scene file I/O
    // ──────────────────────────────────────────────────────────────────

    // Scene file I/O — thin forwards to the SceneController. openScene seeds the
    // bring-up name from the open project (if any).
    bool LuxEditor::openScene(const std::filesystem::path& luxscene_file)
    {
        // Seed the bring-up name from the open project (if any) — original rule:
        // project open ⇒ use its name (even if empty); no project ⇒ leave the
        // BringUpConfig default.
        std::optional<std::string_view> project_name;
        if (const auto* p = currentProject())
            project_name = std::string_view(p->manifest().name);
        return scene_controller_->openScene(luxscene_file, project_name);
    }

    bool LuxEditor::saveScene()
    {
        return scene_controller_->saveScene();
    }

    bool LuxEditor::saveSceneAs(const std::filesystem::path& luxscene_file)
    {
        return scene_controller_->saveSceneAs(luxscene_file);
    }

    // ──────────────────────────────────────────────────────────────────
    //  Command surface
    //
    //  Small seams the extracted EditorMenuBar / ImportDialog call back into.
    //  (The menu-bar tree and the Import Options modal now live in
    //  EditorMenuBar.cpp / ImportDialog.cpp; only their hooks into the editor
    //  state stay here.)
    // ──────────────────────────────────────────────────────────────────

    void LuxEditor::enqueue(std::function<void()> action)
    {
        // Heavy actions (scene / project swaps, imports) must run between
        // frames, outside the live ImGui frame — run() drains this queue at the
        // top of each loop iteration, where EditorScene tearDown/bringUp (which
        // pump render frames that re-enter ImGui) is safe.
        pending_actions_.emplace_back(std::move(action));
    }

    void* LuxEditor::nativeWindowHandle() const noexcept
    {
        // The editor drives native OS file dialogs (nativefiledialog-extended)
        // for its open / save / import flows; they are owner-modal to the editor
        // window, so they need its platform handle (HWND on Windows) to parent
        // them. Only Windows exposes one today (LuxWindow::win32Handle is
        // declared under __PLATFORM_WIN32__); elsewhere return nullptr and the
        // dialog opens unparented.
#ifdef __PLATFORM_WIN32__
        return window_ ? window_->win32Handle() : nullptr;
#else
        return nullptr;
#endif
    }

    void LuxEditor::importExternalAsset(const std::filesystem::path& source)
    {
        // Thin forward — the import subsystem (modal + ops) lives in
        // ImportController. Kept as a member so the menu / OS file-drop call
        // sites stay unchanged.
        import_controller_->importExternalAsset(source);
    }

    int LuxEditor::run()
    {
        if (!initialised_)
        {
            std::fprintf(stderr, "[LuxEditor] run() called before successful init()\n");
            return 1;
        }

        using clock = std::chrono::steady_clock;
        auto prev = clock::now();

        while (!window_->shouldClose() && !quit_requested_.load(std::memory_order_acquire))
        {
            const auto now = clock::now();
            float dt = std::chrono::duration<float>(now - prev).count();
            prev = now;
            // Clamp dt so a paused debugger does not produce a one-shot
            // multi-second tick that scrambles the orbit camera.
            if (dt > 0.1f) dt = 0.1f;

            // build the ImGui frame on the main thread.
            //    The viewport panel paints here and may invoke its resize
            //    callback (queues into the live scene).
            lux::window::LuxWindow::pollEvents();

            // Sample raw OS input AFTER pollEvents so the snapshot reflects
            // this frame's accumulated GLFW callbacks. The snapshot is fed
            // to ActionMapper below (after ImGui::NewFrame so we can ask
            // ImGui whether it is capturing keyboard / mouse for its own
            // text inputs and drag handles).
            lux::window::InputSnapshot snapshot = window_->captureInputSnapshot();

            // Drain the deferred-action queue BEFORE starting the next
            // ImGui frame. Menu callbacks (EditorMenuBar::paint) cannot
            // perform scene swaps inline because tearDown re-enters
            // ImGui::NewFrame via FramePumper; they enqueue here and
            // we execute at the safe in-between-frames boundary.
            // Swap the queue into a local so an action that itself
            // enqueues further work (e.g. an open->open chain) gets
            // its enqueued items processed next iteration, not in the
            // middle of this drain (which would invalidate iterators).
            if (!pending_actions_.empty())
            {
                auto actions = std::move(pending_actions_);
                pending_actions_.clear();
                for (auto& fn : actions)
                    if (fn) fn();
            }

            ui_system_->newFrame();

            // Tick the editor input layer. The host has to decide whether
            // each input category flows to gameplay (the ActionMapper) or
            // to ImGui (the panel UI). Three sources are folded together:
            //
            //   - ImGui::Image (the SceneViewportPanel's rendered texture)
            //     is itself an interactive item, so the moment the cursor
            //     enters the viewport, ImGui flips `io.WantCaptureMouse`
            //     to TRUE — even when the user is just trying to RMB-fly
            //     the camera. If we honored WantCaptureMouse blindly, the
            //     mapper would never see RMB and fly mode could never
            //     start. So: when the cursor is *over the viewport* we
            //     override ImGui and route mouse → gameplay.
            //   - Same logic for keyboard: WASD over the viewport means
            //     "drive the camera", not "fire ImGui hotkeys."
            //   - `ownsMouse()` is the sticky companion — once fly mode is
            //     running, the cursor is hidden + locked so `hovered`
            //     stops being meaningful; the ownership flag carries the
            //     "keep routing to gameplay" state across that gap.
            //   - When the cursor is in any other panel (Inspector text
            //     input, menu bar, etc.), ImGui's WantCapture* wins and
            //     gameplay sees nothing — that's the expected behavior.
            const auto& io           = ImGui::GetIO();
            const bool  over_view    = viewport_panel_ ? viewport_panel_->pointer().hovered : false;
            const bool  owns_mb      = viewport_panel_ ? viewport_panel_->ownsMouse()       : false;
            const bool  route_to_gp  = over_view || owns_mb;
            const bool  want_kb      = route_to_gp || !io.WantCaptureKeyboard;
            const bool  want_ms      = route_to_gp || !io.WantCaptureMouse;
            snapshot.keyboard_captured_by_ui = !want_kb;
            snapshot.mouse_captured_by_ui    = !want_ms;
            action_mapper_->update(snapshot, *input_stack_, dt, want_kb, want_ms);

            ImDrawData* draw_data = ImGui::GetDrawData();

            // drain prior-frame replies and flush any pending
            //    submission BEFORE asking the server for a new frame slot.
            //    Skipping this is fine for an empty editor but deadlocks
            //    once async addFeature / addMeshInstance replies overlap
            //    a new beginFrame. Pattern lifted from
            //    scene_view_integration_test.cpp:1093-1101.
            session_->pumpReplies();
            session_->submitFrame(true);

            if (!session_->beginFrame())
                continue;

            // Advance async thumbnail generation — MUST run with the frame OPEN
            // (it pushes render commands into the live builder and polls prior
            // replies via the pumpReplies above). Non-blocking: one
            // state-machine step per editor frame.
            if (thumbnail_service_)
                thumbnail_service_->tick();

            // Live material-graph preview — also frame-OPEN, non-blocking (push +
            // poll only). Always created at startup; null only if its blocking GPU
            // setup() failed (see init()).
            if (material_preview_)
                material_preview_->tick();

            // Editor texture-upload cache — frame-OPEN (drains queued createTexture2D
            // for SampleTexture picks; the reply caches the bindless handle).
            if (texture_cache_)
                texture_cache_->tick();

            // Scene-feature panel — frame-OPEN: push the commands its paint()
            // flagged this frame (Apply / Dump) + poll the dump reply. paint()
            // runs before beginFrame() so it cannot record commands itself.
            if (scene_feature_setting_panel_)
                scene_feature_setting_panel_->tickRender();

            // Run the engine executor's main-thread continuations: any async
            // asset load that finished its background open+decode injects the
            // decoded data into its registered shell HERE, BEFORE the scene tick
            // reads assets — so a freshly-streamed mesh/material is visible to the
            // RenderableSystem bridge + resolver this same frame.
            if (executor_)
                executor_->drainMain();

            // per-frame scene work (between beginFrame and
            //    submitFrame so the command builder is live).
            if (auto* scene = currentScene())
            {
                scene->processPendingResize();
                const auto cs = viewport_panel_->contentSize();
                scene->tick(dt, cs.x, cs.y, *action_mapper_);

                // M2: fly mode (RMB in viewport) flips the cursor into
                // raw-relative-motion mode so mouse-look is smooth and the
                // pointer doesn't wander off the panel. Track edge
                // transitions so we don't spam GLFW with the same input
                // mode every frame.
                const bool fly = scene->cameraWantsCursorCapture();
                if (fly != cursor_captured_)
                {
                    window_->hideCursor(fly);
                    window_->setRawMouseMotion(fly);
                    if (viewport_panel_) viewport_panel_->setOwnsMouse(fly);
                    cursor_captured_ = fly;
                }
            }

            // ship ImGui draw lists and submit the frame.
            session_->submitImGuiDrawData(lux::render::RenderSceneId{}, draw_data);
            session_->submitFrame();
        }

        shutdown();
        return 0;
    }

    void LuxEditor::requestQuit() noexcept
    {
        quit_requested_.store(true, std::memory_order_release);
    }

    // -------------------------------------------------------------------------
    entt::entity LuxEditor::spawnModelEntity(lux::asset::asset_id_t model_id)
    {
        auto* scene = currentScene();
        if (!scene || !asset_mgr_) return entt::null;

        const auto* model = asset_mgr_->fetchAssetAs<lux::asset::ModelAsset>(model_id);
        if (!model)
        {
            std::fprintf(stderr,
                "[LuxEditor::spawnModelEntity] no ModelAsset for the given id\n");
            return entt::null;
        }
        const auto& mesh_ids = model->meshAssetIds();
        if (mesh_ids.empty())
        {
            std::fprintf(stderr,
                "[LuxEditor::spawnModelEntity] model has no meshes\n");
            return entt::null;
        }

        auto& w = scene->world();

        // Resolve each sub-mesh's material + name from the model node tree (the
        // SAME walk + positional fallback the thumbnail service uses — see
        // resolveModelSubmeshes), so a spawned entity and its thumbnail wear the
        // same materials and each sub-mesh wears its REAL baked material.
        const lux::editor::ModelSubmeshResolve submesh =
            lux::editor::resolveModelSubmeshes(*model);
        const auto& mesh_name = submesh.name;
        const auto materialFor = [&](std::size_t i) -> lux::asset::asset_id_t
        {
            return i < submesh.material.size() ? submesh.material[i]
                                               : lux::asset::asset_id_t{};
        };

        const bool skinned = model->skeletonAssetId().has_value();
        const auto clip_id = model->animationClipAssetIds().empty()
            ? lux::asset::asset_id_t{}
            : model->animationClipAssetIds().front();

        // Attach the renderable component(s) for sub-mesh @p i onto entity @p e.
        auto attachMesh = [&](entt::entity ent, std::size_t i)
        {
            if (skinned)
            {
                auto& smc = w.emplace<lux::pack::SkeletalMeshComponent>(ent);
                smc.mesh_asset_id     = mesh_ids[i];
                smc.skeleton_asset_id = *model->skeletonAssetId();
                smc.material_asset_id = materialFor(i);

                auto& ac = w.emplace<lux::pack::AnimatorComponent>(ent);
                if (clip_id != lux::asset::asset_id_t{})
                {
                    ac.clip_asset_id = clip_id;
                    ac.paused        = false;
                    ac.loop          = true;
                }
            }
            else
            {
                auto& mc = w.emplace<lux::pack::MeshComponent>(ent);
                mc.mesh_asset_id     = mesh_ids[i];
                mc.material_asset_id = materialFor(i);
            }
        };

        const std::string root_name =
            (model->data() && !model->data()->name.empty())
                ? model->data()->name
                : std::string();

        // Single-mesh model → one flat entity (unchanged behaviour). Multi-mesh
        // → an empty transform ROOT grouping one child entity per sub-mesh, so
        // the model moves/selects as a unit and EVERY part renders. (Was: the
        // M3 MVP spawned meshes[0] only — plan §4.1 fan-out, now done.)
        if (mesh_ids.size() == 1)
        {
            auto e = w.createEntity();
            w.emplace<lux::pack::TransformComponent>(e);
            w.emplace<lux::pack::WorldTransformComponent>(e);
            w.emplace<lux::ecs::NameComponent>(e,
                lux::ecs::NameComponent{
                    root_name.empty()
                        ? std::format("Model {}", static_cast<uint32_t>(e))
                        : root_name });
            attachMesh(e, 0);
            selection_->selectEntity(e);
            return e;
        }

        auto root = w.createEntity();
        w.emplace<lux::pack::TransformComponent>(root);
        w.emplace<lux::pack::WorldTransformComponent>(root);
        w.emplace<lux::ecs::NameComponent>(root,
            lux::ecs::NameComponent{
                root_name.empty()
                    ? std::format("Model {}", static_cast<uint32_t>(root))
                    : root_name });

        for (std::size_t i = 0; i < mesh_ids.size(); ++i)
        {
            auto e = w.createEntity();
            w.emplace<lux::pack::TransformComponent>(e);
            w.emplace<lux::pack::WorldTransformComponent>(e);
            w.emplace<lux::ecs::HierarchyComponent>(e).parent = root;

            w.emplace<lux::ecs::NameComponent>(e,
                lux::ecs::NameComponent{
                    mesh_name[i].empty()
                        ? std::format("Mesh_{}", i)
                        : mesh_name[i] });

            attachMesh(e, i);
        }

        // Select the model root — Inspector / Hierarchy / gizmo read the shared
        // selection; clicking a sub-mesh in the viewport picks that child.
        selection_->selectEntity(root);
        return root;
    }

    void LuxEditor::shutdown() noexcept
    {
        // Tear the scene down BEFORE stopping the render thread — tearDown
        // pumps frames, which the render thread must still be serving.
        // closeProject() also flushes the per-project ImGui layout
        // while ui_system_ is still alive.
        closeProject();

        // Drop the controllers NOW — before the panels / session they borrow are
        // torn down below. SceneController holds viewport_conns_ (signal
        // connections into viewport_panel_'s picked/resized signals); if it
        // outlived viewport_panel_.reset() below, those ScopedConnections would
        // unsubscribe against a freed Signal at member-destruction (a UAF).
        // project_controller_ holds a SceneController&, so it must die first.
        project_controller_.reset();
        scene_controller_.reset();

        // Tear down the live material-preview scene's GPU resources while the render
        // thread is still serving frames. Its destructor (material_preview_.reset()
        // below) runs AFTER the thread stops, where issuing render commands asserts in
        // RenderClient::builder() (no live frame) — the editor-close crash. (The
        // thumbnail PreviewScene's own resources are reclaimed at device-destroy; its
        // destructor guards on the client still recording.)
        if (material_preview_)
            material_preview_->releaseGpu();

        // Stop + join the render thread now (window_ + session_ still alive; the
        // scene tearDown above pumped frames the thread was serving). The
        // orchestrator keeps the comm channel / sync alive until it is reset
        // below — AFTER session_, which borrows them.
        if (render_thread_orch_)
            render_thread_orch_->requestStop();

        // Drop the thumbnail cache AFTER the render thread has stopped: an
        // in-flight async readback writes into a job-owned dst buffer when its
        // fence signals, so that buffer must outlive the server. (The server's
        // ~Impl frees any still-pending readback GPU state WITHOUT the memcpy,
        // so a readback caught mid-flight at join can't touch freed memory.)
        if (thumbnail_service_)
            thumbnail_service_->shutdown();

        // EditorTextureCache spawns upload pipelines on the executor's async_scope; drain
        // them here while the session can still pumpReplies (asSender is not cancellable),
        // so the executor's later scope.on_empty() can't hang on a parked upload.
        if (texture_cache_)
            texture_cache_->shutdown();

        // Same for the live PreviewScene's content-swap pipeline (C4).
        if (material_preview_)
            material_preview_->shutdown();

        // Drop the file-drop subscription while the window (and its signal)
        // is still alive; the ScopedConnection stores a raw Signal* and would
        // otherwise unsubscribe against freed memory at member-destruction.
        file_drop_conn_.disconnect();

        // Same for the panel->host connections (AssetBrowser signals): drop them
        // while asset_browser_ (and its embedded signals) is still alive, before
        // the panels are reset below.
        panel_conns_.clear();

        // Tear down in reverse order of creation. The session must die
        // before the channel/sync it borrows from; panels before UISystem
        // because UISystem holds raw pointers to them.
        thumbnail_service_.reset();   // holds PreviewScene (borrows session_)
        material_preview_.reset();    // live PreviewScene (borrows session_)
        texture_cache_.reset();       // uploads via session_ -> reset before it
        asset_registry_.reset();
        session_.reset();
        viewport_panel_.reset();
        hierarchy_panel_.reset();
        inspector_panel_.reset();
        asset_browser_.reset();
        ui_system_.reset();
        builtins_.reset();
        // Stop the async runtime BEFORE asset_mgr_: its background pool + main
        // sync point reference the manager (open+decode, fetchAsset injection).
        // shutdown() cancels in-flight work, drains the main queue, and joins
        // the pool, so no worker can touch the manager after this returns. The
        // scene (and its copies of the requestLoad closure) was already dropped
        // by closeProject() above.
        executor_.reset();
        asset_mgr_.reset();
        window_.reset();
        glfw_.reset();

        // Drop the render orchestrator LAST — it owns the comm channel / sync
        // that session_ (reset above) borrowed, so they must outlive the session.
        render_thread_orch_.reset();

        initialised_ = false;
    }

} // namespace lux::editor
