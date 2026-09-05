#pragma once

#include <lux/engine/simulation/SimulationDescription.hpp>
#include <lux/engine/simulation/SimulationSystemDescription.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace lux::simulation
{
    enum class ESimulationDescriptionError : std::uint8_t
    {
        INVALID_SCHEMA_ID,
        SCHEMA_HASH_COLLISION,
        INVALID_SCHEMA_VERSION,
        DUPLICATE_DATA,
        DATA_NOT_FOUND,
        INVALID_SYSTEM_INSTANCE_NAME,
        INVALID_SYSTEM_TYPE,
        SYSTEM_TYPE_HASH_COLLISION,
        INVALID_SYSTEM_VERSION,
        DUPLICATE_SYSTEM_INSTANCE,
        SYSTEM_MULTIPLICITY_VIOLATION,
        INVALID_CONFIGURATION_SCHEMA,
        INVALID_CAPABILITY,
        DUPLICATE_CAPABILITY,
        INVALID_HOOK_POINT,
        DUPLICATE_HOOK_POINT,
        INVALID_EVENT,
        DUPLICATE_EVENT,
        INVALID_EVENT_DISPATCH_HOOK,
        INVALID_EVENT_PAYLOAD_SCHEMA,
        SYSTEM_NOT_FOUND,
        INVALID_DEPENDENCY,
        DUPLICATE_DEPENDENCY,
        DEPENDENCY_CYCLE,
        SIZE_OVERFLOW,
        ALLOCATION_FAILURE,
    };

    struct SimulationDescriptionFailure final
    {
        ESimulationDescriptionError code{
            ESimulationDescriptionError::ALLOCATION_FAILURE};
        SimulationDataSchemaId schema;
        std::uint64_t subject_hash{};
    };

    class LUX_ENGINE_SIMULATION_DESCRIPTION_PUBLIC
        SimulationDescriptionBuilder final
    {
      public:
        SimulationDescriptionBuilder();
        ~SimulationDescriptionBuilder();
        SimulationDescriptionBuilder(SimulationDescriptionBuilder&&) noexcept;
        SimulationDescriptionBuilder& operator=(
            SimulationDescriptionBuilder&&
        ) noexcept;

        SimulationDescriptionBuilder(const SimulationDescriptionBuilder&) = delete;
        SimulationDescriptionBuilder& operator=(
            const SimulationDescriptionBuilder&
        ) = delete;

        [[nodiscard]] lux::cxx::expected<void, SimulationDescriptionFailure>
        addData(
            SimulationDataSchemaId schema,
            std::uint32_t version,
            std::span<const std::byte> payload
        ) noexcept;

        [[nodiscard]] lux::cxx::expected<void, SimulationDescriptionFailure>
        setData(
            SimulationDataSchemaId schema,
            std::uint32_t version,
            std::span<const std::byte> payload
        ) noexcept;

        [[nodiscard]] lux::cxx::expected<void, SimulationDescriptionFailure>
        eraseData(const SimulationDataSchemaId& schema) noexcept;

        [[nodiscard]] lux::cxx::expected<void, SimulationDescriptionFailure>
        addSystem(
            lux::system::SystemInstanceId instance_id,
            std::string_view instance_name,
            const SimulationSystemDescription& system,
            std::span<const std::byte> configuration = {}
        ) noexcept;

        [[nodiscard]] lux::cxx::expected<void, SimulationDescriptionFailure>
        eraseSystem(lux::system::SystemInstanceId instance_id) noexcept;

        [[nodiscard]] lux::cxx::expected<void, SimulationDescriptionFailure>
        setSystemConfiguration(
            lux::system::SystemInstanceId instance_id,
            std::span<const std::byte> configuration
        ) noexcept;

        [[nodiscard]] lux::cxx::expected<void, SimulationDescriptionFailure>
        addConstructionDependency(
            lux::system::SystemInstanceId before_system,
            lux::system::SystemInstanceId after_system
        ) noexcept;

        [[nodiscard]] lux::cxx::expected<void, SimulationDescriptionFailure>
        eraseConstructionDependency(
            lux::system::SystemInstanceId before_system,
            lux::system::SystemInstanceId after_system
        ) noexcept;

        void clear() noexcept;

        [[nodiscard]] lux::cxx::expected<void, SimulationDescriptionFailure>
        addExecutionDependency(SimulationExecutionPoint before, SimulationExecutionPoint after) noexcept;

        [[nodiscard]] lux::cxx::expected<
            SimulationDescription,
            SimulationDescriptionFailure>
        build() && noexcept;

      private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
