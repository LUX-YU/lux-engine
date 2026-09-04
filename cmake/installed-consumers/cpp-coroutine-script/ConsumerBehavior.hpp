#pragma once

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/simulation/scripting/cpp_static/ScriptCoroutine.hpp>

#include <cstdint>

namespace installed_consumer
{
    inline std::int32_t observed{};

    class LUX_TYPE_INFO(runtime) CoroutineBehavior final
    {
    public:
        LUX_METHOD()
        lux::simulation::script::ScriptCoroutine run(
            lux::simulation::script::ScriptCoroutineContext& context
        ) noexcept
        {
            observed = 1;
            co_await lux::simulation::script::CppStaticCoroutineAccess::makeAwaiter<void>(
                context,
                [](
                    lux::simulation::script::ScriptCoroutineContext&,
                    lux::simulation::script::ScriptStepContext& step
                ) noexcept
                {
                    const auto waiting = step.awaitables.create();
                    return waiting
                        ? lux::simulation::script::ScriptStepResult::suspended(waiting->id)
                        : lux::simulation::script::ScriptStepResult::failed(-1);
                }
            );
            observed = 2;
        }
    };
}
