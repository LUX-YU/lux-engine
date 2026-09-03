#include <lux/engine/scene/SceneMetaManager.hpp>

#include <lux/engine/object/ObjectReflection.hpp>

#include <algorithm>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>

namespace lux::scene
{
    struct SceneMetaManager::Impl final
    {
        struct UsageRange final
        {
            std::size_t offset{};
            std::size_t count{};
        };

        struct TransparentStringHash final
        {
            using is_transparent = void;
            [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept
            {
                return static_cast<std::size_t>(lux::cxx::Fnv1a64::hash(value));
            }
            [[nodiscard]] std::size_t operator()(const std::string& value) const noexcept
            {
                return (*this)(std::string_view(value));
            }
        };

        simulation::ecs::ComponentSchemaSet components;
        simulation::SimulationSystemRegistry simulation_systems;
        std::vector<SceneSystemRegistration> scene_systems;
        std::vector<render::RenderFeatureRegistration> render_feature_registrations;
        std::vector<RenderFeatureMeta> render_features;
        std::vector<std::vector<std::byte>> render_feature_default_configuration_storage;
        std::vector<std::vector<std::byte>> system_default_configuration_storage;
        std::vector<SystemMetaView> all_systems;
        std::unordered_map<std::uint64_t, std::size_t> system_by_hash;
        std::unordered_map<std::uint64_t, std::size_t> scene_system_by_hash;
        std::unordered_map<std::uint64_t, std::size_t> render_feature_by_hash;
        std::vector<ComponentSystemUsage> component_usage;
        std::vector<UsageRange> component_usage_ranges;
        std::unordered_map<
            std::string,
            std::vector<system::SystemTypeId>,
            TransparentStringHash,
            std::equal_to<>> capability_index;
    };

    namespace
    {
        [[nodiscard]] SceneMetaFailure failure(
            ESceneMetaError code,
            std::uint64_t subject_hash = 0U,
            lux::serialization::SerializationFailure configuration = {}
        ) noexcept
        {
            return {code, subject_hash, configuration};
        }

        [[nodiscard]] bool validSemantic(simulation::ecs::EComponentSemanticKind value) noexcept
        {
            using Kind = simulation::ecs::EComponentSemanticKind;
            return value == Kind::FOUNDATION || value == Kind::DOMAIN_CONTRACT ||
                value == Kind::IMPLEMENTATION_EXTENSION || value == Kind::RUNTIME_DERIVED;
        }

        [[nodiscard]] const meta::RefClass* configurationReflection(
            const lux::serialization::PortableValueCodec& codec
        ) noexcept
        {
            if (!codec.valid())
            {
                return nullptr;
            }
            const auto* reflection = meta::ReflectionRegistry::instance().findClass(codec.type.name());
            return reflection != nullptr && reflection->construct != nullptr && reflection->destruct != nullptr
                ? reflection
                : nullptr;
        }

        [[nodiscard]] bool sameType(const system::SystemTypeId& type, const system::SystemTypeDescription& description)
            noexcept
        {
            return type.valid() && description.canonical_name == type.name &&
                type.hash == lux::cxx::Fnv1a64::hash(description.canonical_name);
        }
    } // namespace

