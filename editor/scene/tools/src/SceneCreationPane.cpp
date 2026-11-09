#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/ProjectScenes.hpp>
#include <lux/engine/editor/SceneConfigurationForm.hpp>
#include <lux/engine/editor/SceneCreationPane.hpp>
#include <lux/engine/object/ObjectRuntime.hpp>
#include <lux/engine/plugins/SceneRegistrations.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Layout.hpp>

#include <algorithm>
#include <map>
#include <random>
#include <set>

namespace lux::editor
{
    struct SceneCreationPane::Impl final
    {
        EditorContext& context;
        ProjectScenes& scenes;
        ui::Layout layout;
        ui::Label heading{"Create Scene: geometry, partition domains and independent systems"};
        ui::TextEdit name{"New Scene"};
        ui::TextEdit path{"Scenes/NewScene.luxscene"};
        ui::Choice catalog{{}};
        ui::Choice profiles{{}};
        ui::Layout configuration_layout;
        ui::Label eligibility;

        struct Configuration final
        {
            std::string schema;
            std::uint32_t version = 0;
            std::vector<std::byte> bytes;
            bool valid = true;
            bool initialized = false;
            std::unique_ptr<SceneConfigurationForm> widget;
        };

        struct GeometryChoice final
        {
            const scene::GeometryDeclaration& declaration;
            ui::CheckBox selected;
            ui::TextEdit domain;
            ui::Choice partition{{}};
            std::vector<const world::PartitionDeclaration*> partitions;
            world::WorldDomainSelection original;
            ui::Layout configuration_layout;
            Configuration configuration;
            std::vector<object::Connection> configuration_connections;
            bool configuration_dirty = true;

            explicit GeometryChoice(const scene::GeometryDeclaration& value)
                : declaration(value), selected("Geometry: " + value.name), domain(value.name)
            {
            }
        };

        std::vector<std::unique_ptr<GeometryChoice>> geometries;
        std::vector<world::WorldDomainSelection> preset_domains;
        std::vector<Configuration> configurations, simulation_configurations;
        bool configurations_dirty = false;
        std::vector<object::Connection> configuration_connections;
        std::unique_ptr<SceneCreationCandidate> candidate;
        std::uint64_t revision{};
        bool qualification_pending{}, qualification_dirty{true}, create_requested{};
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
            for (const auto& declaration : registrations.geometries)
            {
                const auto allowed = std::ranges::find_if(
                    registrations.allowed_content.geometries,
                    [&](const auto& value)
                    { return value.name == declaration.name && value.version == declaration.version; }
                );
                if (allowed == registrations.allowed_content.geometries.end())
                {
                    continue;
                }
                auto choice = std::make_unique<GeometryChoice>(declaration);
                std::vector<ui::ChoiceOption> options;
                for (const auto& partition : registrations.partitions)
                {
                    const auto permitted = std::ranges::find_if(
                        registrations.allowed_content.partitions,
                        [&](const auto& value)
                        {
                            return value.name == partition.identity.id.name &&
                                   value.version == partition.identity.version;
                        }
                    );
                    if (partition.input_type != declaration.partition_input_type ||
                        permitted == registrations.allowed_content.partitions.end())
                    {
                        continue;
                    }
                    options.push_back({static_cast<std::int64_t>(options.size()), partition.identity.id.name});
                    choice->partitions.push_back(&partition);
                }
                choice->partition.setOptions(std::move(options));
                choice->domain.setHint("Primary partition domain (shared names share the same domain)");
                add(choice->selected);
                add(choice->domain);
                add(choice->partition);
                add(choice->configuration_layout);
                geometries.push_back(std::move(choice));
            }
            for (const auto& schema : registrations.components.all())
            {
                const bool authored =
                    schema.semantic_kind != ecs::EComponentSemanticKind::RUNTIME_DERIVED &&
                    schema.snapshot == ecs::EComponentSnapshotPolicy::COPY;
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
            add(eligibility);
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
            preset_domains.clear();
            for (const auto& domain : defaults->world->data().domains())
            {
                preset_domains.push_back(
                    {domain.key, domain.partitioner, domain.configuration_version, domain.configuration}
                );
            }
            for (auto& choice : geometries)
            {
                const auto found = std::ranges::find_if(
                    preset->data().geometries(),
                    [&](const auto& value) { return value.geometry.name == choice->declaration.name; }
                );
                choice->selected.setValue(found != preset->data().geometries().end());
                choice->domain.setValue(
                    found != preset->data().geometries().end() ? found->domain : choice->declaration.name
                );
                const auto domain =
                    std::ranges::find(preset_domains, choice->domain.value(), &world::WorldDomainSelection::key);
                choice->original = domain != preset_domains.end() ? *domain : world::WorldDomainSelection{};
                choice->configuration_dirty = true;
                if (domain != preset_domains.end())
                {
                    for (std::size_t i = 0; i < choice->partitions.size(); ++i)
                    {
                        if (choice->partitions[i]->identity.id == domain->partitioner.id &&
                            choice->partitions[i]->identity.version == domain->partitioner.version)
                        {
                            choice->partition.setValue(i);
                            break;
                        }
                    }
                }
            }
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
            const bool supported = std::ranges::all_of(configurations, &Configuration::valid);
            status.setText(
                supported ? "Preset applied. Opaque configurations retain their bytes when no field editor is installed"
                          : "Preset has an unsupported configuration version; choose another profile"
            );
        }

