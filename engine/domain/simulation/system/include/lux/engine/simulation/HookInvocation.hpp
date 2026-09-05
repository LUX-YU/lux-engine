#pragma once

#include <lux/engine/simulation/SimulationClock.hpp>
#include <lux/engine/simulation/SimulationEndpointId.hpp>
#include <lux/engine/system/SystemInstanceId.hpp>

namespace lux::simulation
{
    class Simulation;
    namespace detail { struct HookInvocationTestAccess; }
    template <class Signature> class HookPoint;
    template <class Route, class Payload> class HookChannel;

    class HookInvocation final
    {
    public:
        HookInvocation(const HookInvocation&) = delete;
        HookInvocation& operator=(const HookInvocation&) = delete;
        [[nodiscard]] const SimulationClockSnapshot& clock() const noexcept { return clock_; }
        [[nodiscard]] lux::system::SystemInstanceId system() const noexcept { return system_; }
        [[nodiscard]] HookPointId hook() const noexcept { return hook_; }
        [[nodiscard]] bool scriptCapable() const noexcept { return script_capable_; }
        [[nodiscard]] bool stableResume() const noexcept { return stable_resume_; }

    private:
        HookInvocation(const void* owner, lux::system::SystemInstanceId system, HookPointId hook,
            SimulationClockSnapshot clock, bool script_capable, bool stable_resume) noexcept
            : owner_(owner), system_(system), hook_(hook), clock_(clock),
              script_capable_(script_capable), stable_resume_(stable_resume)
        {}
        const void* owner_{};
        lux::system::SystemInstanceId system_;
        HookPointId hook_;
        SimulationClockSnapshot clock_;
        bool script_capable_{};
        bool stable_resume_{};
        friend class Simulation;
        friend struct detail::HookInvocationTestAccess;
        template <class> friend class HookPoint;
        template <class, class> friend class HookChannel;
    };

    namespace detail
    {
        struct PreparedHookInvocation final { const HookInvocation* current{}; };
    }
}
