#include "ProductCommands.hpp"
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/editor/launcher/LaunchEditor.hpp>
#include <lux/engine/editor/material/MaterialEditor.hpp>
#include <lux/engine/editor/scene/SceneEditor.hpp>
#include <lux/engine/editor/flowforge/FlowForgeEditor.hpp>
#include <lux/engine/editor/detail/TaskResult.hpp>
#include <lux/engine/editor/detail/SignalDelivery.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/platform/Process.hpp>
#include <lux/engine/window/FileDialog.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/ui/Root.hpp>
#include <toml++/toml.hpp>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <random>

namespace lux::editor
{
    namespace
    {
        using RecentProjects = std::vector<std::filesystem::path>;
        std::string utf8(const std::filesystem::path& path)
        {
            const auto text = path.u8string();
            return {text.begin(), text.end()};
        }
        EditorResult<RecentProjects> rememberProject(const std::filesystem::path& project)
        {
            const auto directory = engine::platform::userConfigDirectory();
            if (!directory)
                return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "recent.directory"});
            const auto file = *directory / "lux/editor/recent-projects.toml";
            std::error_code error;
            auto canonical = std::filesystem::weakly_canonical(project, error);
            if (error)
                return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "recent.path", 0, utf8(project)}
                );
            RecentProjects paths{canonical};
            if (std::filesystem::exists(file, error))
            {
                auto parsed = toml::parse_file(utf8(file));
                if (!parsed || parsed["version"].value_or(0) != 1)
                    return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "recent.format"});
                if (const auto* rows = parsed["projects"].as_array())
                    for (const auto& row : *rows)
                    {
                        const auto value = row.value<std::string>();
                        if (!value || paths.size() >= 20)
                            continue;
                        auto path = std::filesystem::u8path(*value);
                        const bool same = std::filesystem::equivalent(path, canonical, error);
                        error.clear(); // A removed recent project remains selectable and reports its error when opened.
                        if (!same && path != canonical && std::ranges::find(paths, path) == paths.end())
                            paths.push_back(std::move(path));
                    }
            }
            if (error)
                return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "recent.read"});
            std::filesystem::create_directories(file.parent_path(), error);
            if (error)
                return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "recent.directory"});
            toml::array rows;
            for (const auto& path : paths)
                rows.push_back(utf8(path));
            std::random_device random;
            uuids::basic_uuid_random_generator<std::random_device> generate{random};
            const auto staged = file.parent_path() / ("recent-projects." + uuids::to_string(generate()) + ".next");
            {
                std::ofstream output(staged, std::ios::binary | std::ios::trunc);
                output << toml::table{{"version", 1}, {"projects", std::move(rows)}};
                output.flush();
                if (!output)
                    return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "recent.write"});
                output.close();
                if (!output)
                    return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "recent.close"});
            }
            std::filesystem::rename(staged, file, error);
            if (error)
                return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "recent.replace"});
            return paths;
        }
        template <class Tool> EditorResult<void> newAsset(EditorContext& context)
        {
            auto created = Tool::create(context.panes().root(), context.panes().makeId(), context);
            if (!created)
                return lux::cxx::unexpected(created.error());
            if (auto initialized = (*created)->newAsset(); !initialized)
                return initialized;
            auto adopted = context.panes().adopt(std::move(*created));
            return adopted ? EditorResult<void>{} : lux::cxx::unexpected(adopted.error());
        }
        // Only the product owns these commands. Task completion records facts; the host applies them outside dispatch.
        class ProjectCommands final : public std::enable_shared_from_this<ProjectCommands>
        {
        public:
            explicit ProjectCommands(EditorContext& context) : context_(context) {}
            EditorResult<void> initialize()
            {
                if (auto installed = refresh(); !installed)
                    return installed;
                if (auto started = submit(context_.project().projectFile(), false); !started)
                {
                    error_ = started.error();
                    requestRefresh();
                }
                return {};
            }
            EditorResult<void> refresh()
            {
                if (result_)
                {
                    if (*result_)
                        recent_ = std::move(**result_);
                    else
                        error_ = result_->error();
                    result_.reset();
                    task_ = {};
                    busy_ = false;
                }
                std::vector<CommandRegistration> commands;
                for (const auto& existing : context_.commands())
                    if (!existing.id.name().starts_with("lux.product."))
                        commands.push_back(existing);
                const auto add = [&](std::string id, std::string label, std::string menu, auto action) {
                    CommandRegistration entry;
                    entry.id = lux::ui::CommandId{"lux.product." + id};
                    entry.label = std::move(label);
                    entry.menu = std::move(menu);
                    entry.invoke = [self = shared_from_this(), action](EditorContext&, lux::ui::Command& command) {
                        if (command.phase == lux::ui::ECommandPhase::QUERY)
                        {
                            const auto id = command.id.name();
                            const bool starts_project =
                                id == "lux.product.open-project" || id.starts_with("lux.product.recent/");
                            command.enabled = !starts_project || !self->busy_;
                            return EditorResult<void>{};
                        }
                        return action(*self);
                    };
                    commands.push_back(std::move(entry));
                };
                add("refresh", "Refresh product commands", "", [](auto& self) { return self.refresh(); });
                add("default", "Open initial scene", "", [](auto& self) -> EditorResult<void> {
                    const auto& project = self.context_.project().manifest();
                    const auto initial =
                        std::ranges::find(project.assets, project.default_scene, &ProjectAssetEntry::source_path);
                    if (initial == project.assets.end())
                        return {};
                    const auto opened = self.context_.openAsset(initial->id);
                    return opened ? EditorResult<void>{} : lux::cxx::unexpected(opened.error());
                });
                const auto create = [](auto& self, std::string_view type) -> EditorResult<void> {
                    const auto created = self.context_.panes().create(lux::ui::PaneTypeIdView{type});
                    return created ? EditorResult<void>{} : lux::cxx::unexpected(created.error());
                };
                add("new-project", "New Project (new Editor)", "File", [create](auto& self) {
                    return create(self, "lux.editor.project.creation");
                });
                add("open-project", "Open Project (new Editor)...", "File", [](auto& self) -> EditorResult<void> {
                    if (self.busy_)
                        return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "project.open"});
                    const window::FileDialogFilter filters[]{{"Lux Project", "luxproject"}};
                    auto selected = window::openFileDialog(self.context_.panes().root().window(), filters);
                    if (!selected)
                        return lux::cxx::unexpected(
                            EditorFailure{EEditorError::SOURCE_FAILURE, "project.dialog", 0, selected.error().detail}
                        );
                    return *selected ? self.submit(**selected, true) : EditorResult<void>{};
                });
                add("new-scene", "Scene", "File/New Asset", [](auto& self) {
                    return newAsset<scene::SceneEditor>(self.context_);
                });
                add("new-material", "Material", "File/New Asset", [](auto& self) {
                    return newAsset<material::MaterialEditor>(self.context_);
                });
                add("new-flow", "FlowForge", "File/New Asset", [](auto& self) {
                    return newAsset<flowforge::FlowForgeEditor>(self.context_);
                });
                add("settings", "Settings / Layouts", "Edit", [create](auto& self) {
                    return create(self, "lux.editor.settings");
                });
                add("open-asset", "Open Asset...", "File", [create](auto& self) {
                    return create(self, "lux.editor.project");
                });
                for (std::size_t index{}; index < recent_.size(); ++index)
                    add("recent/" + std::to_string(index),
                        utf8(recent_[index]),
                        "File/Recent Projects",
                        [path = recent_[index]](auto& self) { return self.submit(path, true); });
                CommandRegistration version;
                version.id = lux::ui::CommandId{"lux.product.about"};
                version.label = "Lux Editor " LUX_EDITOR_VERSION;
                version.menu = "Help";
                version.invoke = [](EditorContext&, lux::ui::Command& command) -> EditorResult<void> {
                    command.enabled = false;
                    return {};
                };
                commands.push_back(std::move(version));
                auto installed = context_.setCommands(std::move(commands));
                if (!installed)
                    return installed;
                if (error_)
                {
                    auto failure = std::move(*error_);
                    error_.reset();
                    return lux::cxx::unexpected(std::move(failure));
                }
                return {};
            }

        private:
            void requestRefresh() noexcept
            {
                lux::ui::Command command{
                    lux::ui::CommandIdView{"lux.product.refresh"},
                    lux::ui::ECommandPhase::EXECUTE
                };
                static_cast<void>(object::sendEvent(context_.panes().root(), command));
            }
            EditorResult<void> submit(std::filesystem::path file, bool launch)
            {
                if (busy_)
                    return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "project.open"});
                const auto scheduler = context_.execution().blocking();
                if (!scheduler)
                    return lux::cxx::unexpected(EditorFailure{EEditorError::EXECUTION_FAILURE, "project.scheduler"});
                auto admitted = context_.execution().submit(
                    {launch ? "Open project in new Editor" : "Update recent projects", "Editor product"},
                    [scheduler = *scheduler, file = std::move(file), installation = context_.installation(), launch](
                        process::TaskReporter
                    ) noexcept {
                        return stdexec::then(
                            stdexec::schedule(scheduler),
                            [file, installation, launch]() -> EditorResult<RecentProjects> {
                                if (launch)
                                {
                                    // Release the preparation write lease before the child process opens the project.
                                    {
                                        const auto prepared = readProjectOpenData(file);
                                        if (!prepared)
                                            return lux::cxx::unexpected(prepared.error());
                                    }
                                    if (auto started = launchEditor(installation, file); !started)
                                        return lux::cxx::unexpected(started.error());
                                }
                                return rememberProject(file);
                            }
                        );
                    },
                    [this](process::TTaskResult<RecentProjects, EditorFailure>&& result) noexcept {
                        result_.emplace(detail::taskResult(std::move(result)));
                        requestRefresh();
                    }
                );
                if (!admitted)
                    return lux::cxx::unexpected(
                        EditorFailure{EEditorError::EXECUTION_FAILURE, "project.submit", 0, {}, admitted.error()}
                    );
                task_ = std::move(*admitted);
                busy_ = true;
                return {};
            }
            EditorContext& context_;
            RecentProjects recent_;
            std::optional<EditorFailure> error_;
            std::optional<EditorResult<RecentProjects>> result_;
            bool busy_{};
            process::Task task_;
        };
    }
    EditorResult<void> assembleCommands(EditorContext& context) noexcept
    {
        return std::make_shared<ProjectCommands>(context)->initialize();
    }
}
