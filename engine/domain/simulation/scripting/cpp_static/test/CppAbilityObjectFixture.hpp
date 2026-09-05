#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/simulation/scripting/cpp_static/ScriptCoroutine.hpp>

namespace lux::simulation::test
{
struct LUX_TYPE_INFO(compile_time) CoroutineAbilityObject final
{
    LUX_METHOD(script_export = "ability.execute", script_coroutine = true)
    script::ScriptCoroutine execute(script::ScriptCoroutineContext &context) noexcept;
};
} // namespace lux::simulation::test
