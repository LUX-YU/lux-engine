#include <lux/engine/editor/scene/SceneConfigurationPreparation.hpp>
#include <lux/engine/editor/scene/AuthoringFacts.hpp>
#include <lux/engine/scene/SceneSystemRegistration.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>
#include <lux/engine/scene/RenderFeatureSceneBinding.hpp>
#include <lux/engine/scene/WorldLoadingSystem.hpp>
#include <lux/engine/scene/TransformSystem.hpp>
#include <lux/engine/simulation/SimulationSystemRegistry.hpp>
#include <lux/engine/function/render/client/core/RenderFeatureRegistration.hpp>
#include <algorithm>
#include <cmath>

namespace lux::editor::scene
{
    namespace
    {
        ScenePreparationFailure invalid(std::string domain, std::string message = {})
        {
            return {EScenePreparationError::INVALID_ARGUMENT, std::move(domain), 0, std::move(message)};
        }
        template<class Failure> auto rejected(std::string domain, Failure error)
        {
            auto failure = invalid(std::move(domain));
            failure.cause = std::move(error);
            return cxx::unexpected(std::move(failure));
        }
        bool matches(const SceneSystemConfigurationDraft& value, const system::SystemTypeDescription& type)
        {
            return value.type.name == type.canonical_name && value.version == type.version &&
                value.configuration_schema == type.configuration_schema_name &&
                value.configuration_version == type.configuration_schema_version;
        }
        bool preserved(const std::optional<SceneConfigurationDraft>& original, const SceneSystemConfigurationDraft& value)
        {
            if (!original)
                return false;
            return std::ranges::find(original->systems, value) != original->systems.end();
        }
        ScenePreparationResult<void> checkRender(
            const SceneSystemConfigurationDraft& row, const serialization::PortableValueCodec& codec,
            const AuthoringFacts& facts, const SceneConfigurationRegistrations& inputs,
            const std::optional<SceneConfigurationDraft>& original
        )
        {
            lux::scene::RenderSystemConfiguration value;
            auto decoded = codec.decode(row.configuration, &value);
            if (!decoded)
                return rejected("scene.configuration.decode", decoded.error());
            const bool is_invalid_size = !std::isfinite(value.coordinate_page_size) || value.coordinate_page_size <= 0;
            if (is_invalid_size)
                return cxx::unexpected(invalid("scene.new.coordinate_page_size"));
            lux::scene::RenderSystemConfiguration baseline;
            if (original)
            {
                const auto source = std::ranges::find(original->systems, row.id, &SceneSystemConfigurationDraft::id);
                const bool is_same_contract = source != original->systems.end() && source->type == row.type &&
                    source->version == row.version && source->configuration_schema == row.configuration_schema &&
                    source->configuration_version == row.configuration_version;
                if (is_same_contract)
                {
                    auto decoded_base = codec.decode(source->configuration, &baseline);
                    if (!decoded_base)
                        return rejected("scene.configuration.baseline", decoded_base.error());
                }
            }
            const auto find = [&](auto type) {
                return std::ranges::find_if(inputs.features, [type](const auto& feature) {
                    return feature.factory.descriptor.type == type;
                });
            };
            for (const auto& selected : value.features)
            {
                const auto found = find(selected.type);
                const bool is_unique = std::ranges::count(value.features, selected.type,
                    &lux::scene::RenderFeatureInstanceDescription::type) == 1;
                if (!is_unique)
                    return cxx::unexpected(invalid("scene.configuration.feature.duplicate"));
                const bool is_known = found != inputs.features.end() &&
                    found->configuration.schema == selected.configuration_schema &&
                    found->configuration.schema_version == selected.configuration_version;
                if (!is_known)
                {
                    const bool is_preserved = std::ranges::any_of(baseline.features, [&](const auto& feature) {
                        return feature.type == selected.type && feature.configuration == selected.configuration &&
                            feature.configuration_schema == selected.configuration_schema &&
                            feature.configuration_version == selected.configuration_version;
                    });
                    if (!is_preserved)
                        return cxx::unexpected(ScenePreparationFailure{
                            EScenePreparationError::MISSING_PROVIDER, "scene.configuration.feature", selected.type
                        });
                    continue;
                }
                const auto& descriptor = found->factory.descriptor;
                const auto binding = std::ranges::find(
                    inputs.feature_bindings, selected.type, &lux::scene::RenderFeatureSceneBinding::feature
                );
                const auto required = binding == inputs.feature_bindings.end() ? std::span<const std::string_view>{}
                    : binding->author_inputs;
                const auto allowed = queryApplicability(facts, {required});
                if (!allowed.supported())
                    return cxx::unexpected(ScenePreparationFailure{
                        EScenePreparationError::NOT_APPLICABLE, "scene.feature.author-input",
                        static_cast<std::uint64_t>(allowed.reason), allowed.subject
                    });
                for (const auto& dependency : descriptor.dependencies)
                {
                    const auto provider = std::ranges::find_if(value.features, [&](const auto& feature) {
                        return feature.type == dependency.type;
                    });
                    const bool is_missing = provider == value.features.end();
                    const auto registration = find(dependency.type);
                    const bool is_unknown = registration == inputs.features.end();
                    const bool is_wrong_version = !is_unknown &&
                        registration->factory.descriptor.abi_version != dependency.abi_version;
                    const bool is_invalid_dependency = (is_missing && !dependency.optional) ||
                        (!is_missing && (is_unknown || is_wrong_version));
                    if (is_invalid_dependency)
                        return cxx::unexpected(ScenePreparationFailure{
                            EScenePreparationError::INVALID_ARGUMENT, "scene.new.feature.dependency",
                            dependency.type, std::string(descriptor.canonical_name)
                        });
                }
                for (const auto conflict : descriptor.conflicts)
                    if (std::ranges::any_of(value.features, [conflict](const auto& feature) {
                        return feature.type == conflict;
                    }))
                        return cxx::unexpected(ScenePreparationFailure{
                            EScenePreparationError::INVALID_ARGUMENT, "scene.new.feature.conflict",
                            conflict, std::string(descriptor.canonical_name)
                        });
            }
            return {};
        }
        // The serialized contract remains usable without the implementation plugin. All spans
        // below borrow the fixed baseline for this call; the builder copies them before returning.
        auto copyUnknownSystem(simulation::SimulationDescriptionBuilder& builder,
            const SceneSystemConfigurationDraft& row, simulation::SimulationSystemView original)
        {
            std::vector<std::string_view> capabilities;
            for (std::size_t index{}; index < original.capabilityCount(); ++index)
                capabilities.push_back(original.capabilityAt(index));
            std::vector<simulation::SimulationTaskSpec> tasks;
            for (const auto& task : original.tasks())
                tasks.push_back({task.id, task.name});
            std::vector<std::vector<semantic::Type>> parameters(original.hookPointCount());
            std::vector<simulation::HookPointSpec> hooks;
            for (std::size_t index{}; index < original.hookPointCount(); ++index)
            {
                const auto hook = original.hookPointAt(index);
                for (std::size_t parameter{}; parameter < hook.parameterCount(); ++parameter)
                    parameters[index].push_back(hook.parameterAt(parameter));
                hooks.push_back({hook.id(), hook.name(), {parameters[index], {}}, hook.scriptCapable(),
                    hook.stableResume(), hook.contractVersion()});
            }
            std::vector<simulation::EventPointSpec> events;
            for (std::size_t index{}; index < original.eventCount(); ++index)
            {
                const auto event = original.eventAt(index);
                events.push_back({event.id(), event.name(), event.dispatchHook().id(), event.route(),
                    event.payloadType(), event.payloadSchemaName(), event.payloadSchemaVersion(),
                    event.ownerReproduction()});
            }
            constexpr std::string_view retained_partition[]{"*"};
            const simulation::SimulationSystemDescription contract{
                {row.type.name, row.version, row.configuration_schema, row.configuration_version,
                    capabilities, original.multiplicity(), retained_partition}, hooks, events, tasks
            };
            return builder.addSystem(row.id, row.name, contract, row.configuration);
        }
    }

