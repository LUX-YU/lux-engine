#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/ProjectScenes.hpp>
#include <lux/engine/editor/SceneCreationPane.hpp>
#include <lux/engine/project/SceneRegistrations.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Layout.hpp>

#include <algorithm>
#include <random>

namespace lux::editor
{
    struct SceneCreationPane::Impl final
    {
        EditorContext& context;
        ProjectScenes& scenes;
        ui::Layout layout;
        ui::Label heading{"Create Scene: select authored schemas and systems"};
        ui::TextEdit name{"New Scene"};
        ui::TextEdit path{"Scenes/NewScene.luxscene"};
        ui::Choice catalog{{}};
        ui::Choice profiles{{}};
        ui::Layout configuration_layout;

        struct Configuration final
        {
            std::vector<std::byte> bytes;
            bool valid = true;
            const SceneConfigurationEditor* editor = nullptr;
            std::unique_ptr<ui::Element> widget;
        };

        std::vector<Configuration> configurations;
        ui::Button create{"Create Scene"}, open{"Open selected Scene"};
        ui::Label status;
        std::vector<std::unique_ptr<ui::CheckBox>> schemas, simulations, systems;
        std::vector<world::WorldDataSchemaId> schema_ids;
        std::vector<ProjectManifest::SceneRecord> catalog_records;
        std::vector<object::Connection> connections;
        std::mt19937 random{std::random_device{}()};
        bool preset_changed = false;
        std::shared_ptr<const scene::SceneAsset> preset;
        std::shared_ptr<const simulation::SimulationAsset> simulation_preset;

        Impl(EditorContext& context, ProjectScenes& scenes) noexcept : context(context), scenes(scenes)
        {
            layout.setScrollable(false, true);
            name.setHint("Scene name");
            path.setHint("Project-relative Scene file");
            std::vector<ui::ChoiceOption> profile_options;
            for (const auto& profile : context.sceneProfiles().profiles())
            {
                profile_options.push_back({static_cast<std::int64_t>(profile_options.size()), profile.display_name});
            }
            profiles.setOptions(std::move(profile_options));
            const auto add = [&](ui::Element& element)
            {
                if (!layout.addElement(element))
                {
                    std::terminate();
                }
            };
            add(catalog);
            add(open);
            add(heading);
            add(name);
            add(path);
            add(profiles);
            const auto& registrations = context.sceneRegistrations();
            for (const auto& schema : registrations.components.all())
            {
                const bool authored =
                    schema.semantic_kind != simulation::ecs::EComponentSemanticKind::RUNTIME_DERIVED &&
                    schema.snapshot == simulation::ecs::EComponentSnapshotPolicy::COPY;
                if (!authored)
                {
                    continue;
                }
                schema_ids.push_back(world::worldDataSchemaId(schema.id.name));
                schemas.push_back(std::make_unique<ui::CheckBox>("Schema: " + schema.id.name, true));
                add(*schemas.back());
            }
            if (registrations.simulation_systems)
            {
                for (const auto& registration : registrations.simulation_systems->all())
                {
                    simulations.push_back(std::make_unique<ui::CheckBox>("Simulation: " + registration.type.name));
                    add(*simulations.back());
                }
            }
            for (const auto& registration : registrations.scene_systems)
            {
                systems.push_back(std::make_unique<ui::CheckBox>("Scene: " + registration.type.name));
                add(*systems.back());
            }
            add(configuration_layout);
            add(create);
            add(status);
            selectPreset();
        }

