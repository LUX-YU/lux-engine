#include <algorithm>
#include <lux/engine/editor/ProjectCreationPane.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Root.hpp>
#include <map>
#include <random>
#include <set>

namespace lux::editor
{
    scene::SceneContentAllowance availableProjectContent(
        std::span<const plugins::PluginDescription> declarations,
        std::span<const ProjectManifest::PluginRecord> selected
    )
    {
        std::map<std::string_view, const plugins::PluginDescription*, std::less<>> index;
        for (const auto& declaration : declarations)
        {
            index.emplace(declaration.identity.id, &declaration);
        }
        std::vector<std::string_view> pending;
        for (const auto& plugin : selected)
        {
            pending.push_back(plugin.id);
        }
        std::set<std::string_view> visited;
        std::set<std::pair<std::string, std::uint32_t>> geometries, partitions;
        while (!pending.empty())
        {
            const auto name = pending.back();
            pending.pop_back();
            if (!visited.insert(name).second)
            {
                continue;
            }
            const auto found = index.find(name);
            if (found == index.end())
            {
                continue; // Project preparation reports missing/version-mismatched plugins.
            }
            for (const auto& dependency : found->second->dependencies)
            {
                pending.push_back(dependency.id);
            }
            for (const auto& geometry : found->second->geometries)
            {
                geometries.emplace(geometry.identity.id, geometry.identity.version);
            }
            for (const auto& partition : found->second->partitions)
            {
                partitions.emplace(partition.identity.id, partition.identity.version);
            }
        }
        scene::SceneContentAllowance result;
        for (const auto& [name, version] : geometries)
        {
            result.geometries.push_back({name, version});
        }
        for (const auto& [name, version] : partitions)
        {
            result.partitions.push_back({name, version});
        }
        return result;
    }

    struct ProjectCreationPane::Impl final
    {
        void* receiver;
        void (*open_project)(void*, OpenProjectRequest&) noexcept;
        void (*create_project)(void*, CreateProjectRequest&) noexcept;
        std::vector<plugins::PluginDescription> available;
        ui::Layout layout;
        ui::Label help{
            "New project: choose an empty directory, plugins and allowed content. Open: choose a .luxproj file."
        };
        ui::TextEdit path, name{"My Project"};
        ui::Button create{"Create project"}, open{"Open project"}, action{"Cancel"};
        ui::Label status;
        EApplicationState application_state = EApplicationState::STARTING;
        std::vector<std::unique_ptr<ui::CheckBox>> plugins;
        std::vector<object::Connection> connections;
        scene::SceneContentAllowance offered;
        std::vector<std::unique_ptr<ui::CheckBox>> geometries, partitions;

        std::vector<ProjectManifest::PluginRecord> selectedPlugins() const
        {
            std::vector<ProjectManifest::PluginRecord> result;
            for (std::size_t i = 0; i < plugins.size(); ++i)
            {
                if (plugins[i]->value())
                {
                    result.push_back({available[i].identity.id, available[i].identity.version});
                }
            }
            return result;
        }

        Impl(
            void* receiver,
            void (*open)(void*, OpenProjectRequest&) noexcept,
            void (*create)(void*, CreateProjectRequest&) noexcept,
            std::vector<plugins::PluginDescription> available
        ) noexcept
            : receiver(receiver), open_project(open), create_project(create), available(std::move(available))
        {
            path.setHint("Absolute directory or .luxproj path");
            name.setHint("Project name");
            help.setWrap(true);
            const auto add = [&](auto& element)
            {
                if (!layout.addElement(element))
                {
                    std::terminate();
                }
            };
            add(help);
            add(path);
            add(name);
            for (const auto& plugin : this->available)
            {
                plugins.push_back(std::make_unique<ui::CheckBox>(
                    plugin.identity.id + " v" + std::to_string(plugin.identity.version),
                    true
                ));
                add(*plugins.back());
            }
            offered = availableProjectContent(this->available, selectedPlugins());
            for (const auto& geometry : offered.geometries)
            {
                geometries.push_back(std::make_unique<ui::CheckBox>("Allow geometry: " + geometry.name, true));
                add(*geometries.back());
            }
            for (const auto& partition : offered.partitions)
            {
                partitions.push_back(std::make_unique<ui::CheckBox>("Allow partition: " + partition.name, true));
                add(*partitions.back());
            }
            add(this->create);
            add(this->open);
            add(status);
            add(action);
            action.setVisible(false);
        }
    };

