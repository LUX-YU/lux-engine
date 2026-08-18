#include "app/EditorShell.hpp"

#include <lux/engine/editor/app/LuxEditor.hpp>   // host surface (services + verbs)
#include <lux/engine/authoring/project/Project.hpp>
#include <lux/engine/editor/scene/EditorScene.hpp>

#include <lux/engine/editor/panels/AssetBrowser.hpp>
#include <lux/engine/editor/panels/HierarchyPanel.hpp>
#include <lux/engine/editor/panels/InspectorPanel.hpp>
#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/editor/panels/SceneFeatureSettingPanel.hpp>
#include <lux/engine/editor/panels/LuaConsole.hpp>
#include "panels/MaterialGraphPanel.hpp"    // private — material-graph node editor
#include "panels/FlowGraphPanel.hpp"        // private — the first GraphKit host
#include "app/AssetDeleteController.hpp"    // private — Delete… 信号 → request()

#include <lux/engine/editor/AssetRegistry.hpp>
#include <lux/engine/editor/thumbnail/ThumbnailService.hpp>
#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/resource/asset/MaterialInstanceAsset.hpp>
#include <lux/engine/resource/asset/MaterialInstanceSerDeser.hpp>
#include <lux/engine/resource/asset/ScriptSerDeser.hpp>   // New-Script authoring
#include <lux/engine/resource/asset/ModelAsset.hpp>       // A-4 describe: model meshes/skeleton
#include <lux/engine/log/Log.hpp>
#include <lux/engine/resource/asset/MeshAsset.hpp>        // A-4 describe: vert/index counts
#include <lux/engine/ecs/script/systems/ScriptRegistry.hpp>   // A-5: cppScriptNames manifest

#include <lux/engine/ui/SceneViewportPanel.hpp>
#include <lux/engine/ui/UIRenderFrameSession.hpp>
#include <lux/engine/ui/UISystem.hpp>