        void selectPreset() noexcept
        {
            preset.reset();
            simulation_preset.reset();
            const auto available = context.sceneProfiles().profiles();
            const auto index = static_cast<std::size_t>(profiles.value());
            if (index >= available.size())
            {
                prepareConfigurations();
                return;
            }
            uuids::uuid_random_generator generate{random};
            auto defaults = available[index].create(
                {asset::AssetId{generate()}, "Creation defaults", context.sceneRegistrations(), {}}
            );
            if (!defaults)
            {
                status.setText(describeProjectSceneFailure(defaults.error()));
                prepareConfigurations();
                return;
            }
            preset = std::move(defaults->scene);
            simulation_preset = std::move(defaults->simulation);
            for (std::size_t i = 0; i < schemas.size(); ++i)
            {
                schemas[i]->setValue(
                    std::ranges::find(defaults->world->data().schemas(), schema_ids[i]) !=
                    defaults->world->data().schemas().end()
                );
            }
            const auto& registrations = context.sceneRegistrations();
            for (std::size_t i = 0; i < simulations.size(); ++i)
            {
                bool selected = false;
                for (std::size_t j = 0; j < simulation_preset->data().systemCount(); ++j)
                {
                    selected |=
                        simulation_preset->data().systemAt(j).type() == registrations.simulation_systems->all()[i].type;
                }
                simulations[i]->setValue(selected);
            }
            for (std::size_t i = 0; i < systems.size(); ++i)
            {
                bool selected = false;
                for (std::size_t j = 0; j < preset->data().systemCount(); ++j)
                {
                    selected |= preset->data().systemAt(j).type() == registrations.scene_systems[i].type;
                }
                systems[i]->setValue(selected);
            }
            prepareConfigurations();
            status.setText("Preset applied; adjust schemas and systems before creation");
        }

        void prepareConfigurations() noexcept
        {
            configurations.clear();
            for (const auto& registration : context.sceneRegistrations().scene_systems)
            {
                auto& config = configurations.emplace_back();
                if (registration.configuration.valid())
                {
                    config.valid = registration.configuration.encode_default(config.bytes).has_value();
                }
                for (std::size_t j = 0; preset && j < preset->data().systemCount(); ++j)
                {
                    const auto system = preset->data().systemAt(j);
                    if (system.type() == registration.type)
                    {
                        const auto bytes = system.configurationPayload();
                        config.bytes.assign(bytes.begin(), bytes.end());
                        config.valid = true;
                    }
                }
                config.editor = context.sceneConfigurationEditor(registration.configuration.type);
                if (config.editor)
                {
                    config.widget = config.editor->create(context, config.bytes);
                    if (!config.widget || !configuration_layout.addElement(*config.widget))
                    {
                        std::terminate();
                    }
                }
            }
        }

