#include "app/EditorMenuBar.hpp"
#include "app/ImportController.hpp"      // importController().paintDialog()
#include "app/AssetDeleteController.hpp" // assetDeleteController().paintDialog()
#include <lux/engine/editor/import/AssetImporter.hpp>   // importableExtensions (audit 7.2)

// ★ 这里曾有 `kNew2DSceneCapabilities`（批 5 删除）——新建 2D 场景时写进文件头的
//   能力掩码，值是 `Core | ImageRendering | FlipbookAnimation`，注释自称
//   「Pixel/physics stay opt-in」。
//
//   问题是**没有 in 可 opt**：掩码只在这里写一次，存盘原样回显，全仓没有任何修改
//   它的入口。于是编辑器建的 2D 场景永远只有那三位，想要物理/像素仿真只能手改
//   文件头或写 C++ —— 这也是 2D 物理的 demo 是自组 plan 的 C++ 测试而不是编辑器
//   场景的原因。掩码删了，条目全装，能不能用由「场景里有没有那个组件」回答。


#include <lux/engine/editor/app/LuxEditor.hpp>     // the editor command surface + accessors
#include <lux/engine/authoring/project/Project.hpp>
#include <lux/engine/editor/app/FileDialog.hpp>    // pick/open/save native dialogs
#include <lux/engine/hosts/launcher/SpawnHelpers.hpp>    // spawnLauncher (Switch Project)
#include <lux/engine/authoring/project/RecentProjects.hpp>
#include <lux/engine/ui/UISystem.hpp>              // saveLayoutToFile / clearLayout
#include <lux/engine/ui/Panel.hpp>                 // Window menu: title / isVisible / setVisible
#include <lux/engine/log/Log.hpp>                  // 保存失败要让用户看见(§7.1)

#include <imgui.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

