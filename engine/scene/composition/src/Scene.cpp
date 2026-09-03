#include <lux/engine/scene/Scene.hpp>

#include <lux/engine/object/ObjectReflection.hpp>
#include <lux/engine/scene/SceneBuilder.hpp>
#include <lux/engine/scene/detail/SceneBuilderImpl.hpp>
#include <lux/engine/system/detail/SystemDependencyOrder.hpp>

#include <algorithm>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace lux::scene
{
    struct Scene::Impl final
    {
        ~Impl() noexcept
        {
            stop.request_stop();
            connections.clear();
            stable_point_hooks.clear();
            presentation_hooks.clear();
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

        std::stop_source stop;
        std::shared_ptr<const SceneDescription> description;
        std::shared_ptr<const world::WorldDescription> world;
        simulation::ecs::Registry registry;
        std::optional<simulation::Simulation> simulation;
        std::vector<detail::SceneSystemObjectRecord> systems;
        std::vector<object::Connection> connections;
        std::vector<detail::SceneHookRecord> stable_point_hooks;
        std::vector<detail::SceneHookRecord> presentation_hooks;
    };

    namespace
    {
        [[nodiscard]] SceneBuildFailure buildFailure(
            ESceneBuildError code,
            std::uint64_t subject_hash = 0U
        ) noexcept
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
            const auto found = std::find_if(providers.begin(), providers.end(), [name](const auto& provider) noexcept {
                return provider.name == name;
            });
            return found != providers.end() ? std::addressof(*found) : nullptr;
        }

        [[nodiscard]] const detail::SceneSystemObjectRecord* findSystemRecord(
            std::span<const detail::SceneSystemObjectRecord> systems,
            system::SystemInstanceId instance
        ) noexcept
        {
            const auto found = std::find_if(systems.begin(), systems.end(), [instance](const auto& record) noexcept {
                return record.instance == instance;
            });
            return found != systems.end() ? std::addressof(*found) : nullptr;
        }
    } // namespace

    Scene::Scene(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
    {
    }

    Scene::~Scene() noexcept = default;

    lux::cxx::expected<std::unique_ptr<Scene>, SceneBuildFailure> Scene::create(SceneCreateInfo info) noexcept
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

        try
        {
            auto impl = std::make_unique<Impl>();
            impl->description = std::move(info.scene);
            impl->world = std::move(info.world);
            auto simulation = simulation::Simulation::create(
                impl->registry,
                std::move(info.simulation),
                info.meta.simulationSystems()
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
                const auto* registration = info.meta.getSceneSystemMeta(system.type());
                if (registration == nullptr)
                {
                    return lux::cxx::unexpected(systemFailure({
                        ESceneSystemBuildError::UNKNOWN_SYSTEM_TYPE,
                        system.instanceId()
                    }));
                }
                if (registration->description == nullptr || registration->description->version != system.version())
                {
                    return lux::cxx::unexpected(systemFailure({
                        ESceneSystemBuildError::VERSION_MISMATCH,
                        system.instanceId()
                    }));
                }
                const auto& description = *registration->description;
                const bool invalid_schema = system.configurationSchemaName() != description.configuration_schema_name ||
                    system.configurationSchemaVersion() != description.configuration_schema_version ||
                    system.configurationSchemaHash() != (description.configuration_schema_name.empty()
                        ? 0U
                        : lux::cxx::Fnv1a64::hash(description.configuration_schema_name));
                if (invalid_schema)
                {
                    return lux::cxx::unexpected(systemFailure({
                        ESceneSystemBuildError::INVALID_DESCRIPTION,
                        system.instanceId()
                    }));
                }
                if (description.multiplicity == lux::system::ESystemMultiplicity::SINGLE_PER_OWNER)
                {
                    for (std::size_t previous{}; previous < ordinal; ++previous)
                    {
                        const auto candidate = impl->description->systemAt(previous);
                        if (candidate.type() == system.type())
                        {
                            return lux::cxx::unexpected(systemFailure({
                                ESceneSystemBuildError::DUPLICATE_SYSTEM,
                                system.instanceId(),
                                candidate.instanceId()
                            }));
                        }
                    }
                }
                registrations[ordinal] = registration;
                instances.push_back(system.instanceId());
            }

            std::vector<system::detail::SystemDependencyOrdinalEdge> edges;
            std::vector<std::vector<std::size_t>> predecessors(count);
            edges.reserve(impl->description->dependencyCount());
            for (std::size_t dependency{}; dependency < impl->description->dependencyCount(); ++dependency)
            {
                const auto edge = impl->description->dependencyAt(dependency);
                std::size_t before{count}, after{count};
                for (std::size_t ordinal{}; ordinal < count; ++ordinal)
                {
                    if (instances[ordinal] == edge.before()) before = ordinal;
                    if (instances[ordinal] == edge.after()) after = ordinal;
                }
                if (before == count || after == count || before == after)
                {
                    return lux::cxx::unexpected(systemFailure({ESceneSystemBuildError::INVALID_DESCRIPTION}));
                }
                edges.push_back({before, after});
                predecessors[after].push_back(before);
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
                            return lux::cxx::unexpected(systemFailure({
                                ESceneSystemBuildError::INVALID_REQUIREMENT_BINDING,
                                system.instanceId(),
                                {},
                                lux::cxx::Fnv1a64::hash(requirement.name)
                            }));
                        }
                        if (selected->type != requirement.expected_type)
                        {
                            return lux::cxx::unexpected(systemFailure({
                                ESceneSystemBuildError::REQUIREMENT_TYPE_MISMATCH,
                                system.instanceId(),
                                {},
                                lux::cxx::Fnv1a64::hash(requirement.name)
                            }));
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
                                return lux::cxx::unexpected(systemFailure({
                                    ESceneSystemBuildError::AMBIGUOUS_REQUIREMENT,
                                    system.instanceId(),
                                    {},
                                    lux::cxx::Fnv1a64::hash(requirement.name)
                                }));
                            }
                            selected = &provider;
                        }
                    }
                    if (selected == nullptr)
                    {
                        if (!requirement.optional)
                        {
                            return lux::cxx::unexpected(systemFailure({
                                ESceneSystemBuildError::MISSING_REQUIREMENT,
                                system.instanceId(),
                                {},
                                lux::cxx::Fnv1a64::hash(requirement.name)
                            }));
                        }
                        continue;
                    }
                    resolved_requirements.push_back({
                        system.instanceId(),
                        requirement.name,
                        requirement.expected_type,
                        selected->value,
                        selected->object
                    });
                }
            }

            SceneBuilder::Impl build;
            build.registry = &impl->registry;
            build.simulation = std::addressof(*impl->simulation);
            build.meta = &info.meta;
            build.systems = &impl->systems;
            build.stable_hooks = &impl->stable_point_hooks;
            build.presentation_hooks = &impl->presentation_hooks;
            build.description = impl->description.get();
            build.predecessors = std::move(predecessors);
            build.requirements = std::move(resolved_requirements);
            SceneBuilder builder(build);
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
                    return lux::cxx::unexpected(systemFailure({
                        ESceneSystemBuildError::INVALID_DESCRIPTION,
                        system.instanceId()
                    }));
                }
            }

            for (const std::size_t ordinal : *order)
            {
                const auto system = impl->description->systemAt(ordinal);
                const auto& registration = *registrations[ordinal];
                const auto* self = findSystemRecord(impl->systems, system.instanceId());
                for (const auto& connection : registration.connections)
                {
                    const auto endpoint = [&](const SceneObjectEndpointRef& reference)
                        -> std::pair<object::LuxObject*, const meta::RefClass*> {
                        if (reference.owner == ESceneConnectionOwner::SELF)
                        {
                            return {
                                self->object_endpoint,
                                meta::ReflectionRegistry::instance().findClass(self->type.name())
                            };
                        }
                        const auto found = std::find_if(
                            build.requirements.begin(),
                            build.requirements.end(),
                            [&](const auto& value) noexcept {
                                return value.system == system.instanceId() && value.name == reference.requirement;
                            }
                        );
                        if (found == build.requirements.end() || found->object == nullptr)
                        {
                            return {};
                        }
                        return {
                            found->object,
                            meta::ReflectionRegistry::instance().findClass(found->object->objectType().name())
                        };
                    };
                    const auto [sender, sender_class] = endpoint(connection.signal);
                    const auto [receiver, receiver_class] = endpoint(connection.method);
                    if (sender == nullptr || receiver == nullptr || sender_class == nullptr ||
                        receiver_class == nullptr)
                    {
                        return lux::cxx::unexpected(systemFailure({
                            ESceneSystemBuildError::CONNECTION_FAILURE,
                            system.instanceId()
                        }));
                    }
                    const auto signal = object::reflection::findSignal(
                        meta::ReflectionRegistry::instance(),
                        *sender_class,
                        connection.signal.member
                    );
                    const auto method = std::find_if(
                        receiver_class->methods.begin(),
                        receiver_class->methods.end(),
                        [&](const auto& value) noexcept { return value.invokable.name == connection.method.member; }
                    );
                    if (!signal || method == receiver_class->methods.end())
                    {
                        return lux::cxx::unexpected(systemFailure({
                            ESceneSystemBuildError::CONNECTION_FAILURE,
                            system.instanceId()
                        }));
                    }
                    auto observed = object::reflection::observe(
                        *sender,
                        signal,
                        *receiver,
                        *method,
                        connection.delivery
                    );
                    if (!observed)
                    {
                        return lux::cxx::unexpected(systemFailure({
                            ESceneSystemBuildError::CONNECTION_FAILURE,
                            system.instanceId()
                        }));
                    }
                    impl->connections.push_back(std::move(*observed));
                }
            }
            return std::unique_ptr<Scene>(new Scene(std::move(impl)));
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(buildFailure(ESceneBuildError::ALLOCATION_FAILURE));
        }
    }

    const SceneDescription& Scene::description() const noexcept
    {
        return *impl_->description;
    }

    const world::WorldDescription& Scene::world() const noexcept
    {
        return *impl_->world;
    }

    simulation::ecs::Registry& Scene::registry() noexcept
    {
        return impl_->registry;
    }

    const simulation::ecs::Registry& Scene::registry() const noexcept
    {
        return impl_->registry;
    }

    simulation::Simulation& Scene::simulation() noexcept
    {
        return *impl_->simulation;
    }

    const simulation::Simulation& Scene::simulation() const noexcept
    {
        return *impl_->simulation;
    }

    void* Scene::findSceneSystemErased(lux::cxx::TypeToken type) noexcept
    {
        const auto found =
            std::find_if(impl_->systems.begin(), impl_->systems.end(), [type](const auto& record) noexcept {
                return record.type == type;
            });
        return found != impl_->systems.end() ? found->object : nullptr;
    }

    const void* Scene::findSceneSystemErased(lux::cxx::TypeToken type) const noexcept
    {
        const auto found =
            std::find_if(impl_->systems.begin(), impl_->systems.end(), [type](const auto& record) noexcept {
                return record.type == type;
            });
        return found != impl_->systems.end() ? found->object : nullptr;
    }

    lux::cxx::expected<void, SceneExecutionFailure> Scene::executeStablePoint() noexcept
    {
        for (auto& hook : impl_->stable_point_hooks)
        {
            if (!hook.invoke())
            {
                return lux::cxx::unexpected(SceneExecutionFailure{ESceneExecutionError::SYSTEM_FAILURE, hook.system});
            }
        }
        return {};
    }

    lux::cxx::expected<void, SceneExecutionFailure> Scene::executePresentation() noexcept
    {
        for (auto& hook : impl_->presentation_hooks)
        {
            if (!hook.invoke())
            {
                return lux::cxx::unexpected(SceneExecutionFailure{ESceneExecutionError::SYSTEM_FAILURE, hook.system});
            }
        }
        return {};
    }

    bool Scene::hasCapability(std::string_view capability) const noexcept
    {
        if (impl_->simulation->description().hasCapability(capability))
        {
            return true;
        }
        return std::ranges::any_of(impl_->systems, [capability](const auto& system) noexcept {
            return system.description != nullptr && std::ranges::find(system.description->capabilities, capability) !=
                system.description->capabilities.end();
        });
    }

    std::stop_token Scene::stopToken() const noexcept
    {
        return impl_->stop.get_token();
    }

    void Scene::requestStop() noexcept
    {
        impl_->stop.request_stop();
    }
} // namespace lux::scene
