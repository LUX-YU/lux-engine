#include "app/EditorMenuBar.hpp"
#include "app/ImportController.hpp"   // importController().paintDialog()

#include <lux/engine/editor/app/LuxEditor.hpp>     // the editor command surface + accessors
#include <lux/engine/editor/app/FileDialog.hpp>    // pick/open/save native dialogs
#include <lux/engine/launcher/SpawnHelpers.hpp>    // spawnLauncher (Switch Project)
#include <lux/engine/project/RecentProjects.hpp>   // loadRecentProjects
#include <lux/engine/asset/PakCook.hpp>            // cookDirectoryToPak (Cook Content)
#include <lux/engine/ui/UISystem.hpp>              // saveLayoutToFile / clearLayout
#include <lux/engine/ui/Panel.hpp>                 // Window menu: title / isVisible / setVisible

#include <imgui.h>

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
                        const auto recents = lux::project::loadRecentProjects();
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
                            editor_.uiSystem().saveLayoutToFile(
                                editor_.currentProject()->layoutPath());
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
                    if (ImGui::MenuItem("Open..."))
                    {
                        editor_.enqueue([this]
                        {
                            const std::filesystem::path def =
                                editor_.currentProject()
                                    ? editor_.currentProject()->scenesRoot()
                                    : std::filesystem::path{};
                            auto p = openFileDialog(
                                editor_.nativeWindowHandle(),
                                { { "Lux Scene", "luxscene" } }, def);
                            if (p && !p->empty()) (void)editor_.openScene(*p);
                        });
                    }

                    const bool have_scene = editor_.currentScene() != nullptr;
                    const bool have_scene_path =
                        have_scene && !editor_.currentScenePath().empty();
                    if (ImGui::MenuItem("Save", nullptr, false, have_scene_path))
                        (void)editor_.saveScene();

                    if (ImGui::MenuItem("Save As...", nullptr, false, have_scene))
                    {
                        editor_.enqueue([this]
                        {
                            std::filesystem::path def_dir;
                            std::string           def_name = "Main.luxscene";
                            if (!editor_.currentScenePath().empty())
                            {
                                def_dir  = editor_.currentScenePath().parent_path();
                                def_name = editor_.currentScenePath().filename().string();
                            }
                            else if (editor_.currentProject())
                            {
                                def_dir = editor_.currentProject()->scenesRoot();
                            }
                            auto p = saveFileDialog(
                                editor_.nativeWindowHandle(),
                                { { "Lux Scene", "luxscene" } }, def_dir, def_name);
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
                        auto p = openFileDialog(
                            editor_.nativeWindowHandle(),
                            // assimp reads many formats; expose the ones users
                            // actually have. .mtl is NOT listed: it is a material
                            // companion to .obj, loaded automatically when the
                            // .obj that references it is imported — you pick the
                            // .obj, not the .mtl. (.blend support is Blender-
                            // version-sensitive in assimp.)
                            { { "3D model", "gltf,glb,fbx,obj,dae,blend,ply,stl,3ds,3mf,x,lwo,lws,ms3d,ase,ifc,dxf,off" },
                              { "Texture",  "png,jpg,jpeg,tga,bmp,hdr,ktx,dds,exr,psd,gif" } },
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
                        auto* project = editor_.currentProject();
                        if (!project) return;
                        const auto out_dir =
                            project->contentRoot().parent_path() / "Cooked";
                        std::error_code mk_ec;
                        std::filesystem::create_directories(out_dir, mk_ec);
                        const auto out_pak = out_dir
                            / (project->manifest().name + ".luxpak");
                        auto cooked = lux::asset::cookDirectoryToPak(
                            project->contentRoot(), out_pak, "/Game");
                        if (cooked)
                            std::fprintf(stderr,
                                "[LuxEditor] cooked %zu asset(s) -> %s\n",
                                cooked.value().asset_count,
                                out_pak.string().c_str());
                        else
                            std::fprintf(stderr,
                                "[LuxEditor] cook FAILED: %s\n",
                                cooked.error().c_str());
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
                for (const auto& w : editor_.panels().windows())
                {
                    bool vis = w.panel->isVisible();
                    if (ImGui::MenuItem(w.panel->title().c_str(), nullptr, &vis))
                        w.panel->setVisible(vis);
                }
                ImGui::EndMenu();
            }

            ImGui::EndMainMenuBar();
        }

        // Import Options modal (mounted at top level). Filesystem-path picking
        // is handled by native OS dialogs from the menu items above, so there is
        // no in-ImGui path prompt to mount here anymore.
        editor_.importController().paintDialog();
    }

} // namespace lux::editor