    SceneMetaManager::SceneMetaManager(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
    {
    }

    SceneMetaManager::SceneMetaManager(SceneMetaManager&&) noexcept = default;
    SceneMetaManager& SceneMetaManager::operator=(SceneMetaManager&&) noexcept = default;
    SceneMetaManager::~SceneMetaManager() = default;

    lux::cxx::expected<SceneMetaManager, SceneMetaFailure> SceneMetaManager::build(BuildInfo info) noexcept
    {
        if (!meta::ReflectionRegistry::initialized())
        {
            return lux::cxx::unexpected(failure(ESceneMetaError::INVALID_REFLECTION_REGISTRY));
        }
        try
        {
            auto impl = std::make_unique<Impl>();
            impl->components = std::move(info.components);
            impl->simulation_systems = std::move(info.simulation_systems);
            impl->scene_systems = std::move(info.scene_systems);
            impl->render_feature_registrations = std::move(info.render_features);

            for (const auto& component : impl->components.all())
            {
                if (!component.id.valid() || !component.cpp_type.isValid() || !validSemantic(component.semantic_kind))
                {
                    return lux::cxx::unexpected(failure(ESceneMetaError::INVALID_COMPONENT_SCHEMA, component.id.hash));
                }
            }

            const std::size_t system_count = impl->simulation_systems.size() + impl->scene_systems.size();
            impl->system_default_configuration_storage.reserve(system_count);
            impl->all_systems.reserve(system_count);
            impl->system_by_hash.reserve(system_count);
            impl->scene_system_by_hash.reserve(impl->scene_systems.size());

            for (const auto& registration : impl->simulation_systems.all())
            {
                if (registration.description == nullptr || !registration.cpp_type.isValid() ||
                    !validSimulationSystemDescription(*registration.description) ||
                    !sameType(registration.type, registration.description->type) || registration.install == nullptr)
                {
                    return lux::cxx::unexpected(
                        failure(ESceneMetaError::INVALID_SIMULATION_SYSTEM, registration.type.hash)
                    );
                }
                const bool has_configuration = !registration.description->type.configuration_schema_name.empty();
                const auto* reflection = configurationReflection(registration.configuration);
                if (has_configuration && reflection == nullptr)
                {
                    return lux::cxx::unexpected(failure(
                        ESceneMetaError::CONFIGURATION_REFLECTION_NOT_FOUND,
                        registration.type.hash
                    ));
                }
                impl->system_default_configuration_storage.emplace_back();
                auto& defaults = impl->system_default_configuration_storage.back();
                if (has_configuration)
                {
                    auto encoded = registration.configuration.encode_default(defaults);
                    if (!encoded)
                    {
                        return lux::cxx::unexpected(failure(
                            ESceneMetaError::CONFIGURATION_DEFAULT_ENCODING_FAILURE,
                            registration.type.hash,
                            encoded.error()
                        ));
                    }
                }
                impl->all_systems.push_back({
                    ESystemDomain::SIMULATION,
                    registration.type,
                    registration.cpp_type,
                    &registration.description->type,
                    registration.configuration,
                    reflection,
                    defaults
                });
            }

            for (std::size_t index{}; index < impl->scene_systems.size(); ++index)
            {
                const auto& registration = impl->scene_systems[index];
                if (!registration.type.valid() || !registration.cpp_type.isValid() ||
                    registration.description == nullptr ||
                    !system::validSystemTypeDescription(*registration.description) ||
                    !sameType(registration.type, *registration.description) || registration.install == nullptr)
                {
                    return lux::cxx::unexpected(failure(ESceneMetaError::INVALID_SCENE_SYSTEM, registration.type.hash));
                }
                for (std::size_t requirement{}; requirement < registration.requirements.size(); ++requirement)
                {
                    const auto& value = registration.requirements[requirement];
                    const bool duplicate = std::any_of(
                        registration.requirements.begin(),
                        registration.requirements.begin() + requirement,
                        [&](const auto& previous) noexcept { return previous.name == value.name; }
                    );
                    if (value.name.empty() || value.capability.empty() || !value.expected_type.isValid() || duplicate)
                    {
                        return lux::cxx::unexpected(
                            failure(ESceneMetaError::INVALID_REQUIREMENT, registration.type.hash)
                        );
                    }
                }
                const meta::RefClass* object_reflection{};
                if (!registration.connections.empty())
                {
                    if (registration.project_object == nullptr)
                    {
                        return lux::cxx::unexpected(failure(ESceneMetaError::INVALID_CONNECTION, registration.type.hash)
                        );
                    }
                    object_reflection = meta::ReflectionRegistry::instance().findClass(registration.cpp_type.name());
                    if (object_reflection == nullptr)
                    {
                        return lux::cxx::unexpected(failure(ESceneMetaError::INVALID_CONNECTION, registration.type.hash)
                        );
                    }
                }
                for (const auto& connection : registration.connections)
                {
                    const bool invalid_member = connection.signal.member.empty() || connection.method.member.empty();
                    const auto requirement_exists = [&](std::string_view name) noexcept {
                        return std::ranges::any_of(registration.requirements, [name](const auto& value) noexcept {
                            return value.name == name;
                        });
                    };
                    const bool invalid_requirement =
                        (connection.signal.owner == ESceneConnectionOwner::REQUIREMENT &&
                         !requirement_exists(connection.signal.requirement)) ||
                        (connection.method.owner == ESceneConnectionOwner::REQUIREMENT &&
                         !requirement_exists(connection.method.requirement));
                    if (invalid_member || invalid_requirement)
                    {
                        return lux::cxx::unexpected(failure(ESceneMetaError::INVALID_CONNECTION, registration.type.hash)
                        );
                    }
                    if (connection.signal.owner == ESceneConnectionOwner::SELF &&
                        !object::reflection::findSignal(
                            meta::ReflectionRegistry::instance(),
                            *object_reflection,
                            connection.signal.member))
                    {
                        return lux::cxx::unexpected(failure(ESceneMetaError::INVALID_CONNECTION, registration.type.hash)
                        );
                    }
                    if (connection.method.owner == ESceneConnectionOwner::SELF)
                    {
                        const bool method_found =
                            std::ranges::any_of(object_reflection->methods, [&](const auto& method) noexcept {
                                return method.invokable.name == connection.method.member;
                            });
                        if (!method_found)
                        {
                            return lux::cxx::unexpected(
                                failure(ESceneMetaError::INVALID_CONNECTION, registration.type.hash)
                            );
                        }
                    }
                }

                const bool has_configuration = !registration.description->configuration_schema_name.empty();
                if (has_configuration != registration.configuration.valid())
                {
                    return lux::cxx::unexpected(failure(ESceneMetaError::INVALID_SCENE_SYSTEM, registration.type.hash));
                }
                const auto* reflection = configurationReflection(registration.configuration);
                if (has_configuration && reflection == nullptr)
                {
                    return lux::cxx::unexpected(failure(
                        ESceneMetaError::CONFIGURATION_REFLECTION_NOT_FOUND,
                        registration.type.hash
                    ));
                }
                impl->system_default_configuration_storage.emplace_back();
                auto& defaults = impl->system_default_configuration_storage.back();
                if (has_configuration)
                {
                    auto encoded = registration.configuration.encode_default(defaults);
                    if (!encoded)
                    {
                        return lux::cxx::unexpected(failure(
                            ESceneMetaError::CONFIGURATION_DEFAULT_ENCODING_FAILURE,
                            registration.type.hash,
                            encoded.error()
                        ));
                    }
                }
                impl->scene_system_by_hash.emplace(registration.type.hash, index);
                impl->all_systems.push_back({
                    ESystemDomain::SCENE,
                    registration.type,
                    registration.cpp_type,
                    registration.description,
                    registration.configuration,
                    reflection,
                    defaults
                });
            }

            impl->render_feature_default_configuration_storage.reserve(
                impl->render_feature_registrations.size()
            );
            impl->render_features.reserve(impl->render_feature_registrations.size());
            impl->render_feature_by_hash.reserve(impl->render_feature_registrations.size());
            std::vector<std::string_view> render_display_names;
            render_display_names.reserve(impl->render_feature_registrations.size());
            for (std::size_t index{}; index < impl->render_feature_registrations.size(); ++index)
            {
                const auto& registration = impl->render_feature_registrations[index];
                const bool invalid_identity = registration.stable_name.empty() || registration.descriptor == nullptr ||
                    !registration.descriptor->valid() || registration.descriptor->name.empty() ||
                    render::featureId(registration.stable_name) != registration.descriptor->type ||
                    !registration.configuration.valid();
                if (invalid_identity)
                {
                    return lux::cxx::unexpected(failure(
                        ESceneMetaError::INVALID_RENDER_FEATURE,
                        registration.descriptor != nullptr ? registration.descriptor->type : 0U
                    ));
                }
                const auto existing = impl->render_feature_by_hash.find(registration.descriptor->type);
                if (existing != impl->render_feature_by_hash.end())
                {
                    const auto& previous = impl->render_feature_registrations[existing->second];
                    const auto code = previous.stable_name == registration.stable_name
                        ? ESceneMetaError::DUPLICATE_RENDER_FEATURE
                        : ESceneMetaError::RENDER_FEATURE_TYPE_COLLISION;
                    return lux::cxx::unexpected(failure(code, registration.descriptor->type));
                }
                if (std::ranges::find(render_display_names, registration.descriptor->name) !=
                    render_display_names.end())
                {
                    return lux::cxx::unexpected(failure(
                        ESceneMetaError::DUPLICATE_RENDER_FEATURE,
                        registration.descriptor->type
                    ));
                }
                const auto* reflection = configurationReflection(registration.configuration.portable);
                if (reflection == nullptr)
                {
                    return lux::cxx::unexpected(failure(
                        ESceneMetaError::CONFIGURATION_REFLECTION_NOT_FOUND,
                        registration.descriptor->type
                    ));
                }
                impl->render_feature_default_configuration_storage.emplace_back();
                auto& defaults = impl->render_feature_default_configuration_storage.back();
                auto encoded = registration.configuration.portable.encode_default(defaults);
                if (!encoded)
                {
                    return lux::cxx::unexpected(failure(
                        ESceneMetaError::CONFIGURATION_DEFAULT_ENCODING_FAILURE,
                        registration.descriptor->type,
                        encoded.error()
                    ));
                }
                std::vector<std::byte> attach;
                auto materialized = registration.configuration.materialize_attach(defaults, attach);
                if (!materialized || attach.size() != registration.configuration.attach_wire_size)
                {
                    return lux::cxx::unexpected(failure(
                        ESceneMetaError::INVALID_RENDER_FEATURE,
                        registration.descriptor->type,
                        materialized ? lux::serialization::SerializationFailure{}
                                     : materialized.error()
                    ));
                }
                impl->render_feature_by_hash.emplace(registration.descriptor->type, index);
                render_display_names.push_back(registration.descriptor->name);
                impl->render_features.push_back({
                    registration.descriptor->type,
                    registration.stable_name,
                    registration.descriptor->name,
                    &registration,
                    reflection,
                    defaults,
                    registration.scene_configurable,
                    {},
                    {}
                });
            }

            std::vector<std::uint8_t> binding_present(impl->render_features.size(), 0U);
            for (const auto& binding : info.render_scene_bindings)
            {
                const auto feature = impl->render_feature_by_hash.find(binding.feature);
                if (feature == impl->render_feature_by_hash.end())
                {
                    return lux::cxx::unexpected(failure(
                        ESceneMetaError::UNKNOWN_RENDER_FEATURE_BINDING,
                        binding.feature
                    ));
                }
                const auto scene_system = impl->scene_system_by_hash.find(binding.scene_system.hash);
                if (scene_system == impl->scene_system_by_hash.end() ||
                    impl->scene_systems[scene_system->second].type != binding.scene_system)
                {
                    return lux::cxx::unexpected(failure(
                        ESceneMetaError::UNKNOWN_SCENE_SYSTEM_BINDING,
                        binding.scene_system.hash
                    ));
                }
                const std::size_t feature_ordinal = feature->second;
                if (binding_present[feature_ordinal] != 0U)
                {
                    return lux::cxx::unexpected(failure(
                        ESceneMetaError::DUPLICATE_RENDER_FEATURE_BINDING,
                        binding.feature
                    ));
                }
                binding_present[feature_ordinal] = 1U;
                auto& meta = impl->render_features[feature_ordinal];
                for (const auto& observation : binding.observations)
                {
                    if (!observation.component.isValid())
                    {
                        return lux::cxx::unexpected(failure(
                            ESceneMetaError::INVALID_RENDER_FEATURE_BINDING,
                            binding.feature
                        ));
                    }
                    if (impl->components.find(observation.component) == nullptr)
                    {
                        return lux::cxx::unexpected(failure(
                            ESceneMetaError::UNKNOWN_COMPONENT_SCHEMA,
                            observation.component.hash()
                        ));
                    }
                }
                meta.create_sync_stage = binding.create_sync_stage;
                meta.observations = binding.observations;
            }

            std::ranges::sort(impl->all_systems, [](const auto& left, const auto& right) noexcept {
                return system::SystemTypeIdLess{}(left.type, right.type);
            });
            for (std::size_t index{}; index < impl->all_systems.size(); ++index)
            {
                const auto& system = impl->all_systems[index];
                if (index != 0U && impl->all_systems[index - 1U].type.hash == system.type.hash)
                {
                    const auto code = impl->all_systems[index - 1U].type.name == system.type.name
                        ? ESceneMetaError::DUPLICATE_SYSTEM_TYPE
                        : ESceneMetaError::SYSTEM_TYPE_COLLISION;
                    return lux::cxx::unexpected(failure(code, system.type.hash));
                }
                impl->system_by_hash.emplace(system.type.hash, index);
                for (const auto capability : system.description->capabilities)
                {
                    impl->capability_index[std::string(capability)].push_back(system.type);
                }
            }

            std::vector<std::vector<ComponentSystemUsage>> usages(impl->components.all().size());
            const auto componentOrdinal = [&](lux::cxx::TypeToken type) noexcept {
                const auto* schema = impl->components.find(type);
                return static_cast<std::size_t>(schema - impl->components.all().data());
            };
            for (const auto& registration : impl->simulation_systems.all())
            {
                for (const auto& access : registration.access.components)
                {
                    if (!access.type.isValid())
                    {
                        return lux::cxx::unexpected(failure(
                            ESceneMetaError::INVALID_SIMULATION_SYSTEM,
                            registration.type.hash
                        ));
                    }
                    if (impl->components.find(access.type) == nullptr)
                    {
                        return lux::cxx::unexpected(failure(
                            ESceneMetaError::UNKNOWN_COMPONENT_SCHEMA,
                            access.type.hash()
                        ));
                    }
                    const auto ordinal = componentOrdinal(access.type);
                    usages[ordinal].push_back({
                        registration.type,
                        ESystemDomain::SIMULATION,
                        access.mode,
                        0U
                    });
                }
            }
            for (const auto& registration : impl->scene_systems)
            {
                for (const auto& observation : registration.observations)
                {
                    if (!observation.component.isValid())
                    {
                        return lux::cxx::unexpected(
                            failure(ESceneMetaError::INVALID_SCENE_SYSTEM, registration.type.hash)
                        );
                    }
                    if (impl->components.find(observation.component) == nullptr)
                    {
                        return lux::cxx::unexpected(failure(
                            ESceneMetaError::UNKNOWN_COMPONENT_SCHEMA,
                            observation.component.hash()
                        ));
                    }
                    const auto ordinal = componentOrdinal(observation.component);
                    usages[ordinal].push_back({
                        registration.type,
                        ESystemDomain::SCENE,
                        {},
                        observation.events
                    });
                }
            }
            for (const auto& feature : impl->render_features)
            {
                if (feature.observations.empty()) continue;
                const auto binding = std::find_if(
                    info.render_scene_bindings.begin(),
                    info.render_scene_bindings.end(),
                    [&](const auto& value) noexcept { return value.feature == feature.type; }
                );
                for (const auto& observation : feature.observations)
                {
                    const auto ordinal = componentOrdinal(observation.component);
                    usages[ordinal].push_back({
                        binding->scene_system,
                        ESystemDomain::SCENE,
                        {},
                        observation.events,
                        feature.type
                    });
                }
            }
            impl->component_usage_ranges.resize(usages.size());
            for (std::size_t ordinal{}; ordinal < usages.size(); ++ordinal)
            {
                impl->component_usage_ranges[ordinal] = {impl->component_usage.size(), usages[ordinal].size()};
                impl->component_usage.insert(
                    impl->component_usage.end(),
                    usages[ordinal].begin(),
                    usages[ordinal].end()
                );
            }
            return SceneMetaManager(std::move(impl));
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(ESceneMetaError::ALLOCATION_FAILURE));
        }
    }