        ProjectSceneResult<SceneCreateSelection> selection() noexcept
        {
            SceneCreateSelection result;
            const auto& registrations = context.sceneRegistrations();
            for (std::size_t i = 0; i < schemas.size(); ++i)
            {
                if (schemas[i]->value())
                {
                    result.schemas.push_back(schema_ids[i]);
                }
            }
            simulation::SimulationDescriptionBuilder simulation;
            for (std::size_t i = 0; i < simulations.size(); ++i)
            {
                if (!simulations[i]->value())
                {
                    continue;
                }
                const auto& registration = registrations.simulation_systems->all()[i];
                std::vector<std::byte> configuration;
                if (registration.configuration.valid() && !registration.configuration.encode_default(configuration))
                {
                    return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::INVALID_DOCUMENT});
                }
                if (simulation_preset)
                {
                    for (std::size_t j = 0; j < simulation_preset->data().systemCount(); ++j)
                    {
                        const auto system = simulation_preset->data().systemAt(j);
                        if (system.type() == registration.type)
                        {
                            const auto bytes = system.configurationPayload();
                            configuration.assign(bytes.begin(), bytes.end());
                        }
                    }
                }
                if (!simulation.addSystem({i + 1}, registration.type.name, *registration.description, configuration))
                {
                    return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::INVALID_DOCUMENT});
                }
            }
            auto simulation_description = std::move(simulation).build();
            if (!simulation_description)
            {
                return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::INVALID_DOCUMENT});
            }
            result.simulation =
                std::make_shared<const simulation::SimulationDescription>(std::move(*simulation_description));
            scene::SceneDescriptionBuilder scene;
            for (std::size_t i = 0; i < systems.size(); ++i)
            {
                if (!systems[i]->value())
                {
                    continue;
                }
                const auto& registration = registrations.scene_systems[i];
                const auto& description = *registration.description;
                auto& config = configurations[i];
                if (!config.valid || (config.editor && !config.editor->serialize(*config.widget, config.bytes)))
                {
                    return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::INVALID_DOCUMENT});
                }
                if (!scene.addSystem(
                        {i + 1},
                        registration.type.name,
                        registration.type,
                        description.version,
                        description.configuration_schema_name,
                        description.configuration_schema_version,
                        config.bytes
                    ))
                {
                    return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::INVALID_DOCUMENT});
                }
            }
            auto scene_description = std::move(scene).buildResolved();
            if (!scene_description)
            {
                return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::INVALID_DOCUMENT});
            }
            result.systems = std::make_shared<const scene::SceneDescription>(std::move(*scene_description));
            return result;
        }
    };

    SceneCreationPane::SceneCreationPane(EditorContext& context, ProjectScenes& scenes, std::string title)
        : Pane(std::move(title)), impl_(std::make_unique<Impl>(context, scenes))
    {
        if (!addElement(impl_->layout))
        {
            std::terminate();
        }
        const auto retain = [&](auto result)
        {
            if (!result)
            {
                std::terminate();
            }
            impl_->connections.push_back(std::move(*result));
        };
        retain(connect(
            &impl_->profiles,
            &ui::Choice::edited,
            this,
            [this](ui::EditResult) noexcept
            {
                impl_->preset_changed = true;
                requestUpdate();
            }
        ));
        retain(connect(&impl_->create, &ui::Button::activated, this, [this]() noexcept { createScene(); }));
        retain(connect(&impl_->open, &ui::Button::activated, this, [this]() noexcept { openScene(); }));
        retain(connect(&scenes, &ProjectScenes::changed, this, [this]() noexcept { refreshCatalog(); }));
        retain(connect(&scenes, &ProjectScenes::opened, this, [this](asset::AssetId) noexcept { refreshCatalog(); }));
        refreshCatalog();
    }

    SceneCreationPane::~SceneCreationPane() noexcept = default;

    void SceneCreationPane::update() noexcept
    {
        if (std::exchange(impl_->preset_changed, false))
        {
            // Replace editor children at the Root update safe point, never during control drawing.
            impl_->selectPreset();
        }
    }

    void SceneCreationPane::refreshCatalog() noexcept
    {
        impl_->catalog_records = impl_->context.manifest().scenes;
        std::vector<ui::ChoiceOption> options;
        for (const auto& record : impl_->catalog_records)
        {
            options.push_back({static_cast<std::int64_t>(options.size()), record.name});
        }
        impl_->catalog.setOptions(std::move(options));
        impl_->status.setText(
            impl_->scenes.failure() ? describeProjectSceneFailure(*impl_->scenes.failure()) : "Ready"
        );
    }

    void SceneCreationPane::createScene() noexcept
    {
        auto selection = impl_->selection();
        if (!selection)
        {
            impl_->status.setText(describeProjectSceneFailure(selection.error()));
            return;
        }
        const auto profiles = impl_->context.sceneProfiles().profiles();
        const auto index = static_cast<std::size_t>(impl_->profiles.value());
        if (index >= profiles.size())
        {
            impl_->status.setText("No Scene creation profile is installed");
            return;
        }
        uuids::uuid_random_generator generate{impl_->random};
        auto result = impl_->scenes.create(
            {asset::AssetId{generate()}, impl_->name.value(), impl_->path.value(), profiles[index].id},
            std::move(*selection)
        );
        impl_->status.setText(result ? "Preparing Scene..." : describeProjectSceneFailure(result.error()));
    }

    void SceneCreationPane::openScene() noexcept
    {
        const auto index = static_cast<std::size_t>(impl_->catalog.value());
        if (index >= impl_->catalog_records.size())
        {
            return;
        }
        auto opened = impl_->scenes.open(
            impl_->context.project().root / std::filesystem::u8path(impl_->catalog_records[index].path)
        );
        impl_->status.setText(opened ? "Opening Scene..." : describeProjectSceneFailure(opened.error()));
    }
} // namespace lux::editor