    ProjectCreationPane::ProjectCreationPane(
        void* receiver,
        void (*open)(void*, OpenProjectRequest&) noexcept,
        void (*create)(void*, CreateProjectRequest&) noexcept,
        std::vector<plugins::PluginDescription> available,
        void (*action)(void*) noexcept
    )
        : Pane("Project selection"), impl_(std::make_unique<Impl>(receiver, open, create, std::move(available)))
    {
        if (!addElement(impl_->layout))
        {
            std::terminate();
        }
        const auto retain = [&](auto connection)
        {
            if (!connection)
            {
                std::terminate();
            }
            impl_->connections.push_back(std::move(*connection));
        };
        for (const auto& plugin : impl_->plugins)
        {
            retain(connect(
                plugin.get(),
                &ui::CheckBox::edited,
                this,
                [this](ui::EditResult) noexcept { refreshContent(); }
            ));
        }
        refreshContent();
        retain(connect(&impl_->create, &ui::Button::activated, this, [this]() noexcept { createProject(); }));
        retain(connect(&impl_->open, &ui::Button::activated, this, [this]() noexcept { openProject(); }));
        if (action)
        {
            retain(connect(
                &impl_->action,
                &ui::Button::activated,
                this,
                [receiver, action]() noexcept { action(receiver); }
            ));
        }
    }

    ProjectCreationPane::~ProjectCreationPane() noexcept = default;

    void ProjectCreationPane::setStatus(std::string text) noexcept
    {
        impl_->status.setText(std::move(text));
    }

    void ProjectCreationPane::setApplicationState(EApplicationState state) noexcept
    {
        impl_->application_state = state;
        if (auto* root = attachedRoot())
        {
            root->deferChange(
                *this,
                +[](object::LuxObject& pane) noexcept { static_cast<ProjectCreationPane&>(pane).applyState(); }
            );
        }
        else
        {
            applyState();
        }
    }

    void ProjectCreationPane::applyState() noexcept
    {
        const auto state = impl_->application_state;
        const bool loading = state == EApplicationState::LOADING_PROJECT;
        const bool failed =
            state == EApplicationState::PROJECT_CLEANUP_FAILED || state == EApplicationState::CLOSE_FAILED;
        const bool busy = loading || failed || state == EApplicationState::CLOSING;
        const std::string action = loading ? "Cancel loading" : failed ? "Retry cleanup" : "";
        setModal(state != EApplicationState::RUNNING);
        impl_->path.setEnabled(!busy);
        impl_->name.setEnabled(!busy);
        impl_->open.setEnabled(!busy);
        impl_->create.setEnabled(!busy);
        for (const auto& plugin : impl_->plugins)
        {
            plugin->setEnabled(!busy);
        }
        if (busy)
        {
            for (const auto& geometry : impl_->geometries)
            {
                geometry->setEnabled(false);
            }
            for (const auto& partition : impl_->partitions)
            {
                partition->setEnabled(false);
            }
        }
        else
        {
            refreshContent();
        }
        impl_->action.setVisible(!action.empty());
        impl_->action.setText(std::move(action));
    }

    void ProjectCreationPane::refreshContent() noexcept
    {
        const auto allowed = availableProjectContent(impl_->available, impl_->selectedPlugins());
        for (std::size_t i = 0; i < impl_->geometries.size(); ++i)
        {
            impl_->geometries[i]->setEnabled(
                std::ranges::find(allowed.geometries, impl_->offered.geometries[i]) != allowed.geometries.end()
            );
        }
        for (std::size_t i = 0; i < impl_->partitions.size(); ++i)
        {
            impl_->partitions[i]->setEnabled(
                std::ranges::find(allowed.partitions, impl_->offered.partitions[i]) != allowed.partitions.end()
            );
        }
    }

    void ProjectCreationPane::createProject() noexcept
    {
        std::mt19937 random{std::random_device{}()};
        uuids::uuid_random_generator generate{random};
        ProjectManifest manifest{2, generate(), impl_->name.value()};
        manifest.plugins = impl_->selectedPlugins();
        const auto allowed = availableProjectContent(impl_->available, manifest.plugins);
        manifest.content.emplace();
        for (std::size_t i = 0; i < impl_->geometries.size(); ++i)
        {
            const auto& identity = impl_->offered.geometries[i];
            if (impl_->geometries[i]->value() &&
                std::ranges::find(allowed.geometries, identity) != allowed.geometries.end())
            {
                manifest.content->geometries.push_back(identity);
            }
        }
        for (std::size_t i = 0; i < impl_->partitions.size(); ++i)
        {
            const auto& identity = impl_->offered.partitions[i];
            if (impl_->partitions[i]->value() &&
                std::ranges::find(allowed.partitions, identity) != allowed.partitions.end())
            {
                manifest.content->partitions.push_back(identity);
            }
        }
        CreateProjectRequest request{std::filesystem::u8path(impl_->path.value()), std::move(manifest)};
        static_cast<void>(impl_->create_project(impl_->receiver, request));
        impl_->status.setText(request.rejection.type ? error::format(request.rejection) : "Preparing project...");
    }

    void ProjectCreationPane::openProject() noexcept
    {
        OpenProjectRequest request{std::filesystem::u8path(impl_->path.value())};
        static_cast<void>(impl_->open_project(impl_->receiver, request));
        impl_->status.setText(request.rejection.type ? error::format(request.rejection) : "Opening project...");
    }
} // namespace lux::editor
