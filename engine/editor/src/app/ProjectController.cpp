#include "app/ProjectController.hpp"
#include "app/SceneController.hpp"   // scene_.loadScene / unloadScene / setCurrentScenePath

#include <lux/engine/editor/scene/EditorScene.hpp>        // BringUpConfig
#include <lux/engine/editor/import/AssetImporter.hpp>     // registerContentFolder + ELoadMode
#include <lux/engine/editor/content/EngineContentPath.hpp>// engine_content_path
#include <lux/engine/editor/panels/AssetBrowser.hpp>      // setWorkingDirectory
#include <lux/engine/editor/AssetRegistry.hpp>            // scan / size / provider
#include <lux/engine/editor/app/PanelRegistry.hpp>        // windows()
#include <lux/engine/ui/Panel.hpp>                        // setVisible / isVisible
#include <lux/engine/ui/UISystem.hpp>                     // layout load/save/clear
#include <lux/engine/asset/AssetManager.hpp>              // setVfs / registerContentFolder arg
#include <lux/engine/asset/AssetVfs.hpp>                  // AssetVfs
#include <lux/engine/asset/LooseDirProvider.hpp>          // /Engine provider
#include <lux/engine/project/RecentProjects.hpp>          // pushRecentProject

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace lux::editor
{
    namespace
    {
        // Whether registerContentFolder materializes info-only shells (lazy) vs
        // eager full loads. Matches the original LuxEditor default.
        constexpr bool kStreamingShells = true;
    } // namespace

    ProjectController::ProjectController(
        SceneController&                          scene,
        lux::ui::UISystem*                        ui_system,
        std::shared_ptr<lux::asset::AssetManager> asset_mgr,
        AssetRegistry*                            asset_registry,
        AssetBrowser*                             asset_browser,
        PanelRegistry&                            panels) noexcept
        : scene_(scene)
        , ui_system_(ui_system)
        , asset_mgr_(std::move(asset_mgr))
        , asset_registry_(asset_registry)
        , asset_browser_(asset_browser)
        , panel_registry_(panels)
    {
    }

    void ProjectController::loadWindowVisibility()
    {
        if (!current_project_)
            return;
        // Parse "<id>=<0|1>" lines (missing file → empty map). A window not listed
        // (e.g. added since the cache was written) keeps its registered default.
        std::unordered_map<std::string, bool> saved;
        std::ifstream f(current_project_->windowConfigPath());
        for (std::string line; std::getline(f, line); )
        {
            if (line.empty() || line.front() == '#')
                continue;
            const auto eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            saved.emplace(line.substr(0, eq), line.substr(eq + 1) != "0");
        }
        for (const auto& w : panel_registry_.windows())
        {
            if (!w.panel)
                continue;
            const auto it = saved.find(w.id);
            w.panel->setVisible(it != saved.end() ? it->second : w.default_visible);
        }
    }

    void ProjectController::saveWindowVisibility() const
    {
        if (!current_project_)
            return;
        const auto path = current_project_->windowConfigPath();
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream f(path, std::ios::trunc);
        if (!f.is_open())
            return;
        f << "# Editor window visibility — auto-saved per project (no manual save).\n"
             "# <id>=<0|1>; a window not listed here uses its built-in default.\n";
        for (const auto& w : panel_registry_.windows())
            if (w.panel)
                f << w.id << '=' << (w.panel->isVisible() ? '1' : '0') << '\n';
    }

    bool ProjectController::openProject(const std::filesystem::path& luxproject_file)
    {
        auto opened = Project::openFromDisk(luxproject_file);
        if (!opened)
        {
            std::fprintf(stderr, "[LuxEditor] openProject failed: %s\n",
                         opened.error().c_str());
            return false;
        }

        closeProject(); // tears down current scene + drops current_project_

        current_project_ = std::make_unique<Project>(std::move(*opened));

        // Per-project ImGui layout. The cache dir may not exist yet on
        // legacy projects; create it so the first autosave write
        // doesn't fail. Load is a no-op when the file is missing —
        // ImGui then falls back to per-window FirstUseEver defaults.
        if (ui_system_)
        {
            const auto layout_path = current_project_->layoutPath();
            std::error_code mkdir_ec;
            std::filesystem::create_directories(layout_path.parent_path(), mkdir_ec);
            ui_system_->loadLayoutFromFile(layout_path);
            ui_system_->setAutosaveTarget(layout_path);
        }

        // Restore this project's window visibility (which panels are shown) from
        // its .lux/ cache — auto-saved on close, so reopening remembers the last
        // setup with no manual action. The dock LAYOUT itself was restored from
        // the sibling .ini above.
        loadWindowVisibility();

        // M3: mount every persisted .luxasset / .luxmodel under Content/
        // into the AssetManager BEFORE the AssetBrowser scans + the scene
        // is loaded. Without this, scene UUID references (mesh / material /
        // texture / skeleton / clip) would all dangle on first frame and
        // the editor would render bind-pose meshes / black material slots.
        // Idempotent: re-opening the same project hits ASSET_ALREADY_EXIST
        // for every file and registerContentFolder treats that as success.
        if (asset_mgr_)
        {
            const auto mode = kStreamingShells
                ? lux::editor::ELoadMode::Shells
                : lux::editor::ELoadMode::Eager;
            const auto registered = lux::editor::registerContentFolder(
                current_project_->contentRoot(), asset_mgr_, mode);
            std::fprintf(stderr,
                "[LuxEditor] registered %zu existing assets from %s (%s)\n",
                registered, current_project_->contentRoot().string().c_str(),
                kStreamingShells ? "shells" : "eager");
        }

        // Lightweight project asset index (header-only scan) for the editor's
        // pickers — the SampleTexture texture picker + a future global asset search.
        if (asset_registry_)
        {
            asset_registry_->scan(current_project_->contentRoot());
            std::fprintf(stderr, "[LuxEditor] asset registry indexed %zu assets\n",
                         asset_registry_->size());
        }

        // VP-P4: rebuild the asset VFS for this project — /Game over the
        // project content (the SAME LooseDirProvider the registry is a view
        // of) + /Engine over the baked engine content. findAssetByPath /
        // ensureAsset (and the script bindings) resolve against this. The
        // eager registerContentFolder load above is intentionally kept —
        // startup cost is unchanged; ensureAsset simply finds everything
        // already present, and only late additions (pak mounts, files the
        // eager pass missed) go through the lazy path.
        if (asset_mgr_)
        {
            auto vfs = std::make_shared<lux::asset::AssetVfs>();
            if (asset_registry_ && asset_registry_->provider())
                vfs->mount({ "/Game", asset_registry_->provider(), 0 });

            std::error_code engine_ec;
            if (std::filesystem::is_directory(engine_content_path, engine_ec)
                && !engine_ec)
            {
                auto engine_provider =
                    std::make_shared<lux::asset::LooseDirProvider>(
                        engine_content_path);
                engine_provider->rescan();
                vfs->mount({ "/Engine", std::move(engine_provider), 0 });
            }
            asset_mgr_->setVfs(std::move(vfs));
        }

        // Re-target AssetBrowser at the project's Content root so the
        // panel shows the project's assets. The AssetBrowser legacy API
        // takes one root;
        if (asset_browser_)
            asset_browser_->setWorkingDirectory(current_project_->contentRoot());

        // If the manifest specifies a default scene that exists on disk, open
        // it. Otherwise bring up an empty scene (just the editor camera + the
        // reference grid); the user opens/creates content from there.
        const auto default_scene = current_project_->defaultScenePath();
        BringUpConfig cfg;
        cfg.name = current_project_->manifest().name;
        if (!default_scene.empty())
        {
            cfg.from_scene_file = default_scene;
            // Set the path BEFORE loadScene: its internal unloadScene early-
            // returns (no scene yet, after closeProject above) so the path
            // survives the bring-up.
            scene_.setCurrentScenePath(default_scene);
        }
        const bool brought_up = scene_.loadScene(cfg);
        if (brought_up)
            lux::project::pushRecentProject(luxproject_file);
        return brought_up;
    }

    bool ProjectController::newProject(const std::filesystem::path& root,
                                       std::string_view              project_name)
    {
        auto created = Project::newOnDisk(root, project_name);
        if (!created)
        {
            std::fprintf(stderr, "[LuxEditor] newProject failed: %s\n",
                         created.error().c_str());
            return false;
        }
        // newOnDisk only writes the skeleton — no default scene yet.
        // openProject() brings up an empty scene (editor camera + grid);
        // the user populates it and `Save Scene As`.
        return openProject(created->manifestPath());
    }

    void ProjectController::closeProject() noexcept
    {
        // Auto-save window visibility before we lose the project pointer — this is
        // what makes "remember which panels were open" work on close / project
        // switch / app exit (closeProject runs on all three), no manual save.
        saveWindowVisibility();

        // Flush the project's ImGui layout before we lose the project
        // pointer; then disengage autosave and clear in-memory ini
        // state so the next project doesn't merge with the previous.
        if (ui_system_ && current_project_)
        {
            ui_system_->saveLayoutToFile(current_project_->layoutPath());
            ui_system_->setAutosaveTarget({});
            ui_system_->clearLayout();
        }

        scene_.unloadScene();
        current_project_.reset();

        // The VFS mounts reference the closed project's content folder —
        // drop them so stale resolves fail loudly instead of reading a
        // dead project's files.
        if (asset_mgr_)
            asset_mgr_->setVfs(nullptr);

        // Empty AssetBrowser back to working directory.
        if (asset_browser_)
            asset_browser_->setWorkingDirectory(std::filesystem::current_path());
    }

    bool ProjectController::saveProject()
    {
        if (!current_project_)
            return false;

        // Window visibility already auto-saves on close; capture it here too so an
        // explicit "Save Project" persists the current set alongside the layout.
        saveWindowVisibility();

        auto saved = current_project_->saveManifest();
        if (!saved)
        {
            std::fprintf(stderr, "[LuxEditor] saveProject failed: %s\n",
                         saved.error().c_str());
            return false;
        }
        // Persist layout alongside the manifest so "Save Project" is
        // a single user action that captures both.
        if (ui_system_)
            ui_system_->saveLayoutToFile(current_project_->layoutPath());
        return true;
    }

} // namespace lux::editor