    const simulation::ecs::ComponentSchemaSet& SceneMetaManager::components() const noexcept
    {
        return impl_->components;
    }

    const simulation::SimulationSystemRegistry& SceneMetaManager::simulationSystems() const noexcept
    {
        return impl_->simulation_systems;
    }

    const simulation::ecs::ComponentSchema* SceneMetaManager::getComponentMeta(
        const simulation::ecs::ComponentSchemaId& id
    ) const noexcept
    {
        return impl_->components.find(id);
    }

    const simulation::ecs::ComponentSchema* SceneMetaManager::getComponentMeta(lux::cxx::TypeToken type) const noexcept
    {
        return impl_->components.find(type);
    }

    std::optional<SystemMetaView> SceneMetaManager::getSystemMeta(const system::SystemTypeId& type) const noexcept
    {
        if (!type.valid()) return std::nullopt;
        const auto found = impl_->system_by_hash.find(type.hash);
        if (found == impl_->system_by_hash.end() || impl_->all_systems[found->second].type != type)
            return std::nullopt;
        return impl_->all_systems[found->second];
    }

    std::optional<SystemMetaView> SceneMetaManager::getSystemMeta(std::string_view canonical_name) const noexcept
    {
        const auto hash = lux::cxx::Fnv1a64::hash(canonical_name);
        const auto found = impl_->system_by_hash.find(hash);
        if (found == impl_->system_by_hash.end() || impl_->all_systems[found->second].type.name != canonical_name)
            return std::nullopt;
        return impl_->all_systems[found->second];
    }

