#pragma once

#include <lux/engine/simulation/SimulationDescription.hpp>
#include <lux/engine/simulation/SystemTypeId.hpp>
#include <lux/engine/simulation/system/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace lux::simulation
{
    class SimulationBuilder;

    enum class ESystemBuildError : std::uint8_t
    {
        INVALID_DESCRIPTION,
        UNKNOWN_SYSTEM_TYPE,
        VERSION_MISMATCH,
        DUPLICATE_SYSTEM,
        CONSTRUCTION_FAILURE,
        ALLOCATION_FAILURE,
        DEPENDENCY_CYCLE,
        UNDECLARED_CONSTRUCTOR_DEPENDENCY,
        DUPLICATE_PRIMARY_TASK,
        MISSING_PRIMARY_TASK,
        TASK_GRAPH_FAILURE,
        COMMAND_PREPARE_FAILURE,
    };

    struct SystemBuildFailure final
    {
        ESystemBuildError code{ESystemBuildError::INVALID_DESCRIPTION};
        SystemInstanceId system{};
        SystemInstanceId related{};
    };

    using InstallSystemFn = lux::cxx::expected<void, SystemBuildFailure> (*)(
        SimulationBuilder& builder,
        SimulationSystemView description
    ) noexcept;

    struct SystemRegistration final
    {
        SystemTypeId type;
        std::uint32_t version{};
        InstallSystemFn install{};
    };

    enum class ESystemRegistrationError : std::uint8_t
    {
        INVALID_REGISTRATION,
        DUPLICATE_TYPE,
        TYPE_COLLISION,
        ALLOCATION_FAILURE,
    };

    struct SystemRegistrationFailure final
    {
        ESystemRegistrationError code{ESystemRegistrationError::INVALID_REGISTRATION};
        SystemTypeId type;
    };

    class LUX_ENGINE_SIMULATION_SYSTEM_PUBLIC SystemRegistry final
    {
    public:
        SystemRegistry();
        ~SystemRegistry();

        SystemRegistry(SystemRegistry&&) noexcept;
        SystemRegistry& operator=(SystemRegistry&&) noexcept;

        SystemRegistry(const SystemRegistry&) = delete;
        SystemRegistry& operator=(const SystemRegistry&) = delete;

        [[nodiscard]] lux::cxx::expected<void, SystemRegistrationFailure>
        add(SystemRegistration registration) noexcept;

        [[nodiscard]] lux::cxx::expected<void, SystemRegistrationFailure>
        add(std::span<const SystemRegistration> registrations) noexcept;

        [[nodiscard]] const SystemRegistration* find(const SystemTypeId& type) const noexcept;
        [[nodiscard]] std::size_t size() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::simulation