namespace lux::editor
{
    void EditorMenuBar::paint()
    {
        if (ImGui::BeginMainMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::BeginMenu("Project"))
                {
                    // NB: every menu item that triggers a scene swap
                    // (newProject / openProject / closeProject / openScene)
                    // enqueues into the editor's deferred-action queue instead
                    // of running inline. Running inline would interleave
                    // editor-side ImGui state with EditorScene::tearDown /
                    // bringUp's multi-frame render-thread submissions.

                    if (ImGui::MenuItem("New..."))
                    {
                        // Native folder picker (owner-modal). The project lives
                        // in the chosen directory; its name is the directory
                        // basename — one dialog deep, no multi-field form. Runs
                        // in the deferred drain (off the ImGui frame), where a
                        // blocking modal dialog is safe.
                        editor_.enqueue([this]
                        {
                            auto dir = pickFolderDialog(
                                editor_.nativeWindowHandle());
                            if (!dir || dir->empty()) return;
                            const std::string name = dir->filename().string();
                            if (name.empty()) return;
                            (void)editor_.newProject(*dir, name);
                        });
                    }

                    if (ImGui::MenuItem("Open..."))
                    {
                        editor_.enqueue([this]
                        {
                            auto p = openFileDialog(
                                editor_.nativeWindowHandle(),
                                { { "Lux Project", "luxproject" } });
                            if (p && !p->empty()) (void)editor_.openProject(*p);
                        });
                    }

                    if (ImGui::BeginMenu("Recent Projects"))
                    {
                        const auto recents = lux::authoring::loadRecentProjects();
                        if (recents.empty())
                        {
                            ImGui::MenuItem("(none)", nullptr, false, false);
                        }
                        else
                        {
                            for (const auto& p : recents)
                            {
                                if (ImGui::MenuItem(p.string().c_str()))
                                    editor_.enqueue(
                                        [this, p]{ (void)editor_.openProject(p); });
                            }
                        }
                        ImGui::EndMenu();
                    }

                    ImGui::Separator();

                    const bool have_project = editor_.currentProject() != nullptr;
                    if (ImGui::MenuItem("Save Manifest", nullptr, false,
                                        have_project))
                        (void)editor_.saveProject();   // pure file write — safe

                    if (ImGui::MenuItem("Save Layout", nullptr, false,
                                        have_project))
                    {
                        if (editor_.currentProject())
                        {
                            // 用户主动点了"保存布局",失败必须说 —— 菜单点完没
                            // 反应会被当成保存成功。
                            const auto path = editor_.currentProject()->layoutPath();
                            if (const auto ec = editor_.uiSystem().saveLayoutToFile(path))
                            {
                                std::fprintf(stderr,
                                    "[LuxEditor] 保存布局失败:%s (%s)\n",
                                    path.string().c_str(), ec.message().c_str());
                            }
                        }
                    }

                    if (ImGui::MenuItem("Reset Layout", nullptr, false,
                                        have_project))
                    {
                        // Drop the file and the in-memory ini so ImGui
                        // falls back to per-window FirstUseEver
                        // defaults on the next paint. Autosave stays
                        // engaged so the freshly reset layout will be
                        // re-persisted as the user rearranges panels.
                        if (editor_.currentProject())
                        {
                            std::error_code rm_ec;
                            std::filesystem::remove(
                                editor_.currentProject()->layoutPath(), rm_ec);
                        }
                        editor_.uiSystem().clearLayout();
                    }

                    if (ImGui::MenuItem("Close Project", nullptr, false,
                                        have_project))
                        editor_.enqueue(
                            [this]{ editor_.closeProject(); });

                    // Switch Project... — spawn the launcher and quit
                    // this editor instance. Keeps inline switching as
                    // an option (Open / New / Recent above) but gives
                    // users a "restart in a clean editor process"
                    // path that avoids any tearDown/bringUp risk in
                    // the running process.
                    if (ImGui::MenuItem("Switch Project..."))
                        editor_.enqueue(
                            [this]
                            {
                                if (!lux::launcher::spawnLauncher())
                                {
                                    std::fprintf(stderr,
                                        "[LuxEditor] Switch Project failed: "
                                        "could not spawn lux_launcher.exe\n");
                                    return;   // keep editor running
                                }
                                editor_.requestQuit();
                            });

                    ImGui::EndMenu();
                }

                if (ImGui::BeginMenu("Scene"))
                {
                    // New scene = pick a path, select one presentation
                    // contribution in LXWA, then open it.
                    const auto new_scene = [this](
                        bool spatial_2d,
                        const char* def_name)
                    {
                        editor_.enqueue([this, spatial_2d, def_name]
                        {
                            const std::filesystem::path def =
                                editor_.currentProject()
                                    ? editor_.currentProject()->worldsRoot()
                                    : std::filesystem::path{};
                            auto p = saveFileDialog(
                                editor_.nativeWindowHandle(),
                                { { "Lux World", "luxworld" } }, def, def_name);
                            if (p && !p->empty())
                                (void)editor_.newScene(*p, spatial_2d);
                        });
                    };
                    if (ImGui::MenuItem("New 3D Scene..."))
                        new_scene(false, "New3D.luxworld");
                    if (ImGui::MenuItem("New 2D Scene..."))
                        new_scene(true, "New2D.luxworld");
                    ImGui::Separator();

                    if (ImGui::MenuItem("Open..."))
                    {
                        editor_.enqueue([this]
                        {
                            const std::filesystem::path def =
                                editor_.currentProject()
                                    ? editor_.currentProject()->worldsRoot()
                                    : std::filesystem::path{};
                            auto p = openFileDialog(
                                editor_.nativeWindowHandle(),
                                { { "Lux World", "luxworld" } }, def);
                            if (p && !p->empty()) (void)editor_.openScene(*p);
                        });
                    }

                    const bool have_scene = editor_.currentScene() != nullptr;
                    const bool have_scene_path =
                        have_scene && !editor_.currentScenePath().empty();
                    if (ImGui::MenuItem("Save", nullptr, false, have_scene_path))
                    {
                        // 此前是 `(void)editor_.saveScene();` —— 用户**主动点了保存**,
                        // 返回值被显式丢弃,失败时连 stderr 都没有:界面上与保存成功
                        // 完全一样。saveScene 是 [[nodiscard]] 的,那个 (void) 正是
                        // 在压制编译器的提醒。
                        if (!editor_.saveScene())
                            lux::log::error("editor", "保存场景失败:{}",
                                            editor_.currentScenePath().string());
                        else
                            editor_.toasts().push("场景已保存", ToastLevel::Success);
                    }

                    if (ImGui::MenuItem("Save As...", nullptr, false, have_scene))
                    {
                        editor_.enqueue([this]
                        {
                            std::filesystem::path def_dir;
                            std::string           def_name = "Main.luxworld";
                            if (!editor_.currentScenePath().empty())
                            {
                                def_dir  = editor_.currentScenePath().parent_path();
                                def_name = editor_.currentScenePath().filename().string();
                            }
                            else if (editor_.currentProject())
                            {
                                def_dir = editor_.currentProject()->worldsRoot();
                            }
                            auto p = saveFileDialog(
                                editor_.nativeWindowHandle(),
                                { { "Lux World", "luxworld" } },
                                def_dir,
                                def_name);
                            if (p && !p->empty()) (void)editor_.saveSceneAs(*p);
                        });
                    }

                    ImGui::EndMenu();
                }

                ImGui::Separator();

                // Import an external model/texture into the project's Content/.
                // Enabled only when a project is open (we need a content root).
                if (ImGui::MenuItem("Import Asset...", nullptr, false,
                                    editor_.currentProject() != nullptr))
                {
                    editor_.enqueue([this]
                    {
                        const std::filesystem::path def =
                            editor_.currentProject()
                                ? editor_.currentProject()->contentRoot()
                                : std::filesystem::path{};
                        // Filter derived from the importer registry (audit 7.2:
                        // the dialog can no longer offer a format the importer
                        // rejects). One "Importable" group, ext list built from
                        // AssetImporter::importableExtensions().
                        std::string exts;
                        for (auto e : lux::editor::importableExtensions())
                        {
                            if (!exts.empty()) exts += ',';
                            exts += e.substr(1);   // strip the leading dot
                        }
                        auto p = openFileDialog(
                            editor_.nativeWindowHandle(),
                            { { "Importable asset", exts.c_str() } },
                            def);
                        if (p && !p->empty()) editor_.importExternalAsset(*p);
                    });
                }

                // Cook the project's whole Content/ into a .luxpak (v1 cook =
                // whole folder, matching the editor's own load set). Same
                // in-process function the lux_asset_packer CLI uses.
                if (ImGui::MenuItem("Cook Content (.luxpak)", nullptr, false,
                                    editor_.currentProject() != nullptr))
                {
                    editor_.enqueue([this]
                    {
                        editor_.cookProjectContent();
                    });
                }

                ImGui::Separator();

                if (ImGui::MenuItem("Exit"))
                    editor_.requestQuit();

                ImGui::EndMenu();
            }