    const simulation::SimulationSystemRegistration* SceneMetaManager::getSimulationSystemMeta(
        const system::SystemTypeId& type
    ) const noexcept
    {
        return impl_->simulation_systems.find(type);
    }

    const SceneSystemRegistration* SceneMetaManager::getSceneSystemMeta(const system::SystemTypeId& type) const noexcept
    {
        const auto found = impl_->scene_system_by_hash.find(type.hash);
        return found != impl_->scene_system_by_hash.end() && impl_->scene_systems[found->second].type == type
            ? &impl_->scene_systems[found->second]
            : nullptr;
    }

    const RenderFeatureMeta* SceneMetaManager::getRenderFeatureMeta(render::FeatureTypeId type) const noexcept
    {
        const auto found = impl_->render_feature_by_hash.find(type);
        return found != impl_->render_feature_by_hash.end() ? &impl_->render_features[found->second] : nullptr;
    }

    const RenderFeatureMeta* SceneMetaManager::getRenderFeatureMeta(std::string_view stable_name) const noexcept
    {
        const auto type = render::featureId(stable_name);
        const auto* meta = getRenderFeatureMeta(type);
        return meta != nullptr && meta->stable_name == stable_name ? meta : nullptr;
    }

    std::span<const ComponentSystemUsage> SceneMetaManager::systemsUsingComponent(
        const simulation::ecs::ComponentSchemaId& component
    ) const noexcept
    {
        const auto* schema = impl_->components.find(component);
        if (schema == nullptr) return {};
        const auto ordinal = static_cast<std::size_t>(schema - impl_->components.all().data());
        const auto range = impl_->component_usage_ranges[ordinal];
        return std::span<const ComponentSystemUsage>(impl_->component_usage).subspan(range.offset, range.count);
    }

    std::span<const system::SystemTypeId> SceneMetaManager::systemsProvidingCapability(
        std::string_view capability
    ) const noexcept
    {
        const auto found = impl_->capability_index.find(capability);
        return found != impl_->capability_index.end()
            ? std::span<const system::SystemTypeId>(found->second)
            : std::span<const system::SystemTypeId>{};
    }

    std::span<const SystemMetaView> SceneMetaManager::allSystems() const noexcept
    {
        return impl_->all_systems;
    }

    std::span<const SceneSystemRegistration> SceneMetaManager::sceneSystems() const noexcept
    {
        return impl_->scene_systems;
    }

    std::span<const RenderFeatureMeta> SceneMetaManager::allRenderFeatures() const noexcept
    {
        return impl_->render_features;
    }
} // namespace lux::scene