#include <imgui.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace lux::editor
{
    namespace
    {
        constexpr auto kViewport = lux::extensions::contributionId(
            "org.lux.editor.viewport");
        constexpr auto kHierarchy = lux::extensions::contributionId(
            "org.lux.editor.hierarchy");
        constexpr auto kInspector = lux::extensions::contributionId(
            "org.lux.editor.inspector");
        constexpr auto kSceneSettings = lux::extensions::contributionId(
            "org.lux.editor.scene-settings");
        constexpr auto kAssetBrowser = lux::extensions::contributionId(
            "org.lux.editor.asset-browser");
        constexpr auto kMaterialGraph = lux::extensions::contributionId(
            "org.lux.editor.material-graph");
        constexpr auto kFlowGraph = lux::extensions::contributionId(
            "org.lux.editor.flow-graph");
        constexpr auto kScriptEditor = lux::extensions::contributionId(
            "org.lux.editor.script-editor");

        // ── Hover enrichment: per-type asset description ─────────────────────
        // Tooltip lines for a hovered browser entry. CONTRACT: in-memory
        // registry lookups ONLY (fetchAsset/queryInfo) — a hover must never
        // trigger disk IO or a decode; absent data says so instead.
        std::vector<std::string> describeAsset(lux::asset::AssetManager&    mgr,
                                               const lux::asset::asset_id_t& id,
                                               lux::asset::EAssetType        type)
        {
            std::vector<std::string> out;
            switch (type)
            {
            case lux::asset::EAssetType::SCRIPT:
            {
                const auto* a = mgr.fetchAssetAs<lux::asset::ScriptAsset>(id);
                if (!a || !a->data()) break;
                const auto& d = *a->data();
                using Kind = lux::rdesc::Script::Kind;
                switch (d.kind())
                {
                case Kind::LuaSource:    out.emplace_back("kind: Lua source"); break;
                case Kind::NativeModule: out.emplace_back("kind: native module"); break;
                case Kind::CppBehavior:  out.emplace_back("kind: C++ behavior"); break;
                default:                 out.emplace_back("kind: (unset)"); break;
                }
                if (!d.module_name.empty())
                    out.push_back("module: " + d.module_name);
                if (!d.provenance.compiler_id.empty())
                    out.push_back("via: " + d.provenance.compiler_id);
                break;
            }
            case lux::asset::EAssetType::MATERIAL_INSTANCE:
            {
                const auto* a = mgr.fetchAssetAs<lux::asset::MaterialInstanceAsset>(id);
                if (!a || !a->data()) break;
                const auto& parent = a->data()->parent_material_id;
                if (const auto* pinfo = mgr.queryInfo(parent); pinfo)
                    out.push_back(std::string("instance of: ") + pinfo->display_name);
                break;
            }
            case lux::asset::EAssetType::MODEL:
            {
                const auto* a = mgr.fetchAssetAs<lux::asset::ModelAsset>(id);
                if (!a) break;
                out.push_back("meshes: " + std::to_string(a->meshAssetIds().size()));
                if (a->skeletonAssetId().has_value())
                    out.emplace_back("skinned (skeleton + "
                        + std::to_string(a->animationClipAssetIds().size()) + " clips)");
                break;
            }
            case lux::asset::EAssetType::MESH:
            {
                const auto* a = mgr.fetchAssetAs<lux::asset::MeshAsset>(id);
                if (!a) break;
                if (const auto* d = a->data())
                    out.push_back("verts: " + std::to_string(d->vertices.size())
                                + "  indices: " + std::to_string(d->indices.size()));
                else
                    out.emplace_back("(data not loaded)");
                break;
            }
            default:
                break;   // other types: name + path suffice
            }
            return out;
        }
    } // namespace

    EditorShell::EditorShell()  = default;
    EditorShell::~EditorShell() = default;

    AssetBrowser* EditorShell::assetBrowser() noexcept
    {
        return tool_host_ ? static_cast<AssetBrowser*>(
            tool_host_->activePanel(kAssetBrowser)) : nullptr;
    }

    InspectorPanel* EditorShell::inspectorPanel() noexcept
    {
        return tool_host_ ? static_cast<InspectorPanel*>(
            tool_host_->activePanel(kInspector)) : nullptr;
    }

    lux::ui::SceneViewportPanel* EditorShell::viewportPanel() noexcept
    {
        return tool_host_ ? static_cast<lux::ui::SceneViewportPanel*>(
            tool_host_->activePanel(kViewport)) : nullptr;
    }

    HierarchyPanel* EditorShell::hierarchyPanel() noexcept
    {
        return tool_host_ ? static_cast<HierarchyPanel*>(
            tool_host_->activePanel(kHierarchy)) : nullptr;
    }

    SceneFeatureSettingPanel* EditorShell::sceneSettingsPanel() noexcept
    {
        return tool_host_ ? static_cast<SceneFeatureSettingPanel*>(
            tool_host_->activePanel(kSceneSettings)) : nullptr;
    }

    FlowGraphPanel* EditorShell::flowGraphPanel() noexcept
    {
        return tool_host_ ? static_cast<FlowGraphPanel*>(
            tool_host_->activePanel(kFlowGraph)) : nullptr;
    }

    MaterialGraphPanel* EditorShell::materialGraphPanel() noexcept
    {
        return tool_host_ ? static_cast<MaterialGraphPanel*>(
            tool_host_->activePanel(kMaterialGraph)) : nullptr;
    }

    LuaConsole* EditorShell::scriptEditorPanel() noexcept
    {
        return tool_host_ ? static_cast<LuaConsole*>(
            tool_host_->activePanel(kScriptEditor)) : nullptr;
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Phase 1 — original init step 5: panels + window registrations.
    // ─────────────────────────────────────────────────────────────────────────
    bool EditorShell::buildPanels(
        LuxEditor& host,
        FlowGraphPanelContext flow_graph_context)
    {
        auto add = [this](EditorPanelContributionDescriptor descriptor)
        {
            return panel_catalog_.add(std::move(descriptor)).has_value();
        };
        auto base = [](std::string_view id, std::string display_name)
        {
            EditorPanelContributionDescriptor descriptor;
            descriptor.id = lux::extensions::ContributionId{std::string{id}};
            descriptor.display_name = std::move(display_name);
            descriptor.provider = lux::extensions::ExtensionId{
                "org.lux.editor.core"};
            return descriptor;
        };

        auto descriptor = base(kViewport.name(), "Scene Viewport");
        descriptor.supports_deactivation = false;
        descriptor.create = [](const EditorPanelCreateContext&)
        {
            return lux::cxx::expected<
                std::unique_ptr<lux::ui::Panel>,
                EEditorPanelCreateError>{
                std::make_unique<lux::ui::SceneViewportPanel>(
                    "Scene Viewport",
                    std::array<float, 2>{1280.f, 720.f})};
        };
        if (!add(std::move(descriptor)))
            return false;

        descriptor = base(kHierarchy.name(), "Hierarchy");
        descriptor.required_editor_services.push_back(
            lux::ecs::typeToken<lux::ecs::ComponentTypeCatalog>());
        descriptor.create = [](const EditorPanelCreateContext& context)
        {
            const auto* components =
                context.find<lux::ecs::ComponentTypeCatalog>();
            if (!components)
                return lux::cxx::expected<
                    std::unique_ptr<lux::ui::Panel>,
                    EEditorPanelCreateError>{lux::cxx::unexpected(
                        EEditorPanelCreateError::REQUIRED_SERVICE_MISSING)};
            return lux::cxx::expected<
                std::unique_ptr<lux::ui::Panel>,
                EEditorPanelCreateError>{
                std::make_unique<HierarchyPanel>(
                    "Hierarchy", *components)};
        };
        if (!add(std::move(descriptor)))
            return false;

        descriptor = base(kInspector.name(), "Inspector");
        descriptor.required_editor_services.push_back(
            lux::ecs::typeToken<lux::ecs::ComponentTypeCatalog>());
        descriptor.create = [](const EditorPanelCreateContext& context)
        {
            const auto* components =
                context.find<lux::ecs::ComponentTypeCatalog>();
            if (!components)
                return lux::cxx::expected<
                    std::unique_ptr<lux::ui::Panel>,
                    EEditorPanelCreateError>{lux::cxx::unexpected(
                        EEditorPanelCreateError::REQUIRED_SERVICE_MISSING)};
            return lux::cxx::expected<
                std::unique_ptr<lux::ui::Panel>,
                EEditorPanelCreateError>{
                std::make_unique<InspectorPanel>(
                    "Inspector", *components)};
        };
        if (!add(std::move(descriptor)))
            return false;

        descriptor = base(kSceneSettings.name(), "SceneFeatureSetting");
        descriptor.create = [](const EditorPanelCreateContext&)
        {
            return lux::cxx::expected<
                std::unique_ptr<lux::ui::Panel>,
                EEditorPanelCreateError>{
                std::make_unique<SceneFeatureSettingPanel>(
                    "SceneFeatureSetting")};
        };
        if (!add(std::move(descriptor)))
            return false;

        descriptor = base(kAssetBrowser.name(), "Asset Browser");
        descriptor.supports_deactivation = false;
        descriptor.required_editor_services.push_back(
            lux::ecs::typeToken<lux::asset::AssetManager>());
        descriptor.create = [](const EditorPanelCreateContext& context)
        {
            auto assets = context.findShared<lux::asset::AssetManager>();
            if (!assets)
                return lux::cxx::expected<
                    std::unique_ptr<lux::ui::Panel>,
                    EEditorPanelCreateError>{lux::cxx::unexpected(
                        EEditorPanelCreateError::REQUIRED_SERVICE_MISSING)};
            return lux::cxx::expected<
                std::unique_ptr<lux::ui::Panel>,
                EEditorPanelCreateError>{
                std::make_unique<AssetBrowser>(
                    "Asset Browser",
                    std::move(assets))};
        };
        if (!add(std::move(descriptor)))
            return false;

        descriptor = base(kMaterialGraph.name(), "Material Graph");
        descriptor.create = [](const EditorPanelCreateContext&)
        {
            return lux::cxx::expected<
                std::unique_ptr<lux::ui::Panel>,
                EEditorPanelCreateError>{
                std::make_unique<MaterialGraphPanel>("Material Graph")};
        };
        if (!add(std::move(descriptor)))
            return false;

        descriptor = base(kFlowGraph.name(), "Flow Graph");
        descriptor.create = [flow_graph_context](const EditorPanelCreateContext&)
        {
            return lux::cxx::expected<
                std::unique_ptr<lux::ui::Panel>,
                EEditorPanelCreateError>{
                std::make_unique<FlowGraphPanel>(
                    "Flow Graph",
                    flow_graph_context)};
        };
        if (!add(std::move(descriptor)))
            return false;

        descriptor = base(kScriptEditor.name(), "Script Editor");
        descriptor.required_editor_services.push_back(
            lux::ecs::typeToken<lux::asset::AssetManager>());
        descriptor.create = [](const EditorPanelCreateContext& context)
        {
            auto assets = context.findShared<lux::asset::AssetManager>();
            if (!assets)
                return lux::cxx::expected<
                    std::unique_ptr<lux::ui::Panel>,
                    EEditorPanelCreateError>{lux::cxx::unexpected(
                        EEditorPanelCreateError::REQUIRED_SERVICE_MISSING)};
            auto panel = std::make_unique<LuaConsole>("Script Editor");
            panel->setAssetManager(std::move(assets));
            return lux::cxx::expected<
                std::unique_ptr<lux::ui::Panel>,
                EEditorPanelCreateError>{std::move(panel)};
        };
        if (!add(std::move(descriptor)))
            return false;

        EditorPanelCreateContext context;
        if (!context.addShared(host.assetManagerShared()) ||
            !context.add(host) ||
            !context.add(host.componentTypes()))
            return false;
        tool_host_ = std::make_unique<EditorToolHost>(
            host.uiSystem(),
            panel_catalog_,
            std::move(context),
            &host.events());

        auto tools = tool_host_->facade();
        for (const auto& contribution : panel_catalog_.all())
        {
            auto ticket = tools.requestOpen(contribution.id.view());
            (void)tool_host_->processSafePoint();
            if (ticket.snapshot().terminal !=
                lux::extensions::EOperationTerminalState::SUCCEEDED)
                return false;
        }

        if (!assetBrowser() || !inspectorPanel() || !viewportPanel() ||
            !hierarchyPanel() || !sceneSettingsPanel() ||
            !materialGraphPanel() || !flowGraphPanel() ||
            !scriptEditorPanel())
            return false;

        // The browser receives single-consumer command handlers later in
        // wireAssetServices(). Committed content changes remain facts and may
        // fan out to the browser, registry and other observers.
        panel_subs_.add(host.events().subscribe<EditorAssetChanged>(
            host.framePump(),
            [this](const EditorAssetChanged& fact)
            {
                if (fact.change != EEditorAssetChange::CONTENT_UPDATED)
                {
                    if (auto* browser = assetBrowser())
                        browser->rescan();
                }
            }));

        return true;
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Phase 2 — original init steps 7b-8: wiring that needs the render session
    //  + asset services.
    // ─────────────────────────────────────────────────────────────────────────
    void EditorShell::wireAssetServices(LuxEditor&           host,
                                        ThumbnailService*    thumbnails,
                                        AssetRegistry*       asset_registry,
                                        MaterialPreviewHost* material_preview)
    {
        auto* browser = assetBrowser();
        auto* inspector = inspectorPanel();
        auto* viewport = viewportPanel();
        auto* settings = sceneSettingsPanel();
        if (!browser || !inspector || !viewport || !settings)
            return;

        // Scene tab edits the current scene's SceneSettingsComponent in place
        // (returned as void* to keep the panel free of gameplay types).
        settings->setSceneSettingsAccessor(
            [&host]() -> void*
            {
                auto* scene = host.currentScene();
                return (scene && scene->isLive())
                    ? static_cast<void*>(&scene->ensureSceneSettings())
                    : nullptr;
            });

        // Thumbnails (null = service init failed; browser uses glyphs only).
        if (thumbnails)
            browser->setThumbnailService(thumbnails);

        // Hover enrichment: type-specific tooltip lines. CONTRACT: registry lookup
        // only (fetchAsset = in-memory), never a load — hover is a hot path.
        browser->setDescribeHook(
            [&host](const lux::asset::asset_id_t& id, lux::asset::EAssetType type)
            { return describeAsset(*host.assetManagerShared(), id, type); });

        // Inspector asset-reference fields: named, type-validated pickers +
        // VFS-path tooltips + the capability-gated Add-Component menu.
        inspector->setAssetRegistry(asset_registry);
        inspector->setAssetManager(host.assetManagerShared().get());
        inspector->setAvailableComponentsProvider(
            [&host]() -> std::vector<std::string>
            {
                const EditorScene* s = host.currentScene();
                return s ? s->availableComponentFqns()
                         : std::vector<std::string>{};
            });
        // A-6 C++ manifest: the browser's "New Script ▸ C++" submenu lists the
        // binary's registered behaviors (registration is startup-time, so the
        // enumeration is session-stable); picking one authors its manifest
        // asset (CppBehaviorScript) — the entry name path is retired.
        browser->setCppScriptNamesProvider(
            [] { return lux::ecs::scriptRegistry().cppScriptNames(); });

        if (auto* mg = materialGraphPanel())
        {
            // Live material preview (null = preview host init failed).
            if (material_preview)
                mg->setPreviewHost(material_preview);

            mg->setAssetServices(asset_registry,
                                 host.assetManagerShared(), &host.events(),
                                 host.thumbnailService());
            // Delete is a single-consumer editor command, not a broadcast fact.
            browser->setDeleteAssetHandler(
                [&host](const DeleteAssetCommand& command)
                {
                    host.assetDeleteController().request(command);
                });

            // UE-style: right-click a graph material -> author a Material
            // Instance of it (parent ref, no overrides yet).
            browser->setCreateInstanceHandler(
                    [&host](const CreateMaterialInstanceCommand& command)
                    {
                        auto  am     = host.assetManagerShared();
                        const auto& parent = command.parent;
                        if (!am || !host.currentProject() || parent.is_nil()) return;

                        std::string name = "MaterialInstance";
                        if (const auto* pinfo = am->queryInfo(parent); pinfo)
                            name = std::string(pinfo->display_name) + "_Inst";

                        auto data = std::make_unique<lux::asset::MaterialInstanceData>();
                        data->parent_material_id = parent;
                        auto asset = am->createAsset<lux::asset::MaterialInstanceAsset>(
                            std::move(data));
                        if (auto* mi = asset->mutableInfo())
                        {
                            const std::size_t n =
                                std::min(name.size(), sizeof(mi->display_name) - 1);
                            std::memcpy(mi->display_name, name.data(), n);
                            mi->display_name[n] = '\0';
                        }
                        const auto id = asset->id();
                        if (!am->registerAsset(std::move(asset))) return;

                        // Pick a free filename (X_Inst, X_Inst_1, …) so authoring
                        // two instances of one parent never clobbers a file.
                        const auto dir = host.currentProject()->contentRoot() / "Materials";
                        std::error_code mkec; std::filesystem::create_directories(dir, mkec);
                        std::filesystem::path dest;
                        for (int i = 0; ; ++i)
                        {
                            const std::string fn = name + (i == 0 ? "" : "_" + std::to_string(i));
                            dest = dir / (fn + ".luxasset");
                            std::error_code ec;
                            if (!std::filesystem::exists(dest, ec)) break;
                        }
                        lux::asset::MaterialInstanceSerDeser ser(am);
                        ser.exportAsLuxAsset(id, dest);

                        host.events().publish(EditorAssetChanged{
                            id,
                            EEditorAssetChange::ADDED,
                            am->contentRevision(id),
                            dest
                        });
                    });
        }

        // The engine VIRTUAL path of a content file (the /Game mount over the
        // content root) — what every "where did this go" surface shows, never a
        // raw OS path (user ruling).
        const auto contentVPath = [asset_registry](const std::filesystem::path& abs) -> std::string
        {
            if (!asset_registry || asset_registry->root().empty())
                return abs.filename().replace_extension().generic_string();
            std::error_code ec;
            auto rel = std::filesystem::relative(abs, asset_registry->root(), ec);
            if (ec || rel.empty()) rel = abs.filename();
            return "/Game/" + rel.replace_extension().generic_string();
        };

        // Double-click a SCRIPT asset -> open its Lua source in the
        // Script Editor for IN-PLACE editing.
        if (scriptEditorPanel())
        {
            // Blank-area right-click "New …" — author the asset AT
            // the right-clicked folder and open its editor immediately.
            browser->setCreateAssetHandler(
                    [this, &host, contentVPath](const CreateAssetCommand& command)
                    {
                        auto am = host.assetManagerShared();
                        if (!am || command.folder.empty())
                            return;

                        if (command.type == lux::asset::EAssetType::MATERIAL)
                        {
                            if (auto* mg = materialGraphPanel())
                            {
                                if (mg->createNewMaterialAssetAt(command.folder))
                                    mg->setVisible(true);
                            }
                            return;
                        }
                        if (command.type != lux::asset::EAssetType::SCRIPT)
                            return;

                        // A-6: "New Script ▸ C++ ▸ <behavior>" — author the
                        // behavior's MANIFEST asset (CppBehaviorScript). Nothing
                        // to open afterwards: the code lives in the binary; the
                        // asset is what makes it pickable/droppable content.
                        if (!command.cpp_behavior.empty())
                        {
                            std::string name = command.cpp_behavior;
                            std::filesystem::path dest;
                            for (int i = 0; ; ++i)
                            {
                                const std::string fn =
                                    name + (i == 0 ? "" : "_" + std::to_string(i));
                                dest = command.folder / (fn + ".luxasset");
                                std::error_code ec;
                                if (!std::filesystem::exists(dest, ec)) break;
                            }
                            auto desc = std::make_unique<lux::rdesc::Script>();
                            desc->module_name            = name;
                            desc->body                   = lux::rdesc::CppBehaviorScript{name};
                            desc->provenance.compiler_id = "editor-new";
                            auto am2   = host.assetManagerShared();
                            auto asset = am2->createAsset<lux::asset::ScriptAsset>(std::move(desc));
                            if (auto* si = asset->mutableInfo())
                            {
                                const std::size_t nn =
                                    std::min(name.size(), sizeof(si->display_name) - 1);
                                std::memcpy(si->display_name, name.data(), nn);
                                si->display_name[nn] = '\0';
                            }
                            const auto id2 = asset->id();
                            if (!am2->registerAsset(std::move(asset)))
                                return;
                            std::error_code mk_ec2;
                            std::filesystem::create_directories(command.folder, mk_ec2);
                            lux::asset::ScriptSerDeser ser2(am2);
                            if (ser2.exportAsLuxAsset(id2, dest) != lux::asset::EAssetError::SUCCESS)
                                return;
                            host.events().publish(EditorAssetChanged{
                                id2,
                                EEditorAssetChange::ADDED,
                                am2->contentRevision(id2),
                                dest
                            });
                            return;
                        }

                        // Free name: NewScript, NewScript_1, … (never clobber).
                        std::string name;
                        std::filesystem::path dest;
                        for (int i = 0; ; ++i)
                        {
                            name = i == 0 ? "NewScript" : ("NewScript_" + std::to_string(i));
                            dest = command.folder / (name + ".luxasset");
                            std::error_code ec;
                            if (!std::filesystem::exists(dest, ec)) break;
                        }

                        // Author the LuaSource SCRIPT asset with a behavior template.
                        auto desc = std::make_unique<lux::rdesc::Script>();
                        desc->module_name            = name;
                        desc->body                   = lux::rdesc::LuaSourceScript{};
                        desc->provenance.compiler_id = "editor-new";
                        auto asset = am->createAsset<lux::asset::ScriptAsset>(std::move(desc));
                        if (auto* si = asset->mutableInfo())
                        {
                            const std::size_t n =
                                std::min(name.size(), sizeof(si->display_name) - 1);
                            std::memcpy(si->display_name, name.data(), n);
                            si->display_name[n] = '\0';
                        }
                        static constexpr std::string_view kTemplate =
                            "return {\n"
                            "  OnCreate = function(self)\n"
                            "  end,\n"
                            "  OnUpdate = function(self, dt)\n"
                            "  end,\n"
                            "  OnDestroy = function(self)\n"
                            "  end,\n"
                            "}\n";
                        std::vector<std::byte> payload(kTemplate.size());
                        std::memcpy(payload.data(), kTemplate.data(), kTemplate.size());
                        static_cast<lux::asset::ScriptAsset*>(asset.get())
                            ->setPayload(std::move(payload));

                        const auto id = asset->id();
                        if (!am->registerAsset(std::move(asset)))
                            return;
                        std::error_code mk_ec;
                        std::filesystem::create_directories(command.folder, mk_ec);
                        lux::asset::ScriptSerDeser ser(am);
                        if (ser.exportAsLuxAsset(id, dest) != lux::asset::EAssetError::SUCCESS)
                            return;
                        host.events().publish(EditorAssetChanged{
                            id,
                            EEditorAssetChange::ADDED,
                            am->contentRevision(id),
                            dest
                        });
                        if (auto* script = scriptEditorPanel())
                        {
                            script->openScriptAsset(
                                id,
                                dest,
                                contentVPath(dest));
                            script->setVisible(true);
                        }
                    });
        }

        // Asset activation has exactly one command consumer. The shell routes
        // by stable asset type so opening an editor never travels through the
        // fact bus and cannot be replayed or delayed by a pump.
        browser->setActivateHandler(
            [this, &host, asset_registry, contentVPath](
                const ActivateAssetCommand& command)
            {
                switch (command.type)
                {
                case lux::asset::EAssetType::MATERIAL:
                case lux::asset::EAssetType::MATERIAL_INSTANCE:
                    if (auto* panel = materialGraphPanel())
                    {
                        panel->openAsset(command.id);
                        panel->setVisible(true);
                    }
                    return;

                case lux::asset::EAssetType::SCRIPT:
                {
                    auto* panel = scriptEditorPanel();
                    if (!panel || !asset_registry)
                        return;

                    // C++ behavior manifests have no editable source payload.
                    const auto* script = host.assetManagerShared()
                        ->fetchAssetAs<lux::asset::ScriptAsset>(command.id);
                    if (script && script->data()
                        && script->data()->kind()
                            != lux::rdesc::Script::Kind::LuaSource)
                    {
                        return;
                    }

                    const auto* meta = asset_registry->find(command.id);
                    if (!meta)
                        return;
                    const auto absolute = asset_registry->root() / meta->rel_path;
                    panel->openScriptAsset(
                        command.id,
                        absolute,
                        contentVPath(absolute)
                    );
                    panel->setVisible(true);
                    return;
                }

                case lux::asset::EAssetType::FLOW_GRAPH:
                    if (auto* panel = flowGraphPanel())
                    {
                        panel->openAsset(command.id);
                        panel->setVisible(true);
                    }
                    return;

                default:
                    return;
                }
            });

        // Viewport asset drop: MODEL family spawns an entity (scene-domain
        // action, host forward); everything else logs a hint so non-model drags
        // don't silently no-op.(单槽回调缝:置空即断开;panel 随 shell 死,
        // 槽死于 teardownPanels 的面板 reset —— 不需要连接对象。)
        viewport->on_asset_dropped =
            [&host](const lux::ui::ViewportAssetDropped& e)
            {
                const auto& payload = e.payload;
                if (!host.currentScene()) return;
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
                const std::string display_name{payload.display_name};
                const auto submitted = host.spawnModelEntity(
                    id,
                    [display_name](lux::editor::InstanceSpawnOutcome outcome)
                    {
                        if (!outcome)
                        {
                            lux::log::error(
                                "editor",
                                "asset-drop spawn '{}' failed (stage={})",
                                display_name,
                                static_cast<int>(outcome.error().code));
                            return;
                        }
                        lux::log::info(
                            "editor",
                            "asset-drop created entity {} from '{}'",
                            static_cast<std::uint32_t>(outcome->root),
                            display_name);
                    });
                if (!submitted)
                {
                    lux::log::warn(
                        "editor",
                        "asset-drop spawn '{}' rejected (runtime={})",
                        display_name,
                        static_cast<int>(submitted.error()));
                }
            };
    }

    // ─────────────────────────────────────────────────────────────────────────
    void EditorShell::retargetScene(LuxEditor& host, EditorScene* scene)
    {
        const bool live = scene && scene->isLive();
        // 目录是进程域的(paramSetOp 那半),不随场景生死 —— 面板从此不可能
        // 攥着一个已亡场景的指针。场景域参数只剩 scene_id。
        if (auto* settings = sceneSettingsPanel(); settings && live)
        {
            settings->setTarget(
                &host.renderSession(),
                &host.renderControlSession(),
                scene->sceneId(),
                &host.featureCatalog());
            settings->setEditorScene(scene);
        }
        else if (settings)
        {
            settings->setTarget(
                &host.renderSession(),
                &host.renderControlSession(),
                lux::render::RenderSceneId{},
                nullptr);
            settings->setEditorScene(nullptr);
        }

        Selection* sel = live ? &scene->selection() : nullptr;
        if (auto* inspector = inspectorPanel())
        {
            inspector->setSelection(sel);
            inspector->setEditorScene(live ? scene : nullptr);
        }
        if (auto* hierarchy = hierarchyPanel())
        {
            hierarchy->setSelection(sel);
            if (live)
            {
                hierarchy->setWorldActorSource(
                    [scene]()
                    {
                        return scene->indexedWorldActorCount();
                    },
                    [scene](
                        std::string_view text,
                        std::size_t offset,
                        std::size_t maximum)
                    {
                        auto indexed = scene->queryWorldActors(
                            text, offset, maximum);
                        std::vector<HierarchyWorldActorItem> result;
                        result.reserve(indexed.size());
                        for (auto& actor : indexed)
                        {
                            result.push_back({
                                lux::entity_scene::PersistentEntityId{
                                    actor.actor.value()},
                                std::move(actor.display_name),
                                std::move(actor.actor_class)});
                        }
                        return result;
                    },
                    [scene](lux::entity_scene::PersistentEntityId actor)
                    {
                        (void)scene->requestWorldActorProxy(actor);
                    });
            }
            else
            {
                hierarchy->clearWorldActorSource();
            }
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // ─────────────────────────────────────────────────────────────────────────
} // namespace lux::editor