    ScenePreparationResult<SceneCreationConfiguration> prepareSceneConfiguration(
        const SceneConfigurationDraft& draft, const SceneConfigurationRegistrations& inputs
    )
    {
        const bool is_invalid_partition = draft.partition.empty() || draft.partition_version == 0;
        const bool is_invalid_base = draft.base &&
            (!draft.base->world || !draft.base->scene || !draft.base->simulation);
        const bool is_invalid_input = is_invalid_partition || is_invalid_base;
        if (is_invalid_input)
            return cxx::unexpected(invalid("scene.configuration.input"));
        if (draft.base)
        {
            const auto& world = draft.base->world->data();
            const bool is_partition_change = draft.partition != world.partitioner().id.name ||
                draft.partition_version != world.partitioner().version;
            const bool is_schema_change = !std::ranges::equal(draft.schemas, world.schemas());
            const bool is_unsupported_structure = is_partition_change || is_schema_change;
            if (is_unsupported_structure)
                return cxx::unexpected(invalid("scene.configuration.structure"));
        }
        const AuthoringFacts facts{
            draft.based_on, draft.schemas, inputs.components, draft.partition, draft.partition_version
        };
        simulation::SimulationDescriptionBuilder simulation;
        lux::scene::SceneDescriptionBuilder scene;
        std::optional<SceneConfigurationDraft> original;
        if (draft.base)
        {
            auto captured = captureSceneConfiguration(*draft.base);
            if (!captured)
                return cxx::unexpected(invalid("scene.configuration.input"));
            original = std::move(*captured);
            scene.setWorld(draft.base->world->id());
            scene.setSimulation(draft.base->simulation->id());
            const auto& source = draft.base->simulation->data();
            for (std::size_t index{}; index < source.dataCount(); ++index)
            {
                const auto data = source.dataAt(index);
                auto added = simulation.addData(data.schema(), data.version(), data.payload());
                if (!added)
                    return rejected("scene.configuration.data", added.error());
            }
        }
        for (const auto& row : draft.systems)
        {
            if (row.domain == EConfigurationSystemDomain::SIMULATION)
            {
                const auto* found = inputs.simulation_systems.find(row.type);
                const bool is_known = found && matches(row, found->description->type);
                if (!is_known)
                {
                    if (!preserved(original, row))
                        return cxx::unexpected(ScenePreparationFailure{
                            EScenePreparationError::MISSING_PROVIDER, "scene.configuration.simulation", 0, row.type.name
                        });
                    auto added = copyUnknownSystem(simulation, row, draft.base->simulation->data().findSystem(row.id));
                    if (!added)
                        return rejected("scene.new.simulation", added.error());
                    continue;
                }
                if (!system::supportsWorldType(found->description->type, draft.partition))
                    return cxx::unexpected(invalid("scene.configuration.partition", row.type.name));
                auto added = simulation.addSystem(row.id, row.name, *found->description, row.configuration);
                if (!added)
                    return rejected("scene.new.simulation", added.error());
            }
            else
            {
                const auto found = std::ranges::find(inputs.scene_systems, row.type,
                    &lux::scene::SceneSystemRegistration::type);
                const bool is_known = found != inputs.scene_systems.end() && matches(row, *found->description);
                if (!is_known && !preserved(original, row))
                    return cxx::unexpected(ScenePreparationFailure{
                        EScenePreparationError::MISSING_PROVIDER, "scene.configuration.system", 0, row.type.name
                    });
                if (is_known)
                {
                    if (!system::supportsWorldType(*found->description, draft.partition))
                        return cxx::unexpected(invalid("scene.configuration.partition", row.type.name));
                    if (found->configuration.type == cxx::typeToken<lux::scene::RenderSystemConfiguration>())
                    {
                        auto valid = checkRender(row, found->configuration, facts, inputs, original);
                        if (!valid)
                            return cxx::unexpected(std::move(valid.error()));
                    }
                    for (const auto& requirement : found->requirements)
                    {
                        const auto bound = std::ranges::find(row.providers, requirement.name,
                            &SceneProviderBinding::requirement);
                        const bool is_unbound = bound == row.providers.end();
                        if (is_unbound && requirement.optional)
                            continue;
                        const bool is_available = !is_unbound && std::ranges::any_of(inputs.providers,
                            [&](const auto& provider) {
                                return provider.capability == requirement.capability && provider.name == bound->provider;
                            });
                        if (!is_available)
                            return cxx::unexpected(ScenePreparationFailure{
                                EScenePreparationError::MISSING_PROVIDER, "scene.new.provider", 0,
                                std::string(requirement.name)
                            });
                    }
                }
                auto added = scene.addSystem(row.id, row.name, row.type, row.version, row.configuration_schema,
                    row.configuration_version, row.configuration);
                if (!added)
                    return rejected("scene.new.system", added.error());
                for (const auto& binding : row.providers)
                {
                    auto bound = scene.bindRequirement(row.id, binding.requirement, binding.provider);
                    if (!bound)
                        return rejected("scene.new.provider", bound.error());
                }
            }
        }
        for (const auto& [before, after] : draft.construction)
            if (auto added = simulation.addConstructionDependency(before, after); !added)
                return rejected("scene.new.construction", added.error());
        for (const auto& [before, after] : draft.scene_dependencies)
            if (auto added = scene.addDependency(before, after); !added)
                return rejected("scene.new.scene-dependency", added.error());
        for (const auto& edge : draft.execution)
            if (auto added = simulation.addExecutionDependency(edge.before, edge.after); !added)
                return rejected("scene.new.execution", added.error());
        for (const auto& producer : draft.producers)
            if (auto added = simulation.addChannelProducer(producer); !added)
                return rejected("scene.new.channel", added.error());
        auto sim = std::move(simulation).build();
        if (!sim)
            return rejected("scene.new.simulation", sim.error());
        auto desc = std::move(scene).buildResolved();
        if (!desc)
            return rejected("scene.new.scene", desc.error());
        if (draft.viewport.valid() && !desc->findSystem(draft.viewport))
            return cxx::unexpected(invalid("scene.configuration.viewport"));
        return SceneCreationConfiguration{draft.name, draft.schemas,
            std::make_shared<const simulation::SimulationDescription>(std::move(*sim)), std::move(*desc), draft.viewport};
    }

