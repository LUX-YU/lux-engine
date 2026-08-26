#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/detail/SimulationDescriptionFailureInjection.hpp>

#include <algorithm>
#include <limits>
#include <new>
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

        struct PendingEvent final
        {
            std::string name;
            std::size_t dispatch_point_ordinal{};
            std::string payload_schema_name;
            std::uint64_t payload_schema_hash{};
            std::uint32_t payload_schema_version{};
        };

        struct PendingSystem final
        {
            std::string instance_name;
            SystemTypeId type;
            std::uint32_t version{};
            std::string configuration_schema_name;
            std::uint64_t configuration_schema_hash{};
            std::uint32_t configuration_schema_version{};
            std::vector<std::string> capabilities;
            std::vector<std::string> execution_points;
            std::vector<PendingEvent> events;
            std::vector<std::byte> configuration;
        };

        struct PendingDependency final
        {
            std::string before_system;
            std::string before_point;
            std::string after_system;
            std::string after_point;

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
        [[nodiscard]] auto findSystem(Range& range, std::string_view name) noexcept
        {
            return std::find_if(
                range.begin(),
                range.end(),
                [name](const auto& system) noexcept
                {
                    return system.instance_name == name;
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
            if (left.type != right.type || left.version != right.version ||
                left.configuration_schema_name != right.configuration_schema_name ||
                left.configuration_schema_hash != right.configuration_schema_hash ||
                left.configuration_schema_version !=
                    right.configuration_schema_version ||
                left.capabilities != right.capabilities ||
                left.execution_points != right.execution_points ||
                left.events.size() != right.events.size())
            {
                return false;
            }
            for (std::size_t index{}; index < left.events.size(); ++index)
            {
                const auto& a = left.events[index];
                const auto& b = right.events[index];
                if (a.name != b.name ||
                    a.dispatch_point_ordinal != b.dispatch_point_ordinal ||
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
                    const auto& before_name = impl.systems[index].instance_name;
                    for (const auto& dependency : impl.dependencies)
                    {
                        if (dependency.before_system != before_name)
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
        std::string_view instance_name,
        const SystemDescription& system,
        std::span<const std::byte> configuration
    ) noexcept
    {
        if (instance_name.empty())
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::INVALID_SYSTEM_INSTANCE_NAME
            ));
        if (system.canonical_name.empty())
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::INVALID_SYSTEM_TYPE
            ));
        if (system.version == 0U)
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::INVALID_SYSTEM_VERSION
            ));
        if (findSystem(impl_->systems, instance_name) != impl_->systems.end())
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::DUPLICATE_SYSTEM_INSTANCE
            ));
        if (system.configuration_schema_name.empty() !=
            (system.configuration_schema_version == 0U) ||
            (system.configuration_schema_name.empty() && !configuration.empty()))
        {
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::INVALID_CONFIGURATION_SCHEMA
            ));
        }
        for (const auto capability : system.capabilities)
        {
            if (capability.empty())
                return lux::cxx::unexpected(failure(
                    ESimulationDescriptionError::INVALID_CAPABILITY
                ));
        }
        if (!uniqueNames(system.capabilities))
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::DUPLICATE_CAPABILITY
            ));

        for (const auto point : system.execution_points)
        {
            if (point.name.empty())
                return lux::cxx::unexpected(failure(
                    ESimulationDescriptionError::INVALID_EXECUTION_POINT
                ));
        }
        for (std::size_t index{}; index < system.execution_points.size(); ++index)
        {
            for (std::size_t previous{}; previous < index; ++previous)
            {
                if (system.execution_points[index].name ==
                    system.execution_points[previous].name)
                {
                    return lux::cxx::unexpected(failure(
                        ESimulationDescriptionError::DUPLICATE_EXECUTION_POINT
                    ));
                }
            }
        }

        for (std::size_t index{}; index < system.events.size(); ++index)
        {
            const auto& event = system.events[index];
            if (event.name.empty())
                return lux::cxx::unexpected(failure(
                    ESimulationDescriptionError::INVALID_EVENT
                ));
            for (std::size_t previous{}; previous < index; ++previous)
            {
                if (event.name == system.events[previous].name)
                    return lux::cxx::unexpected(failure(
                        ESimulationDescriptionError::DUPLICATE_EVENT
                    ));
            }
            const auto point = std::find_if(
                system.execution_points.begin(),
                system.execution_points.end(),
                [&](const auto& candidate) noexcept
                {
                    return candidate.name == event.dispatch_point;
                }
            );
            if (point == system.execution_points.end())
                return lux::cxx::unexpected(failure(
                    ESimulationDescriptionError::INVALID_EVENT_DISPATCH_POINT
                ));
            const bool void_payload =
                event.payload_cpp_type == lux::cxx::typeToken<void>();
            if (!event.payload_cpp_type.isValid() ||
                event.payload_schema_name.empty() !=
                    (event.payload_schema_version == 0U) ||
                void_payload != event.payload_schema_name.empty())
            {
                return lux::cxx::unexpected(failure(
                    ESimulationDescriptionError::INVALID_EVENT_PAYLOAD_SCHEMA
                ));
            }
        }
        if (failMutationForTest())
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::ALLOCATION_FAILURE
            ));

        try
        {
            Impl::PendingSystem candidate;
            candidate.instance_name = instance_name;
            candidate.type = systemTypeId(system.canonical_name);
            candidate.version = system.version;
            candidate.configuration_schema_name =
                system.configuration_schema_name;
            candidate.configuration_schema_hash =
                system.configuration_schema_name.empty()
                ? 0U
                : lux::cxx::Fnv1a64::hash(system.configuration_schema_name);
            candidate.configuration_schema_version =
                system.configuration_schema_version;
            candidate.configuration.assign(configuration.begin(), configuration.end());
            candidate.capabilities.reserve(system.capabilities.size());
            for (const auto value : system.capabilities)
                candidate.capabilities.emplace_back(value);
            candidate.execution_points.reserve(system.execution_points.size());
            for (const auto value : system.execution_points)
                candidate.execution_points.emplace_back(value.name);
            candidate.events.reserve(system.events.size());
            for (const auto& event : system.events)
            {
                const auto point = std::find(
                    candidate.execution_points.begin(),
                    candidate.execution_points.end(),
                    event.dispatch_point
                );
                candidate.events.push_back({
                    std::string(event.name),
                    static_cast<std::size_t>(
                        std::distance(candidate.execution_points.begin(), point)),
                    std::string(event.payload_schema_name),
                    event.payload_schema_name.empty()
                        ? 0U
                        : lux::cxx::Fnv1a64::hash(event.payload_schema_name),
                    event.payload_schema_version
                });
            }

            for (const auto& existing : impl_->systems)
            {
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
        std::string_view instance_name
    ) noexcept
    {
        const auto system = findSystem(impl_->systems, instance_name);
        if (system == impl_->systems.end())
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::SYSTEM_NOT_FOUND
            ));
        impl_->dependencies.erase(
            std::remove_if(
                impl_->dependencies.begin(),
                impl_->dependencies.end(),
                [instance_name](const auto& dependency) noexcept
                {
                    return dependency.before_system == instance_name ||
                        dependency.after_system == instance_name;
                }
            ),
            impl_->dependencies.end()
        );
        impl_->systems.erase(system);
        return {};
    }

    lux::cxx::expected<void, SimulationDescriptionFailure>
    SimulationDescriptionBuilder::setSystemConfiguration(
        std::string_view instance_name,
        std::span<const std::byte> configuration
    ) noexcept
    {
        const auto system = findSystem(impl_->systems, instance_name);
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
        std::string_view before_system,
        SystemExecutionPoint before_point,
        std::string_view after_system,
        SystemExecutionPoint after_point
    ) noexcept
    {
        return addDependency(
            before_system,
            before_point.name,
            after_system,
            after_point.name
        );
    }

    lux::cxx::expected<void, SimulationDescriptionFailure>
    SimulationDescriptionBuilder::addDependency(
        std::string_view before_system,
        std::string_view before_point,
        std::string_view after_system,
        std::string_view after_point
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
        if (std::find(
                before->execution_points.begin(),
                before->execution_points.end(),
                before_point
            ) == before->execution_points.end() ||
            std::find(
                after->execution_points.begin(),
                after->execution_points.end(),
                after_point
            ) == after->execution_points.end())
        {
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::EXECUTION_POINT_NOT_FOUND
            ));
        }

        try
        {
            Impl::PendingDependency candidate{
                std::string(before_system),
                std::string(before_point),
                std::string(after_system),
                std::string(after_point)};
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
        std::string_view before_system,
        std::string_view before_point,
        std::string_view after_system,
        std::string_view after_point
    ) noexcept
    {
        const auto dependency = std::find_if(
            impl_->dependencies.begin(),
            impl_->dependencies.end(),
            [&](const auto& value) noexcept
            {
                return value.before_system == before_system &&
                    value.before_point == before_point &&
                    value.after_system == after_system &&
                    value.after_point == after_point;
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
                    return left.instance_name < right.instance_name;
                }
            );
            std::sort(
                impl_->dependencies.begin(),
                impl_->dependencies.end(),
                [](const auto& left, const auto& right) noexcept
                {
                    if (left.before_system != right.before_system)
                        return left.before_system < right.before_system;
                    if (left.before_point != right.before_point)
                        return left.before_point < right.before_point;
                    if (left.after_system != right.after_system)
                        return left.after_system < right.after_system;
                    return left.after_point < right.after_point;
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
                    return SystemTypeIdLess{}(
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
                record.configuration_schema_name = source.configuration_schema_name;
                record.configuration_schema_hash = source.configuration_schema_hash;
                record.configuration_schema_version =
                    source.configuration_schema_version;
                record.capabilities = source.capabilities;
                record.execution_points = source.execution_points;
                record.events.reserve(source.events.size());
                for (const auto& event : source.events)
                {
                    record.events.push_back({
                        event.name,
                        event.dispatch_point_ordinal,
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
            result.configuration_payload_.reserve(total_configuration);
            for (auto& source : impl_->systems)
            {
                const auto type = std::lower_bound(
                    result.system_types_.begin(),
                    result.system_types_.end(),
                    source.type,
                    [](const auto& candidate, const SystemTypeId& id) noexcept
                    {
                        return SystemTypeIdLess{}(candidate.type, id);
                    }
                );
                result.systems_.push_back({
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
                    [](const auto& system, std::string_view name) noexcept
                    {
                        return system.instance_name < name;
                    }
                );
                const auto after_system = std::lower_bound(
                    result.systems_.begin(),
                    result.systems_.end(),
                    dependency.after_system,
                    [](const auto& system, std::string_view name) noexcept
                    {
                        return system.instance_name < name;
                    }
                );
                const auto before_index = static_cast<std::size_t>(
                    std::distance(result.systems_.begin(), before_system));
                const auto after_index = static_cast<std::size_t>(
                    std::distance(result.systems_.begin(), after_system));
                const auto before_type = before_system->type_ordinal;
                const auto after_type = after_system->type_ordinal;
                const auto before_point = std::find(
                    result.system_types_[before_type].execution_points.begin(),
                    result.system_types_[before_type].execution_points.end(),
                    dependency.before_point
                );
                const auto after_point = std::find(
                    result.system_types_[after_type].execution_points.begin(),
                    result.system_types_[after_type].execution_points.end(),
                    dependency.after_point
                );
                result.dependencies_.push_back({
                    before_index,
                    static_cast<std::size_t>(std::distance(
                        result.system_types_[before_type].execution_points.begin(),
                        before_point)),
                    after_index,
                    static_cast<std::size_t>(std::distance(
                        result.system_types_[after_type].execution_points.begin(),
                        after_point))
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