        void prepareConfigurations() noexcept
        {
            configuration_connections.clear();
            configurations.clear();
            configurations.resize(systems.size());
            simulation_configurations.clear();
            simulation_configurations.resize(simulations.size());
            refreshConfigurations();
        }

        void prepareConfiguration(
            Configuration& config,
            bool selected,
            const serialization::PortableValueCodec& codec,
            const system::SystemTypeDescription& contract,
            std::span<const std::byte> original,
            std::string_view schema,
            std::uint32_t version,
            bool has_original,
            serialization::DecodePortableValueFn decoder
        ) noexcept
        {
            if (!selected)
            {
                if (config.widget)
                {
                    // Keep the user's draft across selection toggles; never substitute defaults on failure.
                    config.valid = config.widget->serialize(config.bytes);
                    config.widget.reset();
                }
                return;
            }
            if (!config.initialized)
            {
                config.initialized = true;
                config.schema = has_original ? schema : contract.configuration_schema_name;
                config.version = has_original ? version : contract.configuration_schema_version;
                config.bytes.assign(original.begin(), original.end());
                if (has_original)
                {
                    const bool same_schema = config.schema == contract.configuration_schema_name;
                    if (same_schema && codec.valid())
                    {
                        config.valid = codec.reencode(config.bytes, decoder, config.bytes).has_value();
                        if (config.valid)
                        {
                            config.version = contract.configuration_schema_version;
                        }
                    }
                    else
                    {
                        config.valid =
                            same_schema && config.schema.empty() && config.version == 0 && config.bytes.empty();
                    }
                }
                else if (codec.valid())
                {
                    config.valid = codec.encode_default(config.bytes).has_value();
                }
            }
            const auto* editor = config.valid ? context.sceneConfigurationEditor(codec.type) : nullptr;
            if (!config.widget && editor)
            {
                config.widget = editor->create(context, config.bytes);
                if (!config.widget || !configuration_layout.addElement(*config.widget))
                {
                    std::terminate();
                }
            }
        }