    ScenePreparationResult<SceneConfiguration> prepareSceneConfigurationEdit(
        const SceneConfigurationDraft& draft, const SceneConfigurationRegistrations& inputs
    )
    {
        if (!draft.base)
            return cxx::unexpected(invalid("scene.configuration.input"));
        auto built = prepareSceneConfiguration(draft, inputs);
        if (!built)
            return cxx::unexpected(std::move(built.error()));
        const auto& base = *draft.base;
        const auto copied = [](auto values) {
            return std::vector<asset::AssetAuxiliaryPayload>(values.begin(), values.end());
        };
        auto simulation = simulation::SimulationAsset::create(base.simulation->info(),
            std::move(built->simulation), copied(base.simulation->auxiliaryPayloads()));
        if (!simulation)
            return rejected("scene.configuration.simulation.asset", simulation.error());
        auto scene = lux::scene::SceneAsset::create(base.scene->info(),
            std::make_shared<const lux::scene::SceneDescription>(std::move(built->scene)),
            copied(base.scene->auxiliaryPayloads()));
        if (!scene)
            return rejected("scene.configuration.scene.asset", scene.error());
        return SceneConfiguration{std::move(*scene), base.world, std::move(*simulation)};
    }

    ScenePreparationResult<SceneConfigurationDraft> makeSceneConfigurationPreset(
        ESceneContentPreset preset, std::string partition, std::uint32_t partition_version,
        const SceneConfigurationRegistrations& inputs
    )
    {
        SceneConfigurationDraft draft;
        draft.name = "Untitled scene";
        draft.partition = std::move(partition);
        draft.partition_version = partition_version;
        const bool is_supported_partition = draft.partition == "lux.spatial.builtin.single" && partition_version == 1;
        if (!is_supported_partition)
            return cxx::unexpected(invalid("scene.preset.partition"));
        if (preset == ESceneContentPreset::EMPTY)
            return draft;
        for (const auto& schema : inputs.components.all())
        {
            const auto& name = schema.id.name;
            const bool is_common = name == "lux.ecs.Parent" || name == "lux.scene.Camera";
            const bool is_spatial = preset == ESceneContentPreset::TWO_DIMENSIONAL ? name == "lux.ecs.Transform2D"
                : name == "lux.ecs.Transform3D" || name == "lux.ecs.Mesh3D" || name == "lux.ecs.Light3D";
            const bool is_selected = isAuthorComponent(schema) && (is_common || is_spatial);
            if (is_selected)
                draft.schemas.push_back(world::worldDataSchemaId(name));
        }
        std::uint64_t next_system{1};
        for (const auto name : {"lux.scene.transform", "lux.scene.world_loading", "lux.builtin.system.render"})
        {
            const auto found = std::ranges::find_if(inputs.scene_systems, [name](const auto& registration) {
                return registration.type.name == name;
            });
            if (found == inputs.scene_systems.end())
                return cxx::unexpected(ScenePreparationFailure{
                    EScenePreparationError::MISSING_PROVIDER, "scene.preset.system", 0, name
                });
            const auto& type = *found->description;
            SceneSystemConfigurationDraft row{
                EConfigurationSystemDomain::SCENE, {next_system}, "system-" + std::to_string(next_system),
                found->type, type.version, std::string(type.configuration_schema_name),
                type.configuration_schema_version, {}, {}
            };
            ++next_system;
            for (const auto& requirement : found->requirements)
            {
                const SceneProviderOption* selected{};
                std::size_t count{};
                for (const auto& provider : inputs.providers)
                    if (provider.capability == requirement.capability)
                    {
                        ++count;
                        selected = &provider;
                    }
                if (count == 1)
                    row.providers.push_back({std::string(requirement.name), std::string(selected->name)});
            }
            serialization::SerializationResult encoded;
            const auto& codec = found->configuration;
            if (codec.type == cxx::typeToken<lux::scene::TransformSystemConfiguration>())
            {
                const lux::scene::TransformSystemConfiguration configuration{4096, 8192, 1048576};
                encoded = codec.encode(&configuration, row.configuration);
            }
            else if (codec.type == cxx::typeToken<lux::scene::WorldLoadingConfiguration>())
            {
                lux::scene::WorldLoadingConfiguration configuration;
                configuration.bootstrap.push_back({0});
                encoded = codec.encode(&configuration, row.configuration);
            }
            else if (codec.type == cxx::typeToken<lux::scene::RenderSystemConfiguration>())
            {
                lux::scene::RenderSystemConfiguration configuration;
                configuration.coordinate_page_size = 1024;
                std::vector<std::string_view> wanted{"lux.render.view_camera.v1", "lux.render.material.v1"};
                if (preset == ESceneContentPreset::TWO_DIMENSIONAL)
                    wanted.push_back("lux.render.canvas2d.v2");
                else
                    for (const auto feature : {"lux.render.mesh_stack.v1", "lux.render.light.v1",
                         "lux.render.forward_mesh.v1", "lux.render.shadow_map.v1"})
                        wanted.push_back(feature);
                for (std::size_t index{}; index < wanted.size(); ++index)
                {
                    const auto feature = std::ranges::find_if(inputs.features, [&](const auto& candidate) {
                        return candidate.factory.descriptor.canonical_name == wanted[index];
                    });
                    if (feature == inputs.features.end())
                        return cxx::unexpected(ScenePreparationFailure{
                            EScenePreparationError::MISSING_PROVIDER, "scene.preset.feature", 0, std::string(wanted[index])
                        });
                    for (const auto& dependency : feature->factory.descriptor.dependencies)
                    {
                        if (dependency.optional)
                            continue;
                        const auto provider = std::ranges::find_if(inputs.features, [&](const auto& candidate) {
                            return candidate.factory.descriptor.type == dependency.type;
                        });
                        if (provider == inputs.features.end())
                            return cxx::unexpected(ScenePreparationFailure{
                                EScenePreparationError::MISSING_PROVIDER, "scene.preset.feature.dependency",
                                dependency.type
                            });
                        const auto canonical = provider->factory.descriptor.canonical_name;
                        if (std::ranges::find(wanted, canonical) == wanted.end())
                            wanted.push_back(canonical);
                    }
                    std::vector<std::byte> bytes;
                    auto result = feature->configuration.portable.encode_default(bytes);
                    if (!result)
                        return rejected("scene.preset.feature", result.error());
                    configuration.features.push_back({feature->factory.descriptor.type, std::move(bytes),
                        std::string(feature->configuration.schema), feature->configuration.schema_version});
                }
                encoded = codec.encode(&configuration, row.configuration);
                draft.viewport = row.id;
            }
            else if (codec.valid())
                encoded = codec.encode_default(row.configuration);
            if (!encoded)
                return rejected("scene.preset.configuration", encoded.error());
            draft.systems.push_back(std::move(row));
        }
        return draft;
    }
}
