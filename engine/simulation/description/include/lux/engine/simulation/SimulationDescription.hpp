#pragma once

#include <lux/engine/simulation/SimulationDataSchemaId.hpp>
#include <lux/engine/simulation/SystemTypeId.hpp>
#include <lux/engine/simulation/description/visibility.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lux::simulation
{
    class SimulationDescription;
    class SimulationDescriptionBuilder;
    class SimulationSystemView;
    class SimulationExecutionPointView;
    class SimulationEventView;
    class SimulationDependencyView;

    class LUX_ENGINE_SIMULATION_DESCRIPTION_PUBLIC SimulationDataView final
    {
      public:
        SimulationDataView() noexcept = default;
        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] const SimulationDataSchemaId& schema() const noexcept;
        [[nodiscard]] std::uint32_t version() const noexcept;
        [[nodiscard]] std::span<const std::byte> payload() const noexcept;

      private:
        SimulationDataView(
            const SimulationDescription& description,
            std::size_t data_index
        ) noexcept;

        const SimulationDescription* description_{};
        std::size_t data_index_{};
        friend class SimulationDescription;
    };

    class LUX_ENGINE_SIMULATION_DESCRIPTION_PUBLIC SimulationSystemView final
    {
      public:
        SimulationSystemView() noexcept = default;
        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] std::string_view instanceName() const noexcept;
        [[nodiscard]] const SystemTypeId& type() const noexcept;
        [[nodiscard]] std::uint32_t version() const noexcept;
        [[nodiscard]] std::string_view configurationSchemaName() const noexcept;
        [[nodiscard]] std::uint64_t configurationSchemaHash() const noexcept;
        [[nodiscard]] std::uint32_t configurationSchemaVersion() const noexcept;
        [[nodiscard]] std::span<const std::byte> configurationPayload() const noexcept;
        [[nodiscard]] std::size_t capabilityCount() const noexcept;
        [[nodiscard]] std::string_view capabilityAt(std::size_t index) const noexcept;
        [[nodiscard]] bool hasCapability(std::string_view name) const noexcept;
        [[nodiscard]] std::size_t executionPointCount() const noexcept;
        [[nodiscard]] SimulationExecutionPointView executionPointAt(
            std::size_t index
        ) const noexcept;
        [[nodiscard]] SimulationExecutionPointView findExecutionPoint(
            std::string_view name
        ) const noexcept;
        [[nodiscard]] std::size_t eventCount() const noexcept;
        [[nodiscard]] SimulationEventView eventAt(std::size_t index) const noexcept;
        [[nodiscard]] SimulationEventView findEvent(std::string_view name) const noexcept;

      private:
        SimulationSystemView(
            const SimulationDescription& description,
            std::size_t system_index
        ) noexcept;

        const SimulationDescription* description_{};
        std::size_t system_index_{};
        friend class SimulationDescription;
        friend class SimulationExecutionPointView;
        friend class SimulationEventView;
    };

    class LUX_ENGINE_SIMULATION_DESCRIPTION_PUBLIC
        SimulationExecutionPointView final
    {
      public:
        SimulationExecutionPointView() noexcept = default;
        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] SimulationSystemView system() const noexcept;
        [[nodiscard]] std::string_view name() const noexcept;

      private:
        SimulationExecutionPointView(
            const SimulationDescription& description,
            std::size_t system_index,
            std::size_t point_index
        ) noexcept;

        const SimulationDescription* description_{};
        std::size_t system_index_{};
        std::size_t point_index_{};
        friend class SimulationDescription;
        friend class SimulationSystemView;
        friend class SimulationEventView;
        friend class SimulationDependencyView;
    };

    class LUX_ENGINE_SIMULATION_DESCRIPTION_PUBLIC SimulationEventView final
    {
      public:
        SimulationEventView() noexcept = default;
        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] SimulationSystemView system() const noexcept;
        [[nodiscard]] std::string_view name() const noexcept;
        [[nodiscard]] SimulationExecutionPointView dispatchPoint() const noexcept;
        [[nodiscard]] std::string_view payloadSchemaName() const noexcept;
        [[nodiscard]] std::uint64_t payloadSchemaHash() const noexcept;
        [[nodiscard]] std::uint32_t payloadSchemaVersion() const noexcept;

      private:
        SimulationEventView(
            const SimulationDescription& description,
            std::size_t system_index,
            std::size_t event_index
        ) noexcept;

        const SimulationDescription* description_{};
        std::size_t system_index_{};
        std::size_t event_index_{};
        friend class SimulationDescription;
        friend class SimulationSystemView;
    };

    class LUX_ENGINE_SIMULATION_DESCRIPTION_PUBLIC SimulationDependencyView final
    {
      public:
        SimulationDependencyView() noexcept = default;
        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] SimulationExecutionPointView before() const noexcept;
        [[nodiscard]] SimulationExecutionPointView after() const noexcept;

      private:
        SimulationDependencyView(
            const SimulationDescription& description,
            std::size_t dependency_index
        ) noexcept;

        const SimulationDescription* description_{};
        std::size_t dependency_index_{};
        friend class SimulationDescription;
    };

    class LUX_ENGINE_SIMULATION_DESCRIPTION_PUBLIC SimulationDescription final
    {
      public:
        SimulationDescription() noexcept = default;
        SimulationDescription(SimulationDescription&&) noexcept = default;
        SimulationDescription& operator=(SimulationDescription&&) noexcept = default;
        ~SimulationDescription() = default;
        SimulationDescription(const SimulationDescription&) = delete;
        SimulationDescription& operator=(const SimulationDescription&) = delete;

        [[nodiscard]] bool empty() const noexcept;
        [[nodiscard]] std::size_t dataCount() const noexcept;
        [[nodiscard]] std::size_t payloadBytes() const noexcept;
        [[nodiscard]] std::size_t configurationPayloadBytes() const noexcept;
        [[nodiscard]] std::size_t retainedBytes() const noexcept;
        [[nodiscard]] std::span<const SimulationDataSchemaId> schemas() const noexcept;
        [[nodiscard]] SimulationDataView dataAt(std::size_t index) const noexcept;
        [[nodiscard]] SimulationDataView findData(
            const SimulationDataSchemaId& schema
        ) const noexcept;
        [[nodiscard]] std::size_t systemCount() const noexcept;
        [[nodiscard]] SimulationSystemView systemAt(std::size_t index) const noexcept;
        [[nodiscard]] SimulationSystemView findSystem(
            std::string_view instance_name
        ) const noexcept;
        [[nodiscard]] bool hasCapability(std::string_view name) const noexcept;
        [[nodiscard]] SimulationExecutionPointView findExecutionPoint(
            std::string_view system_instance,
            std::string_view point_name
        ) const noexcept;
        [[nodiscard]] SimulationEventView findEvent(
            std::string_view system_instance,
            std::string_view event_name
        ) const noexcept;
        [[nodiscard]] std::size_t dependencyCount() const noexcept;
        [[nodiscard]] SimulationDependencyView dependencyAt(
            std::size_t index
        ) const noexcept;

      private:
        struct DataRecord final
        {
            std::size_t schema_ordinal{};
            std::uint32_t version{};
            std::size_t payload_offset{};
            std::size_t payload_size{};
        };

        struct EventRecord final
        {
            std::string name;
            std::size_t dispatch_point_ordinal{};
            std::string payload_schema_name;
            std::uint64_t payload_schema_hash{};
            std::uint32_t payload_schema_version{};
        };

        struct SystemTypeRecord final
        {
            SystemTypeId type;
            std::uint32_t version{};
            std::string configuration_schema_name;
            std::uint64_t configuration_schema_hash{};
            std::uint32_t configuration_schema_version{};
            std::vector<std::string> capabilities;
            std::vector<std::string> execution_points;
            std::vector<EventRecord> events;
        };

        struct SystemRecord final
        {
            std::string instance_name;
            std::size_t type_ordinal{};
            std::size_t configuration_offset{};
            std::size_t configuration_size{};
        };

        struct DependencyRecord final
        {
            std::size_t before_system{};
            std::size_t before_point{};
            std::size_t after_system{};
            std::size_t after_point{};
        };

        std::vector<SimulationDataSchemaId> schemas_;
        std::vector<DataRecord> data_;
        std::vector<std::byte> payload_;
        std::vector<SystemTypeRecord> system_types_;
        std::vector<SystemRecord> systems_;
        std::vector<std::byte> configuration_payload_;
        std::vector<DependencyRecord> dependencies_;

        friend class SimulationDataView;
        friend class SimulationSystemView;
        friend class SimulationExecutionPointView;
        friend class SimulationEventView;
        friend class SimulationDependencyView;
        friend class SimulationDescriptionBuilder;
    };
}