        void refreshConfigurations() noexcept
        {
            configuration_connections.clear();
            const auto& registrations = context.sceneRegistrations();
            for (std::size_t i = 0; i < systems.size(); ++i)
            {
                const auto& registration = registrations.scene_systems[i];
                scene::SceneSystemDescription original;
                for (std::size_t j = 0; preset && j < preset->data().systemCount(); ++j)
                {
                    if (preset->data().systemAt(j).type() == registration.type)
                    {
                        original = preset->data().systemAt(j);
                        break;
                    }
                }
                prepareConfiguration(
                    configurations[i],
                    systems[i]->value(),
                    registration.configuration,
                    *registration.description,
                    original ? original.configurationPayload() : std::span<const std::byte>{},
                    original ? original.configurationSchemaName() : std::string_view{},
                    original ? original.configurationSchemaVersion() : 0,
                    bool(original),
                    original ? registration.configurationDecoder(original.configurationSchemaVersion()) : nullptr
                );
            }
            for (std::size_t i = 0; i < simulations.size(); ++i)
            {
                const auto& registration = registrations.simulation_systems->all()[i];
                simulation::SimulationSystemView original;
                for (std::size_t j = 0; simulation_preset && j < simulation_preset->data().systemCount(); ++j)
                {
                    if (simulation_preset->data().systemAt(j).type() == registration.type)
                    {
                        original = simulation_preset->data().systemAt(j);
                        break;
                    }
                }
                const auto& contract = registration.description->type;
                const auto decoder =
                    original && original.configurationSchemaVersion() == contract.configuration_schema_version
                        ? registration.configuration.decode
                        : nullptr;
                prepareConfiguration(
                    simulation_configurations[i],
                    simulations[i]->value(),
                    registration.configuration,
                    contract,
                    original ? original.configurationPayload() : std::span<const std::byte>{},
                    original ? original.configurationSchemaName() : std::string_view{},
                    original ? original.configurationSchemaVersion() : 0,
                    bool(original),
                    decoder
                );
            }
            configurations_dirty = false;
        }

        void refreshCandidates() noexcept
        {
            // This is a geometry projection, not another full compatibility solver. The exact
            // candidate still resolves dynamic inputs, producers, ambiguity and phases in Engine.
            std::map<std::string_view, bool, std::less<>> geometry_schemas;
            for (const auto& geometry : geometries)
            {
                for (const auto& component : geometry->declaration.components)
                {
                    geometry_schemas[component.schema.name] |= geometry->selected.value();
                }
                for (const auto& data : geometry->declaration.data)
                {
                    if (data.kind == system::ESystemDataKind::SCHEMA)
                    {
                        geometry_schemas[data.name] |= geometry->selected.value();
                    }
                }
            }
            for (std::size_t i = 0; i < schemas.size(); ++i)
            {
                const auto found = geometry_schemas.find(schema_ids[i].name);
                // Projection controls new choices, never erases authored/preset schema selections.
                // Old descriptions may legitimately predate Geometry declarations.
                const bool offered = found == geometry_schemas.end() || found->second || schemas[i]->value();
                schemas[i]->setEnabled(offered);
            }
            std::set<std::string_view> authored;
            for (std::size_t i = 0; i < schemas.size(); ++i)
            {
                if (schemas[i]->value())
                {
                    authored.insert(schema_ids[i].name);
                }
            }
            std::string unavailable;
            const auto project = [&](auto& controls, auto description, std::string_view kind)
            {
                for (std::size_t i = 0; i < controls.size(); ++i)
                {
                    const auto& contract = description(i);
                    std::string_view missing;
                    for (const auto& input : contract.data_inputs)
                    {
                        const auto found = geometry_schemas.find(input.name);
                        if (input.kind == system::ESystemDataKind::SCHEMA && found != geometry_schemas.end() &&
                            !found->second && !authored.contains(input.name))
                        {
                            missing = input.name;
                            break;
                        }
                    }
                    controls[i]->setEnabled(missing.empty() || controls[i]->value());
                    if (!missing.empty())
                    {
                        unavailable += std::string(kind) + std::string(contract.canonical_name) +
                                       ": select geometry providing " + std::string(missing) + "\n";
                    }
                }
            };
            const auto& registrations = context.sceneRegistrations();
            project(
                systems,
                [&](std::size_t i) -> const auto& { return *registrations.scene_systems[i].description; },
                "Scene "
            );
            project(
                simulations,
                [&](std::size_t i) -> const auto&
                { return registrations.simulation_systems->all()[i].description->type; },
                "Simulation "
            );
            eligibility.setText(std::move(unavailable));
        }

