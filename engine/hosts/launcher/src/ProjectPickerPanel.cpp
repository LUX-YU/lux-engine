#include <lux/engine/hosts/launcher/ProjectPickerPanel.hpp>

#include <lux/engine/editor/scene/DemoSceneTemplate.hpp>
#include <lux/engine/authoring/project/Project.hpp>
#include <lux/engine/authoring/project/RecentProjects.hpp>

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace lux::launcher
{
    namespace
    {
        void seedBuffer(std::array<char, 1024>& buf, std::string_view text)
        {
            buf.fill(0);
            const std::size_t n = std::min(text.size(), buf.size() - 1);
            std::memcpy(buf.data(), text.data(), n);
            buf[n] = '\0';
        }
    }

    ProjectPickerPanel::ProjectPickerPanel(
        OnChosen on_chosen,
        const lux::ecs::ComponentTypeCatalog& components)
        : Panel("Lux Launcher", {800.f, 540.f})
        , on_chosen_(std::move(on_chosen))
        , components_(components)
    {
    }

    void ProjectPickerPanel::emit(const std::filesystem::path& p)
    {
        if (on_chosen_)
            on_chosen_(p);
    }

    void ProjectPickerPanel::paint()
    {
        // Refresh the recent list every paint — the editor may have
        // pushed during this launcher's lifetime (rare but possible).
        recents_cache_ = lux::authoring::loadRecentProjects();

        ImGui::TextUnformatted("Welcome to Lux Editor");
        ImGui::Separator();
        ImGui::Spacing();

        // ── Recent projects list ────────────────────────────────────
        ImGui::TextUnformatted("Recent Projects");
        ImGui::Spacing();

        // Reserve space for the action-button row at the bottom (~50 px).
        const float footer_h = 60.0f;
        ImGui::BeginChild("##recent_list",
                          ImVec2(0, -footer_h),
                          true /* border */);

        if (recents_cache_.empty())
        {
            ImGui::TextDisabled(
                "(no recent projects yet — use 'New Project' or "
                "'Open Project' below)");
        }
        else
        {
            for (const auto& p : recents_cache_)
            {
                // One selectable per row. Double-click semantics would
                // be nicer but ImGui's Selectable + IsItemHovered is
                // already enough for M3.5.
                if (ImGui::Selectable(p.string().c_str(),
                                      false /* selected */,
                                      ImGuiSelectableFlags_AllowDoubleClick))
                {
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    {
                        emit(p);
                        ImGui::EndChild();
                        return;
                    }
                }
            }
            ImGui::Spacing();
            ImGui::TextDisabled(
                "(double-click a recent project to open)");
        }
        ImGui::EndChild();

        ImGui::Spacing();

        // ── Action buttons row ──────────────────────────────────────
        const float button_w = 180.f;
        if (ImGui::Button("New Project...", ImVec2(button_w, 36.f)))
        {
            modal_                = Modal::NewProject;
            modal_open_requested_ = true;
            modal_error_.clear();
            path_buffer_.fill(0);
        }
        ImGui::SameLine();
        if (ImGui::Button("New Demo Project...", ImVec2(button_w, 36.f)))
        {
            modal_                = Modal::NewDemoProject;
            modal_open_requested_ = true;
            modal_error_.clear();
            path_buffer_.fill(0);
        }
        ImGui::SameLine();
        if (ImGui::Button("Open Project...", ImVec2(button_w, 36.f)))
        {
            modal_                = Modal::OpenProject;
            modal_open_requested_ = true;
            modal_error_.clear();
            path_buffer_.fill(0);
        }

        paintModal();
    }

    void ProjectPickerPanel::paintModal()
    {
        if (modal_ == Modal::None)
            return;

        // OpenPopup must be called at the same nesting level as
        // BeginPopupModal — outside any nested ImGui::Begin, which is
        // satisfied here because the panel is the outermost window's
        // body.
        const char* title =
            (modal_ == Modal::NewProject)     ? "New Project"
          : (modal_ == Modal::NewDemoProject) ? "New Demo Project"
                                              : "Open Project";

        if (modal_open_requested_)
        {
            ImGui::OpenPopup(title);
            modal_open_requested_ = false;
        }

        // Centre the modal on the viewport for visibility.
        const auto centre = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing,
                                ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal(title, nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            if (modal_ == Modal::NewProject)
            {
                ImGui::TextUnformatted(
                    "Target directory (will be created if missing).\n"
                    "Project name is the directory basename.\n"
                    "Result: an empty project skeleton (no scene).");
            }
            else if (modal_ == Modal::NewDemoProject)
            {
                ImGui::TextUnformatted(
                    "Target directory (will be created if missing).\n"
                    "Project name is the directory basename.\n"
                    "Result: project skeleton + Worlds/Main.luxworld\n"
                    "with a cube on a floor under a directional sun.");
            }
            else
            {
                ImGui::TextUnformatted(
                    "Path to the project's .luxproject file:");
            }

            ImGui::PushItemWidth(560.f);
            ImGui::InputText("##path", path_buffer_.data(),
                             path_buffer_.size());
            ImGui::PopItemWidth();

            if (!modal_error_.empty())
            {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "%s",
                                   modal_error_.c_str());
            }
            ImGui::Spacing();

            const float bw = 120.f;
            bool accepted = false;
            std::filesystem::path resolved;

            if (ImGui::Button("OK", ImVec2(bw, 0.f)))
            {
                const std::filesystem::path raw(path_buffer_.data());
                if (raw.empty())
                {
                    modal_error_ = "Path cannot be empty.";
                }
                else if (modal_ == Modal::NewProject ||
                         modal_ == Modal::NewDemoProject)
                {
                    const auto name = raw.filename().string();
                    if (name.empty())
                    {
                        modal_error_ =
                            "Cannot derive project name from path.";
                    }
                    else
                    {
                        // Step 1 — create the empty project skeleton.
                        auto created = lux::authoring::Project::newOnDisk(
                            raw, name);
                        if (!created)
                        {
                            modal_error_ = created.error();
                        }
                        else if (modal_ == Modal::NewDemoProject)
                        {
                            // Step 2 — write the demo scene into the
                            // project's Worlds/ directory.
                            const auto scene_rel = std::filesystem::path(
                                "Worlds") / "Main.luxworld";
                            const auto scene_abs =
                                created->root() / scene_rel;
                            if (!lux::editor::writeDemoScene(
                                    scene_abs, components_))
                            {
                                modal_error_ =
                                    "Created project but failed to write "
                                    "demo scene.";
                            }
                            else
                            {
                                // Step 3 — point the manifest at it so
                                // the editor opens the scene by default.
                                created->manifest().default_world =
                                    "Worlds/Main.luxworld";
                                if (auto saved = created->saveManifest();
                                    !saved)
                                {
                                    modal_error_ =
                                        "Demo scene written, but failed to "
                                        "update manifest's default_world: " +
                                        saved.error();
                                }
                                else
                                {
                                    resolved = created->manifestPath();
                                    accepted = true;
                                }
                            }
                        }
                        else
                        {
                            resolved = created->manifestPath();
                            accepted = true;
                        }
                    }
                }
                else
                {
                    // Open: just validate the path exists & is a .luxproject.
                    if (!std::filesystem::exists(raw))
                    {
                        modal_error_ = "File does not exist.";
                    }
                    else if (raw.extension() != ".luxproject")
                    {
                        modal_error_ = "Not a .luxproject file.";
                    }
                    else
                    {
                        resolved = raw;
                        accepted = true;
                    }
                }

                if (accepted)
                {
                    ImGui::CloseCurrentPopup();
                    modal_ = Modal::None;
                    emit(resolved);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(bw, 0.f)))
            {
                ImGui::CloseCurrentPopup();
                modal_ = Modal::None;
                modal_error_.clear();
            }

            ImGui::EndPopup();
        }
    }

} // namespace lux::launcher
