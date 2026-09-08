#pragma once

#include "../../../system/test/HookInvocationTestAccess.hpp"
#include "../../../scripting/core/test/ScriptEndpointTestAccess.hpp"
#include <lux/engine/simulation/ScriptSystem.hpp>

namespace lux::simulation::script::test
{
    // Graphless fixtures own an explicit region. Nested test calls reuse the surrounding region;
    // lifecycle reentry is already protected by the runtime's construction/retirement scope.
    template <class Function> decltype(auto) inRuntimeRegion(ScriptSystem& system, Function&& function)
    {
        std::optional<ScriptSystem::ExecutionRegion> region;
        if (!system.inExecutionRegion() && !system.isShutdown())
        {
            auto entered = system.beginExecutionRegion();
            if (entered)
                region.emplace(std::move(*entered));
            else if (entered.error() != EScriptSystemError::ENDPOINT_BUSY)
                std::terminate();
        }
        return std::forward<Function>(function)();
    }

    template <class Hook, class... Arguments>
    decltype(auto) dispatchRuntimeHook(ScriptSystem& system, Hook& hook, Arguments&&... arguments)
    {
        return inRuntimeRegion(system, [&]() -> decltype(auto) {
            return lux::simulation::test::dispatchHookForTest(hook, std::forward<Arguments>(arguments)...);
        });
    }

    template <class Endpoint> decltype(auto) deliverRuntimeEvent(ScriptSystem& system, Endpoint& endpoint)
    {
        return inRuntimeRegion(system, [&]() -> decltype(auto) { return deliverEndpoint(endpoint); });
    }

    [[nodiscard]] inline lux::cxx::expected<ScriptStablePointReport, EScriptSystemError>
    executeRuntimeStablePoint(ScriptSystem& system)
    {
        if (!system.inExecutionRegion())
        {
            const auto lifecycle = system.processLifecycle();
            if (!lifecycle) return lux::cxx::unexpected(lifecycle.error());
        }
        return inRuntimeRegion(system, [&] { return system.executeStablePoint(); });
    }
}