        void prepareDomainConfiguration(GeometryChoice& choice) noexcept
        {
            choice.configuration_dirty = false;
            if (!choice.selected.value())
            {
                const auto index = static_cast<std::size_t>(choice.partition.value());
                if (choice.configuration.widget && index < choice.partitions.size())
                {
                    if (choice.configuration.widget->serialize(choice.configuration.bytes))
                    {
                        choice.original = {
                            choice.domain.value(),
                            choice.partitions[index]->identity,
                            choice.configuration.version,
                            choice.configuration.bytes
                        };
                    }
                    choice.configuration_connections.clear();
                    choice.configuration.widget.reset();
                }
                return;
            }
            choice.configuration_connections.clear();
            choice.configuration = {};
            const auto index = static_cast<std::size_t>(choice.partition.value());
            if (index >= choice.partitions.size())
            {
                return;
            }
            const auto& partition = *choice.partitions[index];
            auto& config = choice.configuration;
            const bool same_partition = partition.identity.id == choice.original.partitioner.id &&
                                        partition.identity.version == choice.original.partitioner.version;
            if (same_partition)
            {
                config.version = choice.original.configuration_version;
                config.bytes = choice.original.configuration;
            }
            else
            {
                config.version = partition.configuration_version;
                config.valid = !partition.configuration.valid() ||
                               partition.configuration.encode_default(config.bytes).has_value();
            }
            const bool editable = config.valid && config.version == partition.configuration_version;
            const auto* editor = editable ? context.sceneConfigurationEditor(partition.configuration.type) : nullptr;
            if (editor)
            {
                config.widget = editor->create(context, config.bytes);
                if (!config.widget || !choice.configuration_layout.addElement(*config.widget))
                {
                    std::terminate();
                }
            }
        }