            // ── Window menu — toggle/summon panels (incl. new ones absent from a
            //    saved imgui layout). Checkbox drives Panel::setVisible; UISystem
            //    honors it (skips painting hidden panels).
            if (ImGui::BeginMenu("Window"))
            {
                // Every registered window gets a visibility checkbox — no hardcoded
                // list, so a newly registered panel (built-in or plugin) appears here
                // automatically. Display label is the panel's live title().
                auto tools = editor_.tools();
                for (const auto& panel : tools.snapshot())
                {
                    if (!panel.active)
                        continue;
                    bool visible = panel.visible;
                    if (ImGui::MenuItem(
                            panel.display_name.c_str(),
                            nullptr,
                            &visible))
                    {
                        (void)tools.requestVisible(
                            panel.panel.view(),
                            visible);
                    }
                }
                ImGui::EndMenu();
            }

            // (Entity creation moved OUT of the menu bar, per user ruling:
            // it lives where mature editors put it, in the Hierarchy panel's `+`
            // button / right-click and the viewport right-click, backed by the
            // SpawnRegistry. See LuxEditor::drawSpawnMenuItems.)

            // ── Run menu — editor Edit/Play. A top-level menu (ALWAYS visible, same
            //    mechanism as File/Window) so it renders regardless of scene state;
            //    Play enables only once a scene is open. The scene snapshots itself on
            //    Play and restores on Stop; scripts + sim run only while playing.
            //    Enqueued (deferred) — enter/exit mutate the World + do file I/O, which
            //    is unsafe inside the live ImGui frame this hook paints in.
            if (ImGui::BeginMenu("Run"))
            {
                const bool have_scene = editor_.currentScene() != nullptr;
                const bool playing    = editor_.isPlaying();

                if (ImGui::MenuItem("Play", nullptr, false, have_scene && !playing))
                    editor_.enqueue([this]{ editor_.enterPlayMode(); });
                if (ImGui::MenuItem("Stop", nullptr, false, playing))
                    editor_.enqueue([this]{ editor_.exitPlayMode(); });

                ImGui::Separator();
                if (!have_scene)
                    ImGui::TextDisabled("Open a scene first (File > Scene)");
                else
                    ImGui::TextDisabled(playing ? "State: Playing" : "State: Editing");

                ImGui::EndMenu();
            }

            ImGui::EndMainMenuBar();
        }

        // Import Options modal (mounted at top level). Filesystem-path picking
        // is handled by native OS dialogs from the menu items above, so there is
        // no in-ImGui path prompt to mount here anymore.
        editor_.importController().paintDialog();

        // 资产删除确认对话框(列引用者 + 强删)—— 同为顶层挂载的 modal;
        // paint 只置位,执行在主循环 frame-OPEN 段的 tick。
        editor_.assetDeleteController().paintDialog();
    }

} // namespace lux::editor
