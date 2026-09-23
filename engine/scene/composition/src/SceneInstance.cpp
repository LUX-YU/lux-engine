#include <lux/engine/scene/SceneInstance.hpp>

#include <lux/engine/scene/SceneBuilder.hpp>
#include <lux/engine/scene/detail/SceneInstanceImpl.hpp>
#include <lux/engine/system/detail/SystemDependencyOrder.hpp>

#include <algorithm>
#include <atomic>
#include <limits>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace lux::scene
{
SceneInstance::Impl::~Impl() noexcept
{
    stop.request_stop();
    connections.clear();
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

[[nodiscard]] const SceneCapabilityProvider *findProviderByName(std::span<const SceneCapabilityProvider> providers,
                                                                std::string_view name) noexcept
{
    const auto found = std::find_if(providers.begin(), providers.end(),
                                    [name](const auto &provider) noexcept { return provider.name == name; });
    return found != providers.end() ? std::addressof(*found) : nullptr;
}

[[nodiscard]] const detail::SceneSystemObjectRecord *findSystemRecord(
    std::span<const detail::SceneSystemObjectRecord> systems, system::SystemInstanceId instance) noexcept
{
    const auto found = std::find_if(systems.begin(), systems.end(),
                                    [instance](const auto &record) noexcept { return record.instance == instance; });
    return found != systems.end() ? std::addressof(*found) : nullptr;
}
} // namespace

SceneInstance::SceneInstance(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
{
}

const SceneDriveSnapshot &SceneInstance::progress() const noexcept
{
    return impl_->progress;
}

SceneInstanceId SceneInstance::id() const noexcept
{
    return impl_->id;
}

SceneInstance::~SceneInstance() noexcept = default;

lux::cxx::expected<std::unique_ptr<SceneInstance>, SceneBuildFailure> SceneInstance::create(
    SceneCreateInfo info) noexcept
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
        const auto &registration = info.scene_systems[index];
        const bool invalid = !validSceneSystemRegistration(registration);
        const bool duplicate = std::ranges::any_of(info.scene_systems.first(index), [&](const auto &previous) {
            return previous.type.hash == registration.type.hash;
        });
        if (invalid || duplicate)
            return lux::cxx::unexpected(systemFailure({ESceneSystemBuildError::INVALID_DESCRIPTION, {}, {}, registration.type.hash}));
    }
    // Derivation can also refresh a paused evolution SceneInstance.
    {
        for (std::size_t index{}; index < info.simulation->systemCount(); ++index)
        {
            const auto system = info.simulation->systemAt(index);
            const auto *registration = info.simulation_systems.find(system.type());
            // Unknown types retain Simulation::create's original error identity below.
            if (!registration || !registration->supports_derivation)
            {
                continue;
            }
            for (const auto &access : registration->access.components)
            {
                if (access.mode != simulation::ESystemAccessMode::WRITE)
                {
                    continue;
                }
                const auto *schema = info.components.find(access.type);
                const bool derived =
                    schema && schema->semantic_kind == simulation::ecs::EComponentSemanticKind::RUNTIME_DERIVED;
                if (!derived)
                {
                    auto error = buildFailure(ESceneBuildError::INVALID_DERIVATION_ACCESS, access.type.hash());
                    error.simulation.system = system.instanceId();
                    return lux::cxx::unexpected(error);
                }
            }
        }
    }
    for (std::size_t index{}; index < info.providers.size(); ++index)
    {
        const auto &provider = info.providers[index];
        const bool duplicate =
            std::any_of(info.providers.begin(), info.providers.begin() + index,
                        [&](const auto &previous) noexcept { return previous.name == provider.name; });
        if (provider.name.empty() || provider.capability.empty() || !provider.type.isValid() ||
            provider.value == nullptr || duplicate)
        {
            return lux::cxx::unexpected(
                buildFailure(ESceneBuildError::INVALID_PROVIDER,
                             provider.name.empty() ? 0U : lux::cxx::Fnv1a64::hash(provider.name)));
        }
    }

    try
    {
        auto impl = std::make_unique<Impl>();
        impl->components = info.components;
        static std::atomic<std::uint64_t> next_instance{1};
        auto next = next_instance.load(std::memory_order_relaxed);
        do
        {
            if (next == (std::numeric_limits<std::uint64_t>::max)())
            {
                return lux::cxx::unexpected(buildFailure(ESceneBuildError::IDENTITY_EXHAUSTED));
            }
        } while (!next_instance.compare_exchange_weak(next, next + 1, std::memory_order_relaxed));
        impl->id = {next};
        if (info.fixed_delta.count() <= 0)
        {
            return lux::cxx::unexpected(buildFailure(ESceneBuildError::INVALID_FIXED_DELTA));
        }
        impl->fixed_delta = info.fixed_delta;
        impl->description = std::move(info.scene);
        impl->world = std::move(info.world);
        auto simulation = simulation::Simulation::create(impl->registry, std::move(info.simulation),
                                                         info.simulation_systems, info.simulation_mode);
        if (!simulation)
        {
            SceneBuildFailure result = buildFailure(ESceneBuildError::SIMULATION_BUILD_FAILURE);
            result.simulation = simulation.error();
            return lux::cxx::unexpected(result);
        }
        impl->simulation.emplace(std::move(*simulation));

        const std::size_t count = impl->description->systemCount();
        std::vector<const SceneSystemRegistration *> registrations(count);
        std::vector<system::SystemInstanceId> instances;
        instances.reserve(count);
        for (std::size_t ordinal{}; ordinal < count; ++ordinal)
        {
            const auto system = impl->description->systemAt(ordinal);
            const auto found = std::ranges::find(info.scene_systems, system.type(), &SceneSystemRegistration::type);
            const auto *registration = found == info.scene_systems.end() ? nullptr : std::addressof(*found);
            if (registration == nullptr)
            {
                return lux::cxx::unexpected(
                    systemFailure({ESceneSystemBuildError::UNKNOWN_SYSTEM_TYPE, system.instanceId()}));
            }
            if (registration->description == nullptr || registration->description->version != system.version())
            {
                return lux::cxx::unexpected(
                    systemFailure({ESceneSystemBuildError::VERSION_MISMATCH, system.instanceId()}));
            }
            const auto &description = *registration->description;
            const bool invalid_schema =
                system.configurationSchemaName() != description.configuration_schema_name ||
                system.configurationSchemaVersion() != description.configuration_schema_version ||
                system.configurationSchemaHash() !=
                    (description.configuration_schema_name.empty()
                         ? 0U
                         : lux::cxx::Fnv1a64::hash(description.configuration_schema_name));
            if (invalid_schema)
            {
                return lux::cxx::unexpected(
                    systemFailure({ESceneSystemBuildError::INVALID_DESCRIPTION, system.instanceId()}));
            }
            if (description.multiplicity == lux::system::ESystemMultiplicity::SINGLE_PER_OWNER)
            {
                for (std::size_t previous{}; previous < ordinal; ++previous)
                {
                    const auto candidate = impl->description->systemAt(previous);
                    if (candidate.type() == system.type())
                    {
                        return lux::cxx::unexpected(systemFailure(
                            {ESceneSystemBuildError::DUPLICATE_SYSTEM, system.instanceId(), candidate.instanceId()}));
                    }
                }
            }
            for (std::size_t index = 0; index < registration->projections.size(); ++index)
            {
                const auto &projection = registration->projections[index];
                if (!projection.type.isValid() || !projection.project || projection.type == registration->cpp_type ||
                    std::ranges::any_of(registration->projections.first(index),
                                        [&](const auto &previous) { return previous.type == projection.type; }))
                {
                    return lux::cxx::unexpected(
                        systemFailure({ESceneSystemBuildError::INVALID_DESCRIPTION, system.instanceId()}));
                }
            }
            registrations[ordinal] = registration;
            impl->code_owners.push_back(registration->code_lifetime);
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
                if (instances[ordinal] == edge.before())
                {
                    before = ordinal;
                }
                if (instances[ordinal] == edge.after())
                {
                    after = ordinal;
                }
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
            const auto &registration = *registrations[ordinal];
            for (const auto &requirement : registration.requirements)
            {
                const auto binding = system.findRequirementBinding(requirement.name);
                const SceneCapabilityProvider *selected{};
                if (binding)
                {
                    selected = findProviderByName(info.providers, binding.provider());
                    if (selected == nullptr || selected->capability != requirement.capability)
                    {
                        return lux::cxx::unexpected(systemFailure({ESceneSystemBuildError::INVALID_REQUIREMENT_BINDING,
                                                                   system.instanceId(),
                                                                   {},
                                                                   lux::cxx::Fnv1a64::hash(requirement.name)}));
                    }
                    if (selected->type != requirement.expected_type)
                    {
                        return lux::cxx::unexpected(systemFailure({ESceneSystemBuildError::REQUIREMENT_TYPE_MISMATCH,
                                                                   system.instanceId(),
                                                                   {},
                                                                   lux::cxx::Fnv1a64::hash(requirement.name)}));
                    }
                }
                else
                {
                    for (const auto &provider : info.providers)
                    {
                        if (provider.capability != requirement.capability || provider.type != requirement.expected_type)
                        {
                            continue;
                        }
                        if (selected != nullptr)
                        {
                            return lux::cxx::unexpected(systemFailure({ESceneSystemBuildError::AMBIGUOUS_REQUIREMENT,
                                                                       system.instanceId(),
                                                                       {},
                                                                       lux::cxx::Fnv1a64::hash(requirement.name)}));
                        }
                        selected = &provider;
                    }
                }
                if (selected == nullptr)
                {
                    if (!requirement.optional)
                    {
                        return lux::cxx::unexpected(systemFailure({ESceneSystemBuildError::MISSING_REQUIREMENT,
                                                                   system.instanceId(),
                                                                   {},
                                                                   lux::cxx::Fnv1a64::hash(requirement.name)}));
                    }
                    continue;
                }
                resolved_requirements.push_back({system.instanceId(), requirement.name, requirement.expected_type,
                                                 selected->value, selected->object});
            }
        }

        SceneBuilder::Impl build;
        build.instance = impl->id;
        build.registry = &impl->registry;
        build.simulation = std::addressof(*impl->simulation);
        build.components = &impl->components;
        build.systems = &impl->systems;
        build.stable_hooks = &impl->stable_point_hooks;
        build.maintenance_hooks = &impl->maintenance_hooks;
        build.publication_hooks = &impl->publication_hooks;
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
                return lux::cxx::unexpected(
                    systemFailure({ESceneSystemBuildError::INVALID_DESCRIPTION, system.instanceId()}));
            }
        }

        for (const std::size_t ordinal : *order)
        {
            const auto system = impl->description->systemAt(ordinal);
            const auto &registration = *registrations[ordinal];
            const auto *self = findSystemRecord(impl->systems, system.instanceId());
            for (const auto &connection : registration.connections)
            {
                const auto endpoint = [&](const SceneObjectEndpointRef &reference) -> object::LuxObject * {
                    if (reference.owner == ESceneConnectionOwner::SELF)
                    {
                        return self->object_endpoint;
                    }
                    const auto found = std::find_if(
                        build.requirements.begin(), build.requirements.end(), [&](const auto &value) noexcept {
                            return value.system == system.instanceId() && value.name == reference.requirement;
                        });
                    return found == build.requirements.end() ? nullptr : found->object;
                };
                auto *sender = endpoint(connection.signal);
                auto *receiver = endpoint(connection.method);
                if (!sender || !receiver || !connection.connect)
                {
                    return lux::cxx::unexpected(
                        systemFailure({ESceneSystemBuildError::CONNECTION_FAILURE, system.instanceId()}));
                }
                auto observed = connection.connect(*sender, *receiver);
                if (!observed)
                {
                    return lux::cxx::unexpected(
                        systemFailure({ESceneSystemBuildError::CONNECTION_FAILURE, system.instanceId()}));
                }
                impl->connections.push_back(std::move(*observed));
            }
        }
        return std::unique_ptr<SceneInstance>(new SceneInstance(std::move(impl)));
    }
    catch (const std::bad_alloc &)
    {
        return lux::cxx::unexpected(buildFailure(ESceneBuildError::ALLOCATION_FAILURE));
    }
}

