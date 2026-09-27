#pragma once

#include <lux/engine/simulation/SimulationTime.hpp>
#include <lux/engine/simulation/SimulationEndpointId.hpp>
#include <lux/engine/system/SystemInstanceId.hpp>

namespace lux::simulation
{
    class Simulation;
    template <class Signature> class THookPoint;
    template <class Route, class Payload> class THookChannel;

    class HookInvocation final
    {
    public:
        HookInvocation(const HookInvocation&) = delete;
        HookInvocation& operator=(const HookInvocation&) = delete;
        [[nodiscard]] const SimulationTime& time() const noexcept
        {
            return time_;
        }
        [[nodiscard]] lux::system::SystemInstanceId system() const noexcept
        {
            return system_;
        }
        [[nodiscard]] HookPointId hook() const noexcept
        {
            return hook_;
        }
        [[nodiscard]] bool scriptCapable() const noexcept
        {
            return script_capable_;
        }
        [[nodiscard]] bool stableResume() const noexcept
        {
            return stable_resume_;
        }

    private:
        HookInvocation(
            const void* owner,
            lux::system::SystemInstanceId system,
            HookPointId hook,
            SimulationTime time,
            bool script_capable,
            bool stable_resume
        ) noexcept
            : owner_(owner), system_(system), hook_(hook), time_(time), script_capable_(script_capable),
              stable_resume_(stable_resume)
        {}
        const void* owner_{};
        lux::system::SystemInstanceId system_;
        HookPointId hook_;
        SimulationTime time_;
        bool script_capable_{};
        bool stable_resume_{};
        friend class Simulation;
        template <class> friend class THookPoint;
        template <class, class> friend class THookChannel;
    };

    namespace detail
    {
        struct PreparedHookInvocation final
        {
            const HookInvocation* current{};
        };
    }
}