        ProjectSceneResult<std::shared_ptr<const simulation::SimulationDescription>> selectSimulation() noexcept
        {
            const auto& registrations = context.sceneRegistrations();
            for (std::size_t i = 0; i < simulations.size(); ++i)
            {
                auto& config = simulation_configurations[i];
                if (simulations[i]->value() &&
                    (!config.valid || (config.widget && !config.widget->serialize(config.bytes))))
                {
                    return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::INVALID_DOCUMENT});
                }
            }
            simulation::SimulationDescriptionBuilder builder;
            std::uint64_t next_instance = 1;
            std::vector<bool> present(simulations.size());
            for (std::size_t i = 0; simulation_preset && i < simulation_preset->data().systemCount(); ++i)
            {
                const auto original = simulation_preset->data().systemAt(i);
                next_instance = std::max(next_instance, original.instanceId().value + 1);
                const auto found = std::ranges::find_if(
                    registrations.simulation_systems->all(),
                    [&](const auto& value) { return value.type == original.type(); }
                );
                if (found == registrations.simulation_systems->all().end())
                {
                    return cxx::unexpected(VProjectSceneFailure{scene::ScenePackageFailure{
                        scene::EScenePackageError::INVALID_ARGUMENT,
                        {},
                        i,
                        original.type().name,
                        "Missing Simulation provider; original configuration retained"
                    }});
                }
                const auto index = static_cast<std::size_t>(found - registrations.simulation_systems->all().begin());
                const bool first_instance = !present[index];
                present[index] = true;
                if (!simulations[index]->value())
                {
                    continue;
                }
                const auto& contract = found->description->type;
                const bool incompatible =
                    original.version() != contract.version ||
                    original.configurationSchemaName() != contract.configuration_schema_name ||
                    original.configurationSchemaVersion() != contract.configuration_schema_version;
                if (incompatible)
                {
                    return cxx::unexpected(VProjectSceneFailure{scene::ScenePackageFailure{
                        scene::EScenePackageError::INVALID_ARGUMENT,
                        {},
                        i,
                        original.type().name,
                        "Unsupported Simulation version; original configuration retained"
                    }});
                }
                if (!builder.addSystem(
                        original.instanceId(),
                        original.instanceName(),
                        *found->description,
                        first_instance ? std::span<const std::byte>{simulation_configurations[index].bytes}
                                       : original.configurationPayload()
                    ))
                {
                    return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::INVALID_DOCUMENT});
                }
            }
            for (std::size_t i = 0; i < simulations.size(); ++i)
            {
                if (!simulations[i]->value() || present[i])
                {
                    continue;
                }
                const auto& registration = registrations.simulation_systems->all()[i];
                const auto& configuration = simulation_configurations[i].bytes;
                if (!builder.addSystem(
                        {next_instance++},
                        registration.type.name,
                        *registration.description,
                        configuration
                    ))
                {
                    return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::INVALID_DOCUMENT});
                }
            }
            if (simulation_preset)
            {
                const auto& source = simulation_preset->data();
                for (std::size_t i = 0; i < source.dataCount(); ++i)
                {
                    const auto data = source.dataAt(i);
                    if (!builder.addData(data.schema(), data.version(), data.payload()))
                    {
                        return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::INVALID_DOCUMENT});
                    }
                }
                for (std::size_t i = 0; i < source.constructionDependencyCount(); ++i)
                {
                    const auto edge = source.constructionDependencyAt(i);
                    if (!builder.addConstructionDependency(edge.before().instanceId(), edge.after().instanceId()))
                    {
                        return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::INVALID_DOCUMENT});
                    }
                }
                for (const auto& edge : source.executionDependencies())
                {
                    if (!builder.addExecutionDependency(edge.before, edge.after))
                    {
                        return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::INVALID_DOCUMENT});
                    }
                }
                for (const auto& producer : source.channelProducers())
                {
                    if (!builder.addChannelProducer(producer))
                    {
                        return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::INVALID_DOCUMENT});
                    }
                }
            }
            auto description = std::move(builder).build();
            if (!description)
            {
                return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::INVALID_DOCUMENT});
            }
            return std::make_shared<const simulation::SimulationDescription>(std::move(*description));
        }

        ProjectSceneResult<void> selectDomains(
            SceneCreateSelection& result,
            scene::SceneDescriptionBuilder& scene
        ) noexcept
        {
            result.domains = preset_domains;
            std::vector<std::string_view> assigned_domains;
            for (const auto& choice : geometries)
            {
                if (!choice->selected.value())
                {
                    continue;
                }
                const auto index = static_cast<std::size_t>(choice->partition.value());
                if (index >= choice->partitions.size())
                {
                    return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::INVALID_DOCUMENT});
                }
                const auto& partition = *choice->partitions[index];
                auto& configuration = choice->configuration;
                if (configuration.widget && !configuration.widget->serialize(configuration.bytes))
                {
                    return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::INVALID_DOCUMENT});
                }
                world::WorldDomainSelection
                    domain{choice->domain.value(), partition.identity, configuration.version, configuration.bytes};
                const auto existing = std::ranges::find(result.domains, domain.key, &world::WorldDomainSelection::key);
                if (existing == result.domains.end())
                {
                    result.domains.push_back(std::move(domain));
                }
                else if (std::ranges::find(assigned_domains, domain.key) == assigned_domains.end())
                {
                    *existing = std::move(domain);
                }
                else if (existing->partitioner.id != domain.partitioner.id ||
                         existing->partitioner.version != domain.partitioner.version ||
                         existing->configuration_version != domain.configuration_version ||
                         existing->configuration != domain.configuration)
                {
                    return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::INVALID_DOCUMENT});
                }
                assigned_domains.push_back(choice->domain.value());
                if (!scene.selectGeometry(
                        {{choice->declaration.name, choice->declaration.version}, choice->domain.value()}
                    ))
                {
                    return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::INVALID_DOCUMENT});
                }
            }
            // Unknown authored geometry must not silently disappear when a plugin is unavailable.
            if (preset)
            {
                for (const auto& geometry : preset->data().geometries())
                {
                    const bool known = std::ranges::any_of(
                        context.sceneRegistrations().geometries,
                        [&](const auto& value) { return value.name == geometry.geometry.name; }
                    );
                    if (!known && !scene.selectGeometry(geometry))
                    {
                        return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::INVALID_DOCUMENT});
                    }
                }
            }
            return {};
        }

        ProjectSceneResult<void> selectSystems(scene::SceneDescriptionBuilder& scene) noexcept
        {
            const auto& registrations = context.sceneRegistrations();
            std::uint64_t next_instance = 1;
            for (std::size_t i = 0; preset && i < preset->data().systemCount(); ++i)
            {
                next_instance = std::max(next_instance, preset->data().systemAt(i).instanceId().value + 1);
            }
            const auto append = [&](scene::SceneSystemDescription original,
                                    const scene::SceneSystemRegistration* registration,
                                    Configuration* config) noexcept
            {
                const auto id = original ? original.instanceId() : system::SystemInstanceId{next_instance++};
                const auto name = original ? original.instanceName() : std::string_view{registration->type.name};
                const auto& type = original ? original.type() : registration->type;
                const auto version = original ? original.version() : registration->description->version;
                const auto schema = config ? std::string_view{config->schema} : original.configurationSchemaName();
                const auto schema_version = config ? config->version : original.configurationSchemaVersion();
                const auto bytes = config ? std::span<const std::byte>{config->bytes} : original.configurationPayload();
                if (!scene.addSystem(id, name, type, version, schema, schema_version, bytes))
                {
                    return false;
                }
                for (std::size_t i = 0; original && i < original.requirementBindingCount(); ++i)
                {
                    const auto binding = original.requirementBindingAt(i);
                    if (!scene.bindRequirement(id, binding.requirement(), binding.provider()))
                    {
                        return false;
                    }
                }
                return true;
            };
            for (std::size_t i = 0; i < systems.size(); ++i)
            {
                if (!systems[i]->value())
                {
                    continue;
                }
                const auto& registration = registrations.scene_systems[i];
                auto& config = configurations[i];
                if (config.widget && !config.widget->serialize(config.bytes))
                {
                    return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::INVALID_DOCUMENT});
                }
                scene::SceneSystemDescription original;
                for (std::size_t j = 0; preset && j < preset->data().systemCount(); ++j)
                {
                    if (preset->data().systemAt(j).type() == registration.type)
                    {
                        original = preset->data().systemAt(j);
                        break;
                    }
                }
                if (!append(original, &registration, &config))
                {
                    return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::INVALID_DOCUMENT});
                }
            }
            for (std::size_t i = 0; preset && i < preset->data().systemCount(); ++i)
            {
                const auto original = preset->data().systemAt(i);
                const auto known = std::ranges::find(
                    registrations.scene_systems,
                    original.type(),
                    &scene::SceneSystemRegistration::type
                );
                bool keep = known == registrations.scene_systems.end();
                if (!keep && systems[known - registrations.scene_systems.begin()]->value())
                {
                    for (std::size_t j = 0; j < i; ++j)
                    {
                        keep |= preset->data().systemAt(j).type() == original.type();
                    }
                }
                if (keep && !append(original, nullptr, nullptr))
                {
                    return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::INVALID_DOCUMENT});
                }
            }
            for (std::size_t i = 0; preset && i < preset->data().dependencyCount(); ++i)
            {
                const auto dependency = preset->data().dependencyAt(i);
                if (!scene.addDependency(dependency.before(), dependency.after()))
                {
                    return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::INVALID_DOCUMENT});
                }
            }
            return {};
        }

        ProjectSceneResult<SceneCreateSelection> selection() noexcept
        {
            if (!preset || !simulation_preset)
            {
                return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::INVALID_DOCUMENT});
            }
            SceneCreateSelection result;
            const auto& registrations = context.sceneRegistrations();
            for (std::size_t i = 0; i < schemas.size(); ++i)
            {
                if (schemas[i]->value())
                {
                    result.schemas.push_back(schema_ids[i]);
                }
            }
            auto simulation = selectSimulation();
            if (!simulation)
            {
                return cxx::unexpected(simulation.error());
            }
            result.simulation = std::move(*simulation);
            scene::SceneDescriptionBuilder scene;
            if (auto domains = selectDomains(result, scene); !domains)
            {
                return cxx::unexpected(domains.error());
            }
            if (auto systems = selectSystems(scene); !systems)
            {
                return cxx::unexpected(systems.error());
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
                invalidateSelection();
            }
        ));
        retain(connect(&impl_->create, &ui::Button::activated, this, [this]() noexcept { createScene(); }));
        retain(connect(&impl_->open, &ui::Button::activated, this, [this]() noexcept { openScene(); }));
        retain(connect(&scenes, &ProjectScenes::changed, this, [this]() noexcept { refreshCatalog(); }));
        retain(connect(&scenes, &ProjectScenes::opened, this, [this](asset::AssetId) noexcept { refreshCatalog(); }));
        for (const auto& control : impl_->schemas)
        {
            retain(connect(
                control.get(),
                &ui::CheckBox::edited,
                this,
                [this](ui::EditResult) noexcept { invalidateSelection(); }
            ));
        }
        for (const auto& control : impl_->simulations)
        {
            retain(connect(
                control.get(),
                &ui::CheckBox::edited,
                this,
                [this](ui::EditResult) noexcept { invalidateSelection(); }
            ));
        }
        for (const auto& control : impl_->systems)
        {
            retain(connect(
                control.get(),
                &ui::CheckBox::edited,
                this,
                [this](ui::EditResult) noexcept { invalidateSelection(); }
            ));
        }
        retain(connect(
            &impl_->name,
            &ui::TextEdit::edited,
            this,
            [this](ui::EditResult) noexcept { invalidateSelection(); }
        ));
        retain(connect(
            &impl_->path,
            &ui::TextEdit::edited,
            this,
            [this](ui::EditResult) noexcept { invalidateSelection(); }
        ));
        for (auto& geometry : impl_->geometries)
        {
            retain(connect(
                &geometry->selected,
                &ui::CheckBox::edited,
                this,
                [this, selected = geometry.get()](ui::EditResult) noexcept
                {
                    selected->configuration_dirty = true;
                    if (selected->selected.value())
                    {
                        for (const auto& component : selected->declaration.components)
                        {
                            const auto schema =
                                std::ranges::find(impl_->schema_ids, world::worldDataSchemaId(component.schema.name));
                            if (schema != impl_->schema_ids.end())
                            {
                                impl_->schemas[schema - impl_->schema_ids.begin()]->setValue(true);
                            }
                        }
                    }
                    invalidateSelection();
                }
            ));
            retain(connect(
                &geometry->domain,
                &ui::TextEdit::edited,
                this,
                [this](ui::EditResult) noexcept { invalidateSelection(); }
            ));
            retain(connect(
                &geometry->partition,
                &ui::Choice::edited,
                this,
                [this, choice = geometry.get()](ui::EditResult) noexcept
                {
                    choice->configuration_dirty = true;
                    invalidateSelection();
                }
            ));
        }
        connectConfigurations();
        refreshCatalog();
        invalidateSelection();
    }

    SceneCreationPane::~SceneCreationPane() noexcept = default;

    void SceneCreationPane::update() noexcept
    {
        if (std::exchange(impl_->preset_changed, false))
        {
            // Replace editor children at the Root update safe point, never during control drawing.
            impl_->selectPreset();
            connectConfigurations();
        }
        impl_->refreshCandidates();
        if (impl_->configurations_dirty)
        {
            impl_->refreshConfigurations();
            connectConfigurations();
        }
        for (auto& choice : impl_->geometries)
        {
            if (!choice->configuration_dirty)
            {
                continue;
            }
            impl_->prepareDomainConfiguration(*choice);
            if (choice->configuration.widget)
            {
                auto connected = connect(
                    choice->configuration.widget.get(),
                    &SceneConfigurationForm::edited,
                    this,
                    [this]() noexcept { invalidateSelection(); }
                );
                if (!connected)
                {
                    std::terminate();
                }
                choice->configuration_connections.push_back(std::move(*connected));
            }
        }
        if (impl_->qualification_dirty && !impl_->qualification_pending)
        {
            qualifySelection();
        }
    }

    void SceneCreationPane::refreshCatalog() noexcept
    {
        if (impl_->catalog_records != impl_->context.manifest().scenes)
        {
            invalidateSelection();
        }
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

    void SceneCreationPane::connectConfigurations() noexcept
    {
        impl_->configuration_connections.clear();
        for (auto* configurations : {&impl_->configurations, &impl_->simulation_configurations})
        {
            for (auto& config : *configurations)
            {
                if (!config.widget)
                {
                    continue;
                }
                auto connection = connect(
                    config.widget.get(),
                    &SceneConfigurationForm::edited,
                    this,
                    [this]() noexcept { invalidateSelection(); }
                );
                if (!connection)
                {
                    std::terminate();
                }
                impl_->configuration_connections.push_back(std::move(*connection));
            }
        }
    }

    void SceneCreationPane::invalidateSelection() noexcept
    {
        ++impl_->revision;
        impl_->configurations_dirty = true;
        impl_->candidate.reset();
        impl_->qualification_dirty = true;
        impl_->create_requested = false;
        impl_->create.setEnabled(false);
        requestUpdate();
    }

    void SceneCreationPane::qualifySelection() noexcept
    {
        impl_->qualification_dirty = false;
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
        const auto candidate_id = asset::AssetId{generate()};
        impl_->qualification_pending = true;
        const auto revision = impl_->revision;
        const auto target = objectId();
        auto result = impl_->scenes.prepare(
            {candidate_id, impl_->name.value(), impl_->path.value(), profiles[index].id},
            std::move(*selection),
            [target, revision](SceneCreationResult result) noexcept
            {
                auto resolved = object::ObjectRuntime::instance().resolve(target);
                if (!resolved)
                {
                    return;
                }
                auto& pane = *static_cast<SceneCreationPane*>(*resolved);
                auto& state = *pane.impl_;
                state.qualification_pending = false;
                if (revision != state.revision)
                {
                    pane.requestUpdate();
                    return;
                }
                if (result)
                {
                    state.candidate = std::move(*result);
                    state.status.setText("Selection qualified; Create uses this exact candidate");
                    state.create.setEnabled(true);
                    if (state.create_requested)
                    {
                        pane.createScene();
                    }
                }
                else
                {
                    state.status.setText(describeProjectSceneFailure(result.error()));
                }
            }
        );
        if (!result)
        {
            impl_->qualification_pending = false;
            impl_->status.setText(describeProjectSceneFailure(result.error()));
        }
        else
        {
            impl_->status.setText("Checking selected declarations and configurations...");
        }
    }

    void SceneCreationPane::createScene() noexcept
    {
        if (!impl_->candidate)
        {
            impl_->create_requested = true;
            if (!impl_->qualification_pending)
            {
                impl_->qualification_dirty = true;
                requestUpdate();
            }
            return;
        }
        impl_->create_requested = false;
        auto result = impl_->scenes.create(std::move(impl_->candidate));
        impl_->create.setEnabled(false);
        impl_->status.setText(result ? "Publishing qualified Scene..." : describeProjectSceneFailure(result.error()));
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
