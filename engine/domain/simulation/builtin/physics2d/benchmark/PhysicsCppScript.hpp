#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/physics2d/abilities/PhysicsQuery2D.hpp>
#include <PhysicsQuery2D.ability.generated.hpp>
#include <lux/engine/simulation/scripting/cpp_static/ScriptDelayCoroutine.hpp>
namespace lux::physics2d::benchmark
{
using namespace lux::simulation::script;
inline std::uint64_t cpp_checksum{};
struct LUX_TYPE_INFO(compile_time) CppObject final
{
    std::uint64_t value{};
    LUX_METHOD(script_export = "physics.tick")
    void tick() noexcept
    {
        ++value;
        cpp_checksum += value;
    }
    LUX_METHOD(script_export = "physics.coroutine", script_coroutine = true)
    ScriptCoroutine coroutineTick(ScriptCoroutineContext &context) noexcept
    {
        auto physics = context.ability<PhysicsQuery2D>();
        if (!physics)
            co_return;
        const auto overlap = physics->overlapsBox(0.0, 0.0, 0.25, 0.25);
        if (!overlap)
            co_return;
        value += static_cast<std::uint64_t>(*overlap);
        co_await context.delay().nextStep();
        ++value;
        cpp_checksum += value;
    }
};
} // namespace lux::physics2d::benchmark
