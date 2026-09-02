#pragma once

#include <lux/engine/simulation/SimulationDescription.hpp>
#include <lux/engine/system/SystemInstanceId.hpp>
#include <lux/engine/system/SystemTypeId.hpp>
#include <lux/engine/serialization/PortableValueCodec.hpp>
#include <lux/engine/simulation/SimulationSystemDescription.hpp>
#include <lux/engine/simulation/SystemAccessSpec.hpp>
#include <lux/engine/simulation/system/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace lux::simulation
{
    class SimulationBuilder;

    enum class ESimulationSystemBuildError : std::uint8_t
    {
        INVALID_DESCRIPTION,
        UNKNOWN_SYSTEM_TYPE,
        VERSION_MISMATCH,
        DUPLICATE_SYSTEM,
        CONSTRUCTION_FAILURE,
        CONFIGURATION_DECODE_FAILURE,
        ALLOCATION_FAILURE,
        DEPENDENCY_CYCLE,
        UNDECLARED_CONSTRUCTOR_DEPENDENCY,
        DUPLICATE_PRIMARY_TASK,
        MISSING_PRIMARY_TASK,
        TASK_GRAPH_FAILURE,
        COMMAND_PREPARE_FAILURE,
        SCRIPT_CAPABILITY_AMBIGUOUS_PROVIDER,
        INVALID_SCRIPT_ENDPOINT,
        DUPLICATE_SCRIPT_ENDPOINT,
    };

    struct SimulationSystemBuildFailure final
    {
        ESimulationSystemBuildError code{ESimulationSystemBuildError::INVALID_DESCRIPTION};
        lux::system::SystemInstanceId system{};
        lux::system::SystemInstanceId related{};
        lux::serialization::SerializationFailure configuration{};
    };

    using InstallSimulationSystemFn = lux::cxx::expected<void, SimulationSystemBuildFailure> (*)(
        SimulationBuilder& builder,
        SimulationSystemView description
    ) noexcept;

    struct SimulationSystemRegistration final
    {
        lux::system::SystemTypeId type;
        lux::cxx::TypeToken cpp_type;
        const SimulationSystemDescription* description{};
        SystemAccessSpec access{};
        lux::serialization::PortableValueCodec configuration{};
        InstallSimulationSystemFn install{};
    };

    enum class ESimulationSystemRegistrationError : std::uint8_t
    {
        INVALID_REGISTRATION,
        DUPLICATE_TYPE,
        TYPE_COLLISION,
        ALLOCATION_FAILURE,
    };

    struct SimulationSystemRegistrationFailure final
    {
        ESimulationSystemRegistrationError code{ESimulationSystemRegistrationError::INVALID_REGISTRATION};
        lux::system::SystemTypeId type;
    };

    class LUX_ENGINE_SIMULATION_SYSTEM_PUBLIC SimulationSystemRegistry final
    {
    public:
        SimulationSystemRegistry();
        ~SimulationSystemRegistry();

        SimulationSystemRegistry(SimulationSystemRegistry&&) noexcept;
        SimulationSystemRegistry& operator=(SimulationSystemRegistry&&) noexcept;

        SimulationSystemRegistry(const SimulationSystemRegistry&) = delete;
        SimulationSystemRegistry& operator=(const SimulationSystemRegistry&) = delete;

        [[nodiscard]] lux::cxx::expected<void, SimulationSystemRegistrationFailure>
        add(SimulationSystemRegistration registration) noexcept;

        [[nodiscard]] lux::cxx::expected<void, SimulationSystemRegistrationFailure>
        add(std::span<const SimulationSystemRegistration> registrations) noexcept;

        [[nodiscard]] const SimulationSystemRegistration* find(const lux::system::SystemTypeId& type) const noexcept;
        [[nodiscard]] std::span<const SimulationSystemRegistration> all() const noexcept;
        [[nodiscard]] std::size_t size() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::simulation
