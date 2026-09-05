#pragma once

#include <lux/engine/simulation/HookPoint.hpp>

#include <cassert>
#include <utility>

namespace lux::simulation::detail
{
    struct HookInvocationTestAccess final
    {
        template <class Hook, class... Arguments>
        static std::size_t dispatch(Hook& hook, Arguments&&... arguments) noexcept
        {
            // Composed Hooks must be driven by their Simulation; this helper only covers standalone primitives.
            assert(hook.binding_owner_ == nullptr);
            const HookInvocation invocation{nullptr, hook.binding_system_, hook.binding_hook_, {}, true, false};
            return hook.dispatch(invocation, std::forward<Arguments>(arguments)...);
        }
    };
}

namespace lux::simulation::test
{
    template <class Hook, class... Arguments>
    std::size_t dispatchHookForTest(Hook& hook, Arguments&&... arguments) noexcept
    {
        return detail::HookInvocationTestAccess::dispatch(hook, std::forward<Arguments>(arguments)...);
    }
}