const SceneDescription &SceneInstance::description() const noexcept
{
    return *impl_->description;
}

const world::WorldDescription &SceneInstance::worldDescription() const noexcept
{
    return *impl_->world;
}

simulation::ecs::Registry &SceneInstance::registry() noexcept
{
    return impl_->registry;
}

const simulation::ecs::Registry &SceneInstance::registry() const noexcept
{
    return impl_->registry;
}

simulation::Simulation &SceneInstance::simulation() noexcept
{
    return *impl_->simulation;
}

const simulation::Simulation &SceneInstance::simulation() const noexcept
{
    return *impl_->simulation;
}

void *SceneInstance::findSceneSystemErased(lux::cxx::TypeToken type) noexcept
{
    return const_cast<void *>(std::as_const(*this).findSceneSystemErased(type));
}

const void *SceneInstance::findSceneSystemErased(lux::cxx::TypeToken type) const noexcept
{
    const void *result{};
    for (const auto &record : impl_->systems)
    {
        if (const auto *candidate = record.project(type))
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

bool SceneInstance::hasCapability(std::string_view capability) const noexcept
{
    if (impl_->simulation->description().hasCapability(capability))
    {
        return true;
    }
    return std::ranges::any_of(impl_->systems, [capability](const auto &system) noexcept {
        return system.description != nullptr && std::ranges::find(system.description->capabilities, capability) !=
                                                    system.description->capabilities.end();
    });
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
