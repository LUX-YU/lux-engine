#include "app/ProjectController.hpp"
#include "app/SceneController.hpp"   // scene_.loadScene / unloadScene

#include <lux/engine/editor/scene/EditorScene.hpp>        // BringUpConfig
#include <lux/engine/editor/import/AssetImporter.hpp>     // registerContentFolder + ELoadMode
#include <lux/engine/editor/content/EngineContentPath.hpp>// engine_content_path
#include <lux/engine/editor/panels/AssetBrowser.hpp>      // setWorkingDirectory
#include <lux/engine/editor/AssetRegistry.hpp>            // scan / size / provider
#include <lux/engine/editor/extensions/EditorTools.hpp>
#include <lux/engine/ui/Panel.hpp>                        // setVisible / isVisible
#include <lux/engine/ui/UISystem.hpp>                     // layout load/save/clear
#include <lux/engine/resource/asset/AssetManager.hpp>              // setVfs / registerContentFolder arg
#include <lux/engine/resource/asset/AssetVfs.hpp>                  // AssetVfs
#include <lux/engine/authoring/assets/LooseAssetProvider.hpp>
#include <lux/engine/authoring/project/RecentProjects.hpp>

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
        lux::ui::UISystem&                        ui_system,
        std::shared_ptr<lux::asset::AssetManager> asset_mgr,
        AssetRegistry&                            asset_registry,
        AssetBrowser&                             asset_browser,
        EditorToolHost&                           tools,
        lux::extensions::EngineExtensions&       extensions) noexcept
        : scene_(scene)
        , ui_system_(ui_system)
        , asset_mgr_(std::move(asset_mgr))
        , asset_registry_(asset_registry)
        , asset_browser_(asset_browser)
        , tools_(tools)
        , extensions_(extensions)
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
        for (const auto& panel : tools_.snapshot())
        {
            if (!panel.active)
                continue;
            const auto it = saved.find(
                std::string{panel.contribution.name()});
            (void)tools_.facade().requestVisible(
                panel.contribution.view(),
                it != saved.end() ? it->second : panel.default_visible);
        }
        (void)tools_.processSafePoint();
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
        for (const auto& panel : tools_.snapshot())
            if (panel.active)
                f << panel.contribution.name() << '='
                  << (panel.visible ? '1' : '0') << '\n';
    }

    bool ProjectController::openProject(const std::filesystem::path& luxproject_file)
    {
        auto opened = lux::authoring::Project::openFromDisk(luxproject_file);
        if (!opened)
        {
            std::fprintf(stderr, "[LuxEditor] openProject failed: %s\n",
                         opened.error().c_str());
            return false;
        }

        closeProject(); // tears down current scene + drops current_project_

        current_project_ = std::make_unique<lux::authoring::Project>(std::move(*opened));

        std::vector<lux::extensions::ExtensionModuleRequirement> requirements;
        requirements.reserve(current_project_->manifest().extensions.size());
        for (const auto& entry : current_project_->manifest().extensions)
        {
            auto path = entry.path;
            if (path.is_relative())
                path = current_project_->root() / path;
            requirements.push_back(
                lux::extensions::ExtensionModuleRequirement::fromPath(
                    entry.id,
                    std::move(path),
                    entry.target,
                    entry.required_major,
                    entry.minimum_minor));
        }
        if (auto added = extensions_.addRequirements(requirements); !added)
        {
            std::fprintf(
                stderr,
                "[LuxEditor] extension manifest rejected (%u)\n",
                static_cast<unsigned>(added.error()));
            current_project_.reset();
            return false;
        }
        if (!requirements.empty())
        {
            PendingProjectOpen pending;
            pending.luxproject_file = luxproject_file;
            pending.modules.reserve(requirements.size());
            for (const auto& requirement : requirements)
                pending.modules.push_back(
                    extensions_.requestLoad(requirement.id.view()));
            pending_open_.emplace(std::move(pending));
            return true;
        }

        return finishOpenProject(luxproject_file);
    }

    bool ProjectController::finishOpenProject(const std::filesystem::path& luxproject_file)
    {
        if (!current_project_)
            return false;

        // Per-project ImGui layout. The cache dir may not exist yet on
        // legacy projects; create it so the first autosave write
        // doesn't fail. Load is a no-op when the file is missing —
        // ImGui then falls back to per-window FirstUseEver defaults.
        {
            const auto layout_path = current_project_->layoutPath();
            std::error_code mkdir_ec;
            std::filesystem::create_directories(layout_path.parent_path(), mkdir_ec);
            // 布局读不出来不该拦住开项目 —— 掉回 FirstUseEver 缺省即可,
            // 但要说一声,否则用户只会看到"我的面板怎么全变了"。
            if (const auto ec = ui_system_.loadLayoutFromFile(layout_path))
            {
                std::fprintf(stderr,
                    "[LuxEditor] 布局读取失败,回落到默认布局:%s (%s)\n",
                    layout_path.string().c_str(), ec.message().c_str());
            }
            ui_system_.setAutosaveTarget(layout_path);
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
        asset_registry_.scan(current_project_->contentRoot());
        std::fprintf(stderr, "[LuxEditor] asset registry indexed %zu assets\n",
                     asset_registry_.size());

        // VP-P4: rebuild the asset VFS for this project — /Game over the
        // project content (the SAME LooseAssetProvider the registry is a view
        // of) + /Engine over the baked engine content. findAssetByPath /
        // ensureAsset (and the script bindings) resolve against this. The
        // eager registerContentFolder load above is intentionally kept —
        // startup cost is unchanged; ensureAsset simply finds everything
        // already present, and only late additions (pak mounts, files the
        // eager pass missed) go through the lazy path.
        if (asset_mgr_)
        {
            auto vfs = std::make_shared<lux::asset::AssetVfs>();
            // provider() IS nullable — the registry has none until a scan
            // succeeds, and then /Game simply isn't mounted.
            if (asset_registry_.provider())
                vfs->mount({ "/Game", asset_registry_.provider(), 0 });

            std::error_code engine_ec;
            if (std::filesystem::is_directory(engine_content_path, engine_ec)
                && !engine_ec)
            {
                auto engine_provider =
                    std::make_shared<lux::authoring::LooseAssetProvider>(
                        engine_content_path);
                engine_provider->rescan();
                vfs->mount({ "/Engine", std::move(engine_provider), 0 });
            }
            asset_mgr_->setVfs(std::move(vfs));
        }

        // Re-target AssetBrowser at the project's Content root so the
        // panel shows the project's assets. The AssetBrowser legacy API
        // takes one root;
        asset_browser_.setWorkingDirectory(current_project_->contentRoot());

        // If the manifest specifies a default scene that exists on disk, open
        // it. Otherwise bring up an empty scene (just the editor camera + the
        // reference grid); the user opens/creates content from there.
        const auto default_world = current_project_->defaultWorldPath();
        BringUpConfig cfg;
        cfg.name = current_project_->manifest().name;
        cfg.play_cache_root =
            current_project_->cacheRoot() / "cache" / "world-play";
        if (!default_world.empty())
            cfg.from_scene_file = default_world;
        const bool brought_up = scene_.loadScene(cfg);
        if (brought_up)
            lux::authoring::pushRecentProject(luxproject_file);
        return brought_up;
    }

    std::size_t ProjectController::processSafePoint() noexcept
    {
        if (!pending_open_)
            return 0u;

        bool all_ready = true;
        for (const auto& ticket : pending_open_->modules)
        {
            const auto snapshot = ticket.snapshot();
            if (snapshot.terminal ==
                lux::extensions::EOperationTerminalState::FAILED ||
                snapshot.terminal ==
                lux::extensions::EOperationTerminalState::SUPERSEDED)
            {
                std::fprintf(
                    stderr,
                    "[LuxEditor] project extension load failed; project "
                    "was not published\n");
                pending_open_.reset();
                closeProject();
                return 1u;
            }
            all_ready = all_ready &&
                snapshot.terminal ==
                    lux::extensions::EOperationTerminalState::SUCCEEDED;
        }
        if (!all_ready)
            return 0u;

        auto manifest_path = pending_open_->luxproject_file;
        pending_open_.reset();
        if (!finishOpenProject(manifest_path))
        {
            closeProject();
            return 1u;
        }
        return 1u;
    }

    bool ProjectController::newProject(const std::filesystem::path& root,
                                       std::string_view              project_name)
    {
        auto created = lux::authoring::Project::newOnDisk(root, project_name);
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
        pending_open_.reset();
        // Auto-save window visibility before we lose the project pointer — this is
        // what makes "remember which panels were open" work on close / project
        // switch / app exit (closeProject runs on all three), no manual save.
        saveWindowVisibility();

        // Flush the project's ImGui layout before we lose the project
        // pointer; then disengage autosave and clear in-memory ini
        // state so the next project doesn't merge with the previous.
        if (current_project_)
        {
            // 关项目是最后一次落盘的机会:失败就是用户这一整轮的布局改动没了。
            if (const auto ec = ui_system_.saveLayoutToFile(current_project_->layoutPath()))
            {
                std::fprintf(stderr,
                    "[LuxEditor] 关闭项目时布局保存失败,本次布局改动已丢失:%s (%s)\n",
                    current_project_->layoutPath().string().c_str(), ec.message().c_str());
            }
            ui_system_.setAutosaveTarget({});
            ui_system_.clearLayout();
        }

        scene_.unloadScene();
        current_project_.reset();

        // The VFS mounts reference the closed project's content folder —
        // drop them so stale resolves fail loudly instead of reading a
        // dead project's files.
        if (asset_mgr_)
            asset_mgr_->setVfs(nullptr);

        // Empty AssetBrowser back to working directory.
        asset_browser_.setWorkingDirectory(std::filesystem::current_path());
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
        //
        // 布局写不进去 → 整个"保存项目"就没有完整完成,返回 false。清单存住了
        // 而布局没有,报成功等于骗人:用户下次开项目会发现面板全回到了默认。
        if (const auto ec = ui_system_.saveLayoutToFile(current_project_->layoutPath()))
        {
            std::fprintf(stderr,
                "[LuxEditor] 保存项目:清单已写入,但布局保存失败:%s (%s)\n",
                current_project_->layoutPath().string().c_str(), ec.message().c_str());
            return false;
        }
        return true;
    }

} // namespace lux::editor
