#include <lux/engine/editor/ProjectCreationPane.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <random>

namespace lux::editor
{
    struct ProjectCreationPane::Impl final
    {
        void* receiver;
        void (*open_project)(void*, OpenProjectRequest&) noexcept;
        void (*create_project)(void*, CreateProjectRequest&) noexcept;
        std::vector<ProjectManifest::PluginRecord> available;
        ui::Layout layout;
        ui::Label help{"New project: choose an empty directory and plugins. Open: choose a .luxproj file."};
        ui::TextEdit path, name{"My Project"};
        ui::Button create{"Create project"}, open{"Open project"};
        ui::Label status;
        std::vector<std::unique_ptr<ui::CheckBox>> plugins;
        std::vector<object::Connection> connections;

        Impl(
            void* receiver,
            void (*open)(void*, OpenProjectRequest&) noexcept,
            void (*create)(void*, CreateProjectRequest&) noexcept,
            std::vector<ProjectManifest::PluginRecord> available
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
                    plugin.id + " v" + std::to_string(plugin.version),
                    plugin.id == "lux.builtin.scene_render"
                ));
                add(*plugins.back());
            }
            add(this->create);
            add(this->open);
            add(status);
        }
    };

    ProjectCreationPane::ProjectCreationPane(
        void* receiver,
        void (*open)(void*, OpenProjectRequest&) noexcept,
        void (*create)(void*, CreateProjectRequest&) noexcept,
        std::vector<ProjectManifest::PluginRecord> available
    )
        : Pane("Project"), impl_(std::make_unique<Impl>(receiver, open, create, std::move(available)))
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
        retain(connect(&impl_->create, &ui::Button::activated, this, [this]() noexcept { createProject(); }));
        retain(connect(&impl_->open, &ui::Button::activated, this, [this]() noexcept { openProject(); }));
    }

    ProjectCreationPane::~ProjectCreationPane() noexcept = default;

    void ProjectCreationPane::setStatus(std::string text) noexcept
    {
        impl_->status.setText(std::move(text));
    }

    void ProjectCreationPane::createProject() noexcept
    {
        std::mt19937 random{std::random_device{}()};
        uuids::uuid_random_generator generate{random};
        ProjectManifest manifest{1, generate(), impl_->name.value()};
        for (std::size_t i = 0; i < impl_->plugins.size(); ++i)
        {
            if (impl_->plugins[i]->value())
            {
                manifest.plugins.push_back(impl_->available[i]);
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
