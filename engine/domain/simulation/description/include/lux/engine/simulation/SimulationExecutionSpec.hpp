#pragma once

#include <lux/engine/simulation/SimulationEndpointId.hpp>
#include <lux/engine/system/SystemInstanceId.hpp>

#include <compare>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace lux::simulation
{
    struct SimulationTaskId final
    {
        std::uint64_t value{};
        [[nodiscard]] constexpr bool valid() const noexcept { return value != 0U; }
        friend constexpr auto operator<=>(SimulationTaskId, SimulationTaskId) noexcept = default;
    };

    inline constexpr SimulationTaskId PrimarySimulationTask{1U};

    struct SimulationTaskSpec final
    {
        SimulationTaskId id;
        std::string_view name;
    };

    inline constexpr std::array DefaultSimulationTasks{SimulationTaskSpec{PrimarySimulationTask, "execute"}};

    struct SimulationTaskDescription final
    {
        SimulationTaskId id;
        std::string name;
        friend bool operator==(const SimulationTaskDescription&, const SimulationTaskDescription&) noexcept = default;
    };

    enum class ESimulationExecutionPoint : std::uint8_t
    {
        SYSTEM_TASK,
        HOOK,
    };

    struct SimulationExecutionPoint final
    {
        lux::system::SystemInstanceId system;
        std::uint64_t point{};
        ESimulationExecutionPoint kind{ESimulationExecutionPoint::SYSTEM_TASK};

        [[nodiscard]] static constexpr SimulationExecutionPoint task(
            lux::system::SystemInstanceId system,
            SimulationTaskId id = PrimarySimulationTask
        ) noexcept
        {
            return {system, id.value, ESimulationExecutionPoint::SYSTEM_TASK};
        }

        [[nodiscard]] static constexpr SimulationExecutionPoint hook(
            lux::system::SystemInstanceId system, HookPointId id
        ) noexcept
        {
            return {system, id.value, ESimulationExecutionPoint::HOOK};
        }

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return system.valid() && point != 0U && kind <= ESimulationExecutionPoint::HOOK;
        }

        friend constexpr bool operator==(SimulationExecutionPoint, SimulationExecutionPoint) noexcept = default;
    };

    struct SimulationExecutionDependency final
    {
        SimulationExecutionPoint before;
        SimulationExecutionPoint after;
        friend constexpr bool operator==(
            SimulationExecutionDependency, SimulationExecutionDependency
        ) noexcept = default;
    };

    struct SimulationChannelProducer final
    {
        lux::system::SystemInstanceId system;
        EventPointId event;
        lux::system::SystemInstanceId producer_system;
        SimulationTaskId stage;
        friend constexpr bool operator==(SimulationChannelProducer, SimulationChannelProducer) noexcept = default;
    };
}
