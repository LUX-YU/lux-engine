#include <lux/engine/scene/detail/SceneInstance.hpp>

#include <lux/engine/scene/SceneSystemInstaller.hpp>
#include <lux/engine/scene/detail/SceneInstanceImpl.hpp>
#include <lux/engine/system/detail/SystemDependencyOrder.hpp>

#include <algorithm>
#include <atomic>
#include <limits>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace lux::scene
{
    SceneInstance::Impl::~Impl() noexcept
    {
        stop.request_stop();
        synchronization_hooks.clear();
        stable_point_hooks.clear();
        maintenance_hooks.clear();
        publication_hooks.clear();
        for (auto iterator = systems.rbegin(); iterator != systems.rend(); ++iterator)
        {
            if (iterator->object != nullptr)
            {
                iterator->destroy(iterator->object);
                iterator->object = nullptr;
            }
        }
        systems.clear();
        simulation.reset();
    }

    namespace
    {
        [[nodiscard]] SceneBuildFailure buildFailure(ESceneBuildError code, std::uint64_t subject_hash = 0U) noexcept
        {
            SceneBuildFailure result;
            result.code = code;
            result.subject_hash = subject_hash;
            return result;
        }

        [[nodiscard]] SceneBuildFailure systemFailure(SceneSystemBuildFailure value) noexcept
        {
            SceneBuildFailure result;
            result.code = ESceneBuildError::SCENE_SYSTEM_BUILD_FAILURE;
            result.scene_system = value;
            return result;
        }

        [[nodiscard]] const SceneCapabilityProvider* findProviderByName(
            std::span<const SceneCapabilityProvider> providers,
            std::string_view name
        ) noexcept
        {
            const auto found = std::find_if(
                providers.begin(),
                providers.end(),
                [name](const auto& provider) noexcept { return provider.name == name; }
            );
            return found != providers.end() ? std::addressof(*found) : nullptr;
        }

        [[nodiscard]] const detail::SceneSystemObjectRecord* findSystemRecord(
            std::span<const detail::SceneSystemObjectRecord> systems,
            system::SystemInstanceId instance
        ) noexcept
        {
            const auto found = std::find_if(
                systems.begin(),
                systems.end(),
                [instance](const auto& record) noexcept { return record.instance == instance; }
            );
            return found != systems.end() ? std::addressof(*found) : nullptr;
        }

        struct SceneDataPreparation final
        {
            SceneDataDependencies scene;
            std::vector<simulation::SimulationExecutionDependency> simulation;
        };

        cxx::expected<SceneDataPreparation, SceneBuildFailure> prepareDataDependencies(const SceneCreateInfo& info
        ) noexcept
        {
            const auto simulation_count = info.simulation->systemCount();
            std::vector<SceneSystemContract> systems;
            systems.reserve(simulation_count + info.scene->systemCount());
            std::map<system::SystemInstanceId, std::size_t> scene_ordinals;
            for (std::size_t i{}; i < simulation_count; ++i)
            {
                const auto selected = info.simulation->systemAt(i);
                const auto* registration = info.simulation_systems.find(selected.type());
                if (!registration)
                {
                    auto error = buildFailure(ESceneBuildError::SIMULATION_BUILD_FAILURE);
                    error.simulation = {
                        simulation::ESimulationSystemBuildError::UNKNOWN_SYSTEM_TYPE,
                        selected.instanceId()
                    };
                    return cxx::unexpected(error);
                }
                systems.push_back({selected.instanceId(), &registration->description->type, true});
            }
            for (std::size_t i{}; i < info.scene->systemCount(); ++i)
            {
                const auto selected = info.scene->systemAt(i);
                const auto registration =
                    std::ranges::find(info.scene_systems, selected.type(), &SceneSystemRegistration::type);
                if (registration == info.scene_systems.end())
                {
                    return cxx::unexpected(
                        systemFailure({ESceneSystemBuildError::UNKNOWN_SYSTEM_TYPE, selected.instanceId()})
                    );
                }
                scene_ordinals.emplace(selected.instanceId(), systems.size());
                systems.push_back({selected.instanceId(), registration->description, false});
            }
            std::vector<system::SystemDataContract> schemas;
            schemas.reserve(info.world->schemas().size());
            for (const auto& selected : info.world->schemas())
            {
                const auto* schema = info.components.find(simulation::ecs::componentSchemaId(selected.name));
                if (schema)
                {
                    schemas.push_back({selected.name, schema->version});
                }
            }
            SceneDataDependencies explicit_edges;
            for (std::size_t i{}; i < info.scene->dependencyCount(); ++i)
            {
                const auto edge = info.scene->dependencyAt(i);
                const auto before = scene_ordinals.find(edge.before());
                const auto after = scene_ordinals.find(edge.after());
                if (before == scene_ordinals.end() || after == scene_ordinals.end())
                {
                    return cxx::unexpected(systemFailure({ESceneSystemBuildError::INVALID_DESCRIPTION}));
                }
                explicit_edges.push_back({before->second, after->second});
            }
            auto resolved = resolveSceneData(systems, schemas, explicit_edges);
            if (!resolved)
            {
                auto error = buildFailure(ESceneBuildError::INCOMPATIBLE_DATA);
                error.subject_hash = cxx::Fnv1a64::hash(resolved.error().data);
                error.compatibility = std::move(resolved.error());
                return cxx::unexpected(std::move(error));
            }
            const auto points = [&](std::size_t ordinal)
            {
                const auto selected = info.simulation->systemAt(ordinal);
                std::vector<simulation::SimulationExecutionPoint> result;
                for (const auto& task : selected.tasks())
                {
                    result.push_back(simulation::SimulationExecutionPoint::task(selected.instanceId(), task.id));
                }
                for (std::size_t i{}; i < selected.hookPointCount(); ++i)
                {
                    result.push_back(
                        simulation::SimulationExecutionPoint::hook(selected.instanceId(), selected.hookPointAt(i).id())
                    );
                }
                return result;
            };
            SceneDataPreparation result;
            for (const auto edge : *resolved)
            {
                if (edge.before >= simulation_count && edge.after >= simulation_count)
                {
                    result.scene.push_back({edge.before - simulation_count, edge.after - simulation_count});
                }
                else if (edge.before < simulation_count && edge.after < simulation_count)
                {
                    const auto before = points(edge.before);
                    const auto after = points(edge.after);
                    for (const auto source : before)
                    {
                        for (const auto destination : after)
                        {
                            result.simulation.push_back({source, destination});
                        }
                    }
                }
                // Simulation completion already precedes Scene synchronization and publication.
            }
            return result;
        }
    } // namespace

    SceneInstance::SceneInstance(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    const SceneDriveSnapshot& SceneInstance::progress() const noexcept
    {
        return impl_->progress;
    }

    bool SceneInstance::atSafePoint() const noexcept
    {
        return !impl_->advancing && !impl_->stop.stop_requested() && impl_->progress.result &&
               impl_->progress.phase == ESceneDrivePhase::NONE;
    }

    bool SceneInstance::canTick() const noexcept
    {
        return atSafePoint() && impl_->maintenance_ready;
    }

    SceneInstanceId SceneInstance::id() const noexcept
    {
        return impl_->id;
    }

    SceneInstance::~SceneInstance() noexcept = default;

    std::uint64_t SceneInstance::allocateDomain() noexcept
    {
        static std::atomic<std::uint64_t> next_domain{1};
        auto next = next_domain.load(std::memory_order_relaxed);
        do
        {
            if (next == std::numeric_limits<std::uint64_t>::max())
            {
                return 0;
            }
        } while (!next_domain.compare_exchange_weak(next, next + 1, std::memory_order_relaxed));
        return next;
    }

    lux::cxx::expected<std::unique_ptr<SceneInstance>, SceneBuildFailure> SceneInstance::create(SceneCreateInfo info
    ) noexcept
    {
        const auto domain = allocateDomain();
        if (!domain)
        {
            return lux::cxx::unexpected(buildFailure(ESceneBuildError::IDENTITY_EXHAUSTED));
        }
        auto prepared = prepare(std::move(info), {domain, 0, 1});
        if (!prepared)
        {
            return lux::cxx::unexpected(prepared.error());
        }
        return std::unique_ptr<SceneInstance>(new SceneInstance(std::move(*prepared)));
    }

    lux::cxx::expected<std::unique_ptr<SceneInstance::Impl>, SceneBuildFailure> SceneInstance::prepare(
        SceneCreateInfo info,
        SceneInstanceId id
    ) noexcept
    {
        if (!info.scene)
        {
            return lux::cxx::unexpected(buildFailure(ESceneBuildError::INVALID_DESCRIPTION));
        }
        if (!info.world)
        {
            return lux::cxx::unexpected(buildFailure(ESceneBuildError::INVALID_WORLD));
        }
        if (!info.simulation)
        {
            return lux::cxx::unexpected(buildFailure(ESceneBuildError::INVALID_SIMULATION));
        }
        for (std::size_t index{}; index < info.scene_systems.size(); ++index)
        {
            const auto& registration = info.scene_systems[index];
            const bool invalid = !validSceneSystemRegistration(registration);
            const bool duplicate = std::ranges::any_of(
                info.scene_systems.first(index),
                [&](const auto& previous) { return previous.type.hash == registration.type.hash; }
            );
            if (invalid || duplicate)
            {
                return lux::cxx::unexpected(
                    systemFailure({ESceneSystemBuildError::INVALID_DESCRIPTION, {}, {}, registration.type.hash})
                );
            }
        }
        auto data_dependencies = prepareDataDependencies(info);
        if (!data_dependencies)
        {
            return cxx::unexpected(std::move(data_dependencies.error()));
        }
        for (std::size_t index{}; index < info.providers.size(); ++index)
        {
            const auto& provider = info.providers[index];
            const bool duplicate = std::any_of(
                info.providers.begin(),
                info.providers.begin() + index,
                [&](const auto& previous) noexcept { return previous.name == provider.name; }
            );
            if (provider.name.empty() || provider.capability.empty() || !provider.type.isValid() ||
                provider.value == nullptr || duplicate)
            {
                return lux::cxx::unexpected(buildFailure(
                    ESceneBuildError::INVALID_PROVIDER,
                    provider.name.empty() ? 0U : lux::cxx::Fnv1a64::hash(provider.name)
                ));
            }
        }

        {
            auto impl = std::make_unique<Impl>();
            impl->components = info.components;
            impl->id = id;
            impl->description = std::move(info.scene);
            impl->world = std::move(info.world);
            auto simulation = simulation::Simulation::create(
                impl->registry,
                std::move(info.simulation),
                info.simulation_systems,
                data_dependencies->simulation
            );
            if (!simulation)
            {
                SceneBuildFailure result = buildFailure(ESceneBuildError::SIMULATION_BUILD_FAILURE);
                result.simulation = simulation.error();
                return lux::cxx::unexpected(result);
            }
            impl->simulation.emplace(std::move(*simulation));

            const std::size_t count = impl->description->systemCount();
            std::vector<const SceneSystemRegistration*> registrations(count);
            std::vector<system::SystemInstanceId> instances;
            instances.reserve(count);
            for (std::size_t ordinal{}; ordinal < count; ++ordinal)
            {
                const auto system = impl->description->systemAt(ordinal);
                const auto found = std::ranges::find(info.scene_systems, system.type(), &SceneSystemRegistration::type);
                const auto* registration = found == info.scene_systems.end() ? nullptr : std::addressof(*found);
                if (registration == nullptr)
                {
                    return lux::cxx::unexpected(
                        systemFailure({ESceneSystemBuildError::UNKNOWN_SYSTEM_TYPE, system.instanceId()})
                    );
                }
                if (registration->description == nullptr || registration->description->version != system.version())
                {
                    return lux::cxx::unexpected(
                        systemFailure({ESceneSystemBuildError::VERSION_MISMATCH, system.instanceId()})
                    );
                }
                const auto& description = *registration->description;
                const bool invalid_schema =
                    system.configurationSchemaName() != description.configuration_schema_name ||
                    (system.configurationSchemaVersion() != description.configuration_schema_version &&
                     !registration->configurationDecoder(system.configurationSchemaVersion())) ||
                    system.configurationSchemaHash() !=
                        (description.configuration_schema_name.empty()
                             ? 0U
                             : lux::cxx::Fnv1a64::hash(description.configuration_schema_name));
                if (invalid_schema)
                {
                    return lux::cxx::unexpected(
                        systemFailure({ESceneSystemBuildError::INVALID_DESCRIPTION, system.instanceId()})
                    );
                }
                if (description.multiplicity == lux::system::ESystemMultiplicity::SINGLE_PER_OWNER)
                {
                    for (std::size_t previous{}; previous < ordinal; ++previous)
                    {
                        const auto candidate = impl->description->systemAt(previous);
                        if (candidate.type() == system.type())
                        {
                            return lux::cxx::unexpected(systemFailure(
                                {ESceneSystemBuildError::DUPLICATE_SYSTEM, system.instanceId(), candidate.instanceId()}
                            ));
                        }
                    }
                }
                for (std::size_t index = 0; index < registration->projections.size(); ++index)
                {
                    const auto& projection = registration->projections[index];
                    if (!projection.type.isValid() || !projection.project ||
                        projection.type == registration->cpp_type ||
                        std::ranges::any_of(
                            registration->projections.first(index),
                            [&](const auto& previous) { return previous.type == projection.type; }
                        ))
                    {
                        return lux::cxx::unexpected(
                            systemFailure({ESceneSystemBuildError::INVALID_DESCRIPTION, system.instanceId()})
                        );
                    }
                }
                registrations[ordinal] = registration;
                impl->code_owners.push_back(registration->code_lifetime);
                instances.push_back(system.instanceId());
            }

            auto edges = std::move(data_dependencies->scene);
            std::vector<std::vector<std::size_t>> predecessors(count);
            for (const auto edge : edges)
            {
                predecessors[edge.after].push_back(edge.before);
            }
            auto order = system::detail::deterministicSystemOrder(instances, edges);
            if (!order)
            {
                const auto code = order.error() == system::detail::ESystemDependencyOrderError::CYCLE
                                      ? ESceneSystemBuildError::DEPENDENCY_CYCLE
                                      : ESceneSystemBuildError::ALLOCATION_FAILURE;
                return lux::cxx::unexpected(systemFailure({code}));
            }

            std::vector<detail::ResolvedSceneRequirement> resolved_requirements;
            for (std::size_t ordinal{}; ordinal < count; ++ordinal)
            {
                const auto system = impl->description->systemAt(ordinal);
                const auto& registration = *registrations[ordinal];
                for (const auto& requirement : registration.requirements)
                {
                    const auto binding = system.findRequirementBinding(requirement.name);
                    const SceneCapabilityProvider* selected{};
                    if (binding)
                    {
                        selected = findProviderByName(info.providers, binding.provider());
                        if (selected == nullptr || selected->capability != requirement.capability)
                        {
                            return lux::cxx::unexpected(systemFailure(
                                {ESceneSystemBuildError::INVALID_REQUIREMENT_BINDING,
                                 system.instanceId(),
                                 {},
                                 lux::cxx::Fnv1a64::hash(requirement.name)}
                            ));
                        }
                        if (selected->type != requirement.expected_type)
                        {
                            return lux::cxx::unexpected(systemFailure(
                                {ESceneSystemBuildError::REQUIREMENT_TYPE_MISMATCH,
                                 system.instanceId(),
                                 {},
                                 lux::cxx::Fnv1a64::hash(requirement.name)}
                            ));
                        }
                    }
                    else
                    {
                        for (const auto& provider : info.providers)
                        {
                            if (provider.capability != requirement.capability ||
                                provider.type != requirement.expected_type)
                            {
                                continue;
                            }
                            if (selected != nullptr)
                            {
                                return lux::cxx::unexpected(systemFailure(
                                    {ESceneSystemBuildError::AMBIGUOUS_REQUIREMENT,
                                     system.instanceId(),
                                     {},
                                     lux::cxx::Fnv1a64::hash(requirement.name)}
                                ));
                            }
                            selected = &provider;
                        }
                    }
                    if (selected == nullptr)
                    {
                        if (!requirement.optional)
                        {
                            return lux::cxx::unexpected(systemFailure(
                                {ESceneSystemBuildError::MISSING_REQUIREMENT,
                                 system.instanceId(),
                                 {},
                                 lux::cxx::Fnv1a64::hash(requirement.name)}
                            ));
                        }
                        continue;
                    }
                    resolved_requirements.push_back(
                        {system.instanceId(), requirement.name, requirement.expected_type, selected->value}
                    );
                }
            }

            SceneSystemInstaller::Impl build;
            build.instance = impl->id;
            build.registry = &impl->registry;
            build.simulation = std::addressof(*impl->simulation);
            build.components = &impl->components;
            build.systems = &impl->systems;
            build.synchronization_hooks = &impl->synchronization_hooks;
            build.stable_hooks = &impl->stable_point_hooks;
            build.maintenance_hooks = &impl->maintenance_hooks;
            build.publication_hooks = &impl->publication_hooks;
            build.description = impl->description.get();
            build.predecessors = std::move(predecessors);
            build.requirements = std::move(resolved_requirements);
            SceneSystemInstaller builder(build);
            for (const std::size_t ordinal : *order)
            {
                build.current_ordinal = ordinal;
                build.current_registration = registrations[ordinal];
                const auto system = impl->description->systemAt(ordinal);
                auto installed = registrations[ordinal]->install(builder, system);
                if (!installed)
                {
                    return lux::cxx::unexpected(systemFailure(installed.error()));
                }
                if (build.pending_failure)
                {
                    return lux::cxx::unexpected(systemFailure(*build.pending_failure));
                }
                if (findSystemRecord(impl->systems, system.instanceId()) == nullptr)
                {
                    return lux::cxx::unexpected(
                        systemFailure({ESceneSystemBuildError::INVALID_DESCRIPTION, system.instanceId()})
                    );
                }
            }

            return impl;
        }
    }

    simulation::ecs::Registry& SceneInstance::registry() noexcept
    {
        return impl_->registry;
    }

    const simulation::ecs::Registry& SceneInstance::registry() const noexcept
    {
        return impl_->registry;
    }

    simulation::Simulation& SceneInstance::simulation() noexcept
    {
        return *impl_->simulation;
    }

    const simulation::Simulation& SceneInstance::simulation() const noexcept
    {
        return *impl_->simulation;
    }

    void* SceneInstance::findSceneSystemErased(lux::cxx::TypeToken type) noexcept
    {
        return const_cast<void*>(std::as_const(*this).findSceneSystemErased(type));
    }

    const void* SceneInstance::findSceneSystemErased(lux::cxx::TypeToken type) const noexcept
    {
        const void* result{};
        for (const auto& record : impl_->systems)
        {
            if (const auto* candidate = record.project(type))
            {
                if (result)
                {
                    return nullptr; // Ambiguous capability is not selected by order.
                }
                result = candidate;
            }
        }
        return result;
    }

    std::stop_token SceneInstance::stopToken() const noexcept
    {
        return impl_->stop.get_token();
    }

    void SceneInstance::requestStop() noexcept
    {
        impl_->stop.request_stop();
    }
} // namespace lux::scene
