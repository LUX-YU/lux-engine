#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/detail/SimulationDescriptionFailureInjection.hpp>

#include <algorithm>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lux::simulation
{
    struct SimulationDescriptionBuilder::Impl final
    {
        struct PendingData final
        {
            SimulationDataSchemaId schema;
            std::uint32_t version{};
            std::vector<std::byte> payload;
        };

        struct PendingSemanticType final
        {
            std::uint64_t type_id{};
            std::string canonical_name;
            lux::semantic::EValuePass pass{
                lux::semantic::EValuePass::VALUE};

            friend bool operator==(
                const PendingSemanticType&,
                const PendingSemanticType&
            ) noexcept = default;
        };

        struct PendingHook final
        {
            HookPointId id;
            std::string name;
            std::vector<PendingSemanticType> parameters;

            friend bool operator==(const PendingHook&, const PendingHook&) noexcept =
                default;
        };

        struct PendingEvent final
        {
            EventPointId id;
            std::string name;
            std::size_t dispatch_hook_ordinal{};
            EEventRoute route{EEventRoute::SIMULATION_BROADCAST};
            lux::semantic::TypeId payload_type{};
            std::string payload_schema_name;
            std::uint64_t payload_schema_hash{};
            std::uint32_t payload_schema_version{};
        };

        struct PendingSystem final
        {
            lux::system::SystemInstanceId instance_id;
            std::string instance_name;
            lux::system::SystemTypeId type;
            std::uint32_t version{};
            lux::system::ESystemMultiplicity multiplicity{lux::system::ESystemMultiplicity::MULTIPLE};
            std::string configuration_schema_name;
            std::uint64_t configuration_schema_hash{};
            std::uint32_t configuration_schema_version{};
            std::vector<std::string> capabilities;
            std::vector<PendingHook> hooks;
            std::vector<PendingEvent> events;
            std::vector<std::byte> configuration;
        };

        struct PendingDependency final
        {
            lux::system::SystemInstanceId before_system;
            lux::system::SystemInstanceId after_system;

            friend bool operator==(const PendingDependency&, const PendingDependency&)
                noexcept = default;
        };

        std::vector<PendingData> data;
        std::vector<PendingSystem> systems;
        std::vector<PendingDependency> dependencies;
    };

    namespace
    {
        [[nodiscard]] SimulationDescriptionFailure failure(
            ESimulationDescriptionError code,
            SimulationDataSchemaId schema = {},
            std::uint64_t subject_hash = 0U
        ) noexcept
        {
            return {code, std::move(schema), subject_hash};
        }

        template <class Range>
        [[nodiscard]] auto findData(
            Range& range,
            const SimulationDataSchemaId& schema
        ) noexcept
        {
            return std::find_if(
                range.begin(),
                range.end(),
                [&](const auto& candidate) noexcept
                {
                    return candidate.schema == schema;
                }
            );
        }

        template <class Range>
        [[nodiscard]] auto findSystem(
            Range& range,
            lux::system::SystemInstanceId id
        ) noexcept
        {
            return std::find_if(
                range.begin(),
                range.end(),
                [id](const auto& system) noexcept
                {
                    return system.instance_id == id;
                }
            );
        }

        [[nodiscard]] bool failMutationForTest() noexcept
        {
            return detail::consumeSimulationDescriptionFailureForTest(
                detail::ESimulationDescriptionFailurePoint::MUTATION_ALLOCATION
            );
        }

        [[nodiscard]] bool uniqueNames(
            std::span<const std::string_view> names
        ) noexcept
        {
            for (std::size_t index{}; index < names.size(); ++index)
            {
                for (std::size_t previous{}; previous < index; ++previous)
                {
                    if (names[index] == names[previous])
                        return false;
                }
            }
            return true;
        }

        template <class PendingSystem>
        [[nodiscard]] bool sameTypeDeclaration(
            const PendingSystem& left,
            const PendingSystem& right
        ) noexcept
        {
            if (left.type != right.type || left.version != right.version || left.multiplicity != right.multiplicity ||
                left.configuration_schema_name != right.configuration_schema_name ||
                left.configuration_schema_hash != right.configuration_schema_hash ||
                left.configuration_schema_version !=
                    right.configuration_schema_version ||
                left.capabilities != right.capabilities ||
                left.hooks != right.hooks ||
                left.events.size() != right.events.size())
            {
                return false;
            }
            for (std::size_t index{}; index < left.events.size(); ++index)
            {
                const auto& a = left.events[index];
                const auto& b = right.events[index];
                if (a.id != b.id || a.name != b.name ||
                    a.dispatch_hook_ordinal != b.dispatch_hook_ordinal ||
                    a.route != b.route || a.payload_type != b.payload_type ||
                    a.payload_schema_name != b.payload_schema_name ||
                    a.payload_schema_hash != b.payload_schema_hash ||
                    a.payload_schema_version != b.payload_schema_version)
                {
                    return false;
                }
            }
            return true;
        }

        template <class ImplType>
        [[nodiscard]] bool hasDependencyCycle(const ImplType& impl)
        {
            std::vector<std::size_t> indegree(impl.systems.size(), 0U);
            for (const auto& dependency : impl.dependencies)
            {
                const auto after = findSystem(impl.systems, dependency.after_system);
                if (after == impl.systems.end())
                    return true;
                ++indegree[static_cast<std::size_t>(
                    std::distance(impl.systems.begin(), after))];
            }

            std::vector<bool> removed(impl.systems.size(), false);
            std::size_t removed_count{};
            bool progress{true};
            while (progress)
            {
                progress = false;
                for (std::size_t index{}; index < impl.systems.size(); ++index)
                {
                    if (removed[index] || indegree[index] != 0U)
                        continue;
                    removed[index] = true;
                    ++removed_count;
                    progress = true;
                    const auto before_id = impl.systems[index].instance_id;
                    for (const auto& dependency : impl.dependencies)
                    {
                        if (dependency.before_system != before_id)
                            continue;
                        const auto after = findSystem(
                            impl.systems,
                            dependency.after_system
                        );
                        const auto after_index = static_cast<std::size_t>(
                            std::distance(impl.systems.begin(), after)
                        );
                        if (!removed[after_index])
                            --indegree[after_index];
                    }
                }
            }
            return removed_count != impl.systems.size();
        }
    }

    SimulationDescriptionBuilder::SimulationDescriptionBuilder()
        : impl_(std::make_unique<Impl>())
    {
    }

    SimulationDescriptionBuilder::~SimulationDescriptionBuilder() = default;
    SimulationDescriptionBuilder::SimulationDescriptionBuilder(
        SimulationDescriptionBuilder&&
    ) noexcept = default;
    SimulationDescriptionBuilder& SimulationDescriptionBuilder::operator=(
        SimulationDescriptionBuilder&&
    ) noexcept = default;

    lux::cxx::expected<void, SimulationDescriptionFailure>
    SimulationDescriptionBuilder::addData(
        SimulationDataSchemaId schema,
        std::uint32_t version,
        std::span<const std::byte> payload
    ) noexcept
    {
        if (!schema.valid())
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::INVALID_SCHEMA_ID,
                std::move(schema)
            ));
        if (version == 0U)
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::INVALID_SCHEMA_VERSION,
                std::move(schema)
            ));
        if (findData(impl_->data, schema) != impl_->data.end())
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::DUPLICATE_DATA,
                std::move(schema)
            ));
        if (failMutationForTest())
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::ALLOCATION_FAILURE,
                std::move(schema)
            ));
        try
        {
            impl_->data.push_back({
                std::move(schema),
                version,
                std::vector<std::byte>(payload.begin(), payload.end())
            });
            return {};
        }
        catch (const std::length_error&)
        {
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::SIZE_OVERFLOW,
                std::move(schema)
            ));
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::ALLOCATION_FAILURE,
                std::move(schema)
            ));
        }
    }

    lux::cxx::expected<void, SimulationDescriptionFailure>
    SimulationDescriptionBuilder::setData(
        SimulationDataSchemaId schema,
        std::uint32_t version,
        std::span<const std::byte> payload
    ) noexcept
    {
        if (!schema.valid())
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::INVALID_SCHEMA_ID,
                std::move(schema)
            ));
        if (version == 0U)
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::INVALID_SCHEMA_VERSION,
                std::move(schema)
            ));
        if (failMutationForTest())
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::ALLOCATION_FAILURE,
                std::move(schema)
            ));
        auto data = findData(impl_->data, schema);
        try
        {
            std::vector<std::byte> replacement(payload.begin(), payload.end());
            if (data == impl_->data.end())
                impl_->data.push_back({std::move(schema), version, std::move(replacement)});
            else
            {
                data->version = version;
                data->payload = std::move(replacement);
            }
            return {};
        }
        catch (const std::length_error&)
        {
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::SIZE_OVERFLOW,
                std::move(schema)
            ));
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::ALLOCATION_FAILURE,
                std::move(schema)
            ));
        }
    }

    lux::cxx::expected<void, SimulationDescriptionFailure>
    SimulationDescriptionBuilder::eraseData(
        const SimulationDataSchemaId& schema
    ) noexcept
    {
        auto data = findData(impl_->data, schema);
        if (data == impl_->data.end())
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::DATA_NOT_FOUND,
                schema
            ));
        impl_->data.erase(data);
        return {};
    }

    lux::cxx::expected<void, SimulationDescriptionFailure>
    SimulationDescriptionBuilder::addSystem(
        lux::system::SystemInstanceId instance_id,
        std::string_view instance_name,
        const SimulationSystemDescription& system,
        std::span<const std::byte> configuration
    ) noexcept
    {
        if (!instance_id.valid() || instance_name.empty())
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::INVALID_SYSTEM_INSTANCE_NAME
            ));
        if (system.type.canonical_name.empty())
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::INVALID_SYSTEM_TYPE
            ));
        if (system.type.version == 0U)
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::INVALID_SYSTEM_VERSION
            ));
        if (system.type.multiplicity != lux::system::ESystemMultiplicity::MULTIPLE &&
            system.type.multiplicity != lux::system::ESystemMultiplicity::SINGLE_PER_OWNER)
        {
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::INVALID_SYSTEM_TYPE
            ));
        }
        if (findSystem(impl_->systems, instance_id) != impl_->systems.end())
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::DUPLICATE_SYSTEM_INSTANCE
            ));
        if (system.type.configuration_schema_name.empty() !=
            (system.type.configuration_schema_version == 0U) ||
            (system.type.configuration_schema_name.empty() && !configuration.empty()))
        {
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::INVALID_CONFIGURATION_SCHEMA
            ));
        }
        for (const auto capability : system.type.capabilities)
        {
            if (capability.empty())
                return lux::cxx::unexpected(failure(
                    ESimulationDescriptionError::INVALID_CAPABILITY
                ));
        }
        if (!uniqueNames(system.type.capabilities))
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::DUPLICATE_CAPABILITY
            ));

        for (const auto& hook : system.hooks)
            if (!validHookPointSpec(hook))
                return lux::cxx::unexpected(failure(
                    ESimulationDescriptionError::INVALID_HOOK_POINT));
        for (std::size_t index{}; index < system.hooks.size(); ++index)
        {
            for (std::size_t previous{}; previous < index; ++previous)
            {
                if (system.hooks[index].id == system.hooks[previous].id)
                {
                    return lux::cxx::unexpected(failure(
                        ESimulationDescriptionError::DUPLICATE_HOOK_POINT
                    ));
                }
            }
        }

        for (std::size_t index{}; index < system.events.size(); ++index)
        {
            const auto& event = system.events[index];
            if (!validEventPointSpec(event))
                return lux::cxx::unexpected(failure(
                    ESimulationDescriptionError::INVALID_EVENT
                ));
            for (std::size_t previous{}; previous < index; ++previous)
            {
                if (event.id == system.events[previous].id)
                    return lux::cxx::unexpected(failure(
                        ESimulationDescriptionError::DUPLICATE_EVENT
                    ));
            }
            const auto hook = std::find_if(
                system.hooks.begin(),
                system.hooks.end(),
                [&](const auto& candidate) noexcept
                {
                    return candidate.id == event.dispatch_hook;
                }
            );
            if (hook == system.hooks.end())
                return lux::cxx::unexpected(failure(
                    ESimulationDescriptionError::INVALID_EVENT_DISPATCH_HOOK
                ));
        }
        if (failMutationForTest())
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::ALLOCATION_FAILURE
            ));

        try
        {
            Impl::PendingSystem candidate;
            candidate.instance_id = instance_id;
            candidate.instance_name = instance_name;
            candidate.type = lux::system::systemTypeId(system.type.canonical_name);
            candidate.version = system.type.version;
            candidate.multiplicity = system.type.multiplicity;
            candidate.configuration_schema_name =
                system.type.configuration_schema_name;
            candidate.configuration_schema_hash =
                system.type.configuration_schema_name.empty()
                ? 0U
                : lux::cxx::Fnv1a64::hash(system.type.configuration_schema_name);
            candidate.configuration_schema_version =
                system.type.configuration_schema_version;
            candidate.configuration.assign(configuration.begin(), configuration.end());
            candidate.capabilities.reserve(system.type.capabilities.size());
            for (const auto value : system.type.capabilities)
                candidate.capabilities.emplace_back(value);
            candidate.hooks.reserve(system.hooks.size());
            for (const auto& value : system.hooks)
            {
                Impl::PendingHook hook;
                hook.id = value.id;
                hook.name = value.diagnostic_name;
                hook.parameters.reserve(value.signature.parameters.size());
                for (const auto& parameter : value.signature.parameters)
                {
                    hook.parameters.push_back({
                        parameter.type_id,
                        std::string(parameter.canonical_name),
                        parameter.pass
                    });
                }
                candidate.hooks.push_back(std::move(hook));
            }
            candidate.events.reserve(system.events.size());
            for (const auto& event : system.events)
            {
                const auto hook = std::find_if(
                    candidate.hooks.begin(),
                    candidate.hooks.end(),
                    [&](const auto& value) noexcept
                    {
                        return value.id == event.dispatch_hook;
                    }
                );
                candidate.events.push_back({
                    event.id,
                    std::string(event.diagnostic_name),
                    static_cast<std::size_t>(
                        std::distance(candidate.hooks.begin(), hook)),
                    event.route,
                    event.payload_type,
                    std::string(event.payload_schema_name),
                    lux::semantic::typeId(event.payload_schema_name),
                    event.payload_schema_version
                });
            }

            for (const auto& existing : impl_->systems)
            {
                if (existing.type == candidate.type &&
                    (existing.multiplicity == lux::system::ESystemMultiplicity::SINGLE_PER_OWNER ||
                     candidate.multiplicity == lux::system::ESystemMultiplicity::SINGLE_PER_OWNER))
                {
                    return lux::cxx::unexpected(failure(
                        ESimulationDescriptionError::SYSTEM_MULTIPLICITY_VIOLATION,
                        {},
                        candidate.type.hash
                    ));
                }
                if (existing.type.hash == candidate.type.hash &&
                    existing.type.name != candidate.type.name)
                {
                    return lux::cxx::unexpected(failure(
                        ESimulationDescriptionError::SYSTEM_TYPE_HASH_COLLISION,
                        {},
                        candidate.type.hash
                    ));
                }
                if (existing.type == candidate.type &&
                    !sameTypeDeclaration(existing, candidate))
                {
                    return lux::cxx::unexpected(failure(
                        ESimulationDescriptionError::INVALID_SYSTEM_TYPE,
                        {},
                        candidate.type.hash
                    ));
                }
            }
            impl_->systems.push_back(std::move(candidate));
            return {};
        }
        catch (const std::length_error&)
        {
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::SIZE_OVERFLOW
            ));
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::ALLOCATION_FAILURE
            ));
        }
    }

    lux::cxx::expected<void, SimulationDescriptionFailure>
    SimulationDescriptionBuilder::eraseSystem(
        lux::system::SystemInstanceId instance_id
    ) noexcept
    {
        const auto system = findSystem(impl_->systems, instance_id);
        if (system == impl_->systems.end())
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::SYSTEM_NOT_FOUND
            ));
        impl_->dependencies.erase(
            std::remove_if(
                impl_->dependencies.begin(),
                impl_->dependencies.end(),
                [instance_id](const auto& dependency) noexcept
                {
                    return dependency.before_system == instance_id ||
                        dependency.after_system == instance_id;
                }
            ),
            impl_->dependencies.end()
        );
        impl_->systems.erase(system);
        return {};
    }

    lux::cxx::expected<void, SimulationDescriptionFailure>
    SimulationDescriptionBuilder::setSystemConfiguration(
        lux::system::SystemInstanceId instance_id,
        std::span<const std::byte> configuration
    ) noexcept
    {
        const auto system = findSystem(impl_->systems, instance_id);
        if (system == impl_->systems.end())
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::SYSTEM_NOT_FOUND
            ));
        if (system->configuration_schema_name.empty() && !configuration.empty())
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::INVALID_CONFIGURATION_SCHEMA
            ));
        try
        {
            std::vector<std::byte> replacement(
                configuration.begin(),
                configuration.end()
            );
            system->configuration = std::move(replacement);
            return {};
        }
        catch (const std::length_error&)
        {
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::SIZE_OVERFLOW
            ));
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::ALLOCATION_FAILURE
            ));
        }
    }

    lux::cxx::expected<void, SimulationDescriptionFailure>
    SimulationDescriptionBuilder::addDependency(
        lux::system::SystemInstanceId before_system,
        lux::system::SystemInstanceId after_system
    ) noexcept
    {
        const auto before = findSystem(impl_->systems, before_system);
        const auto after = findSystem(impl_->systems, after_system);
        if (before == impl_->systems.end() || after == impl_->systems.end())
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::SYSTEM_NOT_FOUND
            ));
        if (before_system == after_system)
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::INVALID_DEPENDENCY
            ));
        try
        {
            Impl::PendingDependency candidate{before_system, after_system};
            if (std::find(
                    impl_->dependencies.begin(),
                    impl_->dependencies.end(),
                    candidate
                ) != impl_->dependencies.end())
            {
                return lux::cxx::unexpected(failure(
                    ESimulationDescriptionError::DUPLICATE_DEPENDENCY
                ));
            }
            impl_->dependencies.push_back(std::move(candidate));
            if (hasDependencyCycle(*impl_))
            {
                impl_->dependencies.pop_back();
                return lux::cxx::unexpected(failure(
                    ESimulationDescriptionError::DEPENDENCY_CYCLE
                ));
            }
            return {};
        }
        catch (const std::length_error&)
        {
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::SIZE_OVERFLOW
            ));
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::ALLOCATION_FAILURE
            ));
        }
    }

    lux::cxx::expected<void, SimulationDescriptionFailure>
    SimulationDescriptionBuilder::eraseDependency(
        lux::system::SystemInstanceId before_system,
        lux::system::SystemInstanceId after_system
    ) noexcept
    {
        const auto dependency = std::find_if(
            impl_->dependencies.begin(),
            impl_->dependencies.end(),
            [&](const auto& value) noexcept
            {
                return value.before_system == before_system &&
                    value.after_system == after_system;
            }
        );
        if (dependency == impl_->dependencies.end())
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::INVALID_DEPENDENCY
            ));
        impl_->dependencies.erase(dependency);
        return {};
    }

    void SimulationDescriptionBuilder::clear() noexcept
    {
        impl_->data.clear();
        impl_->systems.clear();
        impl_->dependencies.clear();
    }

    lux::cxx::expected<SimulationDescription, SimulationDescriptionFailure>
    SimulationDescriptionBuilder::build() && noexcept
    {
        if (detail::consumeSimulationDescriptionFailureForTest(
                detail::ESimulationDescriptionFailurePoint::BUILD_ALLOCATION))
        {
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::ALLOCATION_FAILURE
            ));
        }
        if (detail::consumeSimulationDescriptionFailureForTest(
                detail::ESimulationDescriptionFailurePoint::BUILD_SIZE_OVERFLOW))
        {
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::SIZE_OVERFLOW
            ));
        }
        try
        {
            std::sort(
                impl_->data.begin(),
                impl_->data.end(),
                [](const auto& left, const auto& right) noexcept
                {
                    return SimulationDataSchemaIdLess{}(left.schema, right.schema);
                }
            );
            std::sort(
                impl_->systems.begin(),
                impl_->systems.end(),
                [](const auto& left, const auto& right) noexcept
                {
                    return left.instance_id < right.instance_id;
                }
            );
            std::sort(
                impl_->dependencies.begin(),
                impl_->dependencies.end(),
                [](const auto& left, const auto& right) noexcept
                {
                    if (left.before_system != right.before_system)
                        return left.before_system < right.before_system;
                    return left.after_system < right.after_system;
                }
            );

            SimulationDescription result;
            std::size_t total_payload{};
            for (std::size_t index{}; index < impl_->data.size(); ++index)
            {
                const auto& current = impl_->data[index];
                if (index != 0U)
                {
                    const auto& previous = impl_->data[index - 1U];
                    if (previous.schema.hash == current.schema.hash &&
                        previous.schema.name != current.schema.name)
                    {
                        return lux::cxx::unexpected(failure(
                            ESimulationDescriptionError::SCHEMA_HASH_COLLISION,
                            current.schema
                        ));
                    }
                }
                if (current.payload.size() >
                    std::numeric_limits<std::size_t>::max() - total_payload)
                {
                    return lux::cxx::unexpected(failure(
                        ESimulationDescriptionError::SIZE_OVERFLOW,
                        current.schema
                    ));
                }
                total_payload += current.payload.size();
            }
            result.schemas_.reserve(impl_->data.size());
            result.data_.reserve(impl_->data.size());
            result.payload_.reserve(total_payload);
            for (auto& source : impl_->data)
            {
                const SimulationDescription::DataRecord record{
                    result.schemas_.size(),
                    source.version,
                    result.payload_.size(),
                    source.payload.size()};
                result.schemas_.push_back(std::move(source.schema));
                result.data_.push_back(record);
                result.payload_.insert(
                    result.payload_.end(),
                    source.payload.begin(),
                    source.payload.end()
                );
            }

            std::vector<std::size_t> type_sources;
            type_sources.reserve(impl_->systems.size());
            for (std::size_t index{}; index < impl_->systems.size(); ++index)
            {
                const auto& system = impl_->systems[index];
                const auto existing = std::find_if(
                    type_sources.begin(),
                    type_sources.end(),
                    [&](std::size_t source) noexcept
                    {
                        return impl_->systems[source].type == system.type;
                    }
                );
                if (existing == type_sources.end())
                    type_sources.push_back(index);
            }
            std::sort(
                type_sources.begin(),
                type_sources.end(),
                [&](std::size_t left, std::size_t right) noexcept
                {
                    return lux::system::SystemTypeIdLess{}(
                        impl_->systems[left].type,
                        impl_->systems[right].type
                    );
                }
            );
            result.system_types_.reserve(type_sources.size());
            for (const auto source_index : type_sources)
            {
                auto& source = impl_->systems[source_index];
                SimulationDescription::SystemTypeRecord record;
                record.type = source.type;
                record.version = source.version;
                record.multiplicity = source.multiplicity;
                record.configuration_schema_name = source.configuration_schema_name;
                record.configuration_schema_hash = source.configuration_schema_hash;
                record.configuration_schema_version =
                    source.configuration_schema_version;
                record.capabilities = source.capabilities;
                record.hooks.reserve(source.hooks.size());
                record.hook_ordinals.reserve(source.hooks.size());
                for (const auto& hook : source.hooks)
                {
                    SimulationDescription::HookRecord hook_record;
                    hook_record.id = hook.id;
                    hook_record.name = hook.name;
                    hook_record.parameters.reserve(hook.parameters.size());
                    for (const auto& parameter : hook.parameters)
                    {
                        hook_record.parameters.push_back({
                            parameter.type_id,
                            parameter.canonical_name,
                            parameter.pass
                        });
                    }
                    record.hook_ordinals.emplace(hook.id.value, record.hooks.size());
                    record.hooks.push_back(std::move(hook_record));
                }
                record.events.reserve(source.events.size());
                record.event_ordinals.reserve(source.events.size());
                for (const auto& event : source.events)
                {
                    record.event_ordinals.emplace(event.id.value, record.events.size());
                    record.events.push_back({
                        event.id,
                        event.name,
                        event.dispatch_hook_ordinal,
                        event.route,
                        event.payload_type,
                        event.payload_schema_name,
                        event.payload_schema_hash,
                        event.payload_schema_version
                    });
                }
                result.system_types_.push_back(std::move(record));
            }

            std::size_t total_configuration{};
            for (const auto& source : impl_->systems)
            {
                if (source.configuration.size() >
                    std::numeric_limits<std::size_t>::max() - total_configuration)
                {
                    return lux::cxx::unexpected(failure(
                        ESimulationDescriptionError::SIZE_OVERFLOW
                    ));
                }
                total_configuration += source.configuration.size();
            }
            result.systems_.reserve(impl_->systems.size());
            result.system_ordinals_.reserve(impl_->systems.size());
            result.configuration_payload_.reserve(total_configuration);
            for (auto& source : impl_->systems)
            {
                const auto type = std::lower_bound(
                    result.system_types_.begin(),
                    result.system_types_.end(),
                    source.type,
                    [](const auto& candidate, const lux::system::SystemTypeId& id) noexcept
                    {
                        return lux::system::SystemTypeIdLess{}(candidate.type, id);
                    }
                );
                result.system_ordinals_.emplace(source.instance_id.value, result.systems_.size());
                result.systems_.push_back({
                    source.instance_id,
                    std::move(source.instance_name),
                    static_cast<std::size_t>(
                        std::distance(result.system_types_.begin(), type)),
                    result.configuration_payload_.size(),
                    source.configuration.size()
                });
                result.configuration_payload_.insert(
                    result.configuration_payload_.end(),
                    source.configuration.begin(),
                    source.configuration.end()
                );
            }

            result.dependencies_.reserve(impl_->dependencies.size());
            for (const auto& dependency : impl_->dependencies)
            {
                const auto before_system = std::lower_bound(
                    result.systems_.begin(),
                    result.systems_.end(),
                    dependency.before_system,
                    [](const auto& system, lux::system::SystemInstanceId id) noexcept
                    {
                        return system.id < id;
                    }
                );
                const auto after_system = std::lower_bound(
                    result.systems_.begin(),
                    result.systems_.end(),
                    dependency.after_system,
                    [](const auto& system, lux::system::SystemInstanceId id) noexcept
                    {
                        return system.id < id;
                    }
                );
                const auto before_index = static_cast<std::size_t>(
                    std::distance(result.systems_.begin(), before_system));
                const auto after_index = static_cast<std::size_t>(
                    std::distance(result.systems_.begin(), after_system));
                result.dependencies_.push_back({
                    before_index,
                    after_index
                });
            }
            impl_->data.clear();
            impl_->systems.clear();
            impl_->dependencies.clear();
            return result;
        }
        catch (const std::length_error&)
        {
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::SIZE_OVERFLOW
            ));
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::ALLOCATION_FAILURE
            ));
        }
        catch (...)
        {
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::INVALID_SYSTEM_TYPE
            ));
        }
    }
}
