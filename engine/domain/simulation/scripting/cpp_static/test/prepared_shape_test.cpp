#include <lux/engine/simulation/scripting/cpp_static/CppStaticScriptBridge.hpp>

#include "NarrowShapeFixture.hpp"
#include "WideShapeFixture.hpp"
#include "NarrowShapeFixture.ShapeNarrow.script.generated.hpp"
#include "WideShapeFixture.ShapeWide.script.generated.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <vector>

namespace
{
using namespace lux::simulation::script;
std::size_t run(std::size_t population, bool include_wide)
{
    const auto &contract = include_wide ? generated::ShapeWide : generated::ShapeNarrow;
    auto description = materializeCppStaticScript(contract);
    assert(description);
    auto artifact = lux::script::ScriptArtifact::create(std::move(*description), {});
    assert(artifact);
    const std::array pools{
        CppStaticScriptPoolDescription{&contract, population, 0U, 0U, alignof(std::max_align_t), population}};
    auto backend = CppStaticScriptBackend::create(pools);
    assert(backend);
    const auto runtime = backend->descriptor();
    std::vector<ScriptBackendInstance> instances(population);
    std::vector<ScriptBackendPreparedMethod> methods(population);
    for (std::size_t index{}; index < population; ++index)
    {
        assert(
            runtime.createInstance(runtime.context,
                                   {{}, SimulationScriptScope{}, nullptr, {static_cast<std::uint32_t>(index + 1U), 1U}},
                                   *artifact, instances[index]) == EScriptBackendResult::SUCCESS);
        assert(runtime.prepareMethod(runtime.context, instances[index], artifact->description().exports[0],
                                     methods[index]) == EScriptBackendResult::SUCCESS);
        lux_script_call_frame frame{};
        frame.user_context = methods[index].synchronous.context;
        assert(methods[index].synchronous.invoke(&frame) == 0);
    }
    const auto stats = backend->stats();
    assert(stats.active_prepared_methods == population);
    if (include_wide)
    {
        runtime.releaseMethod(runtime.context, instances[0], methods[0]);
        assert(runtime.prepareMethod(runtime.context, instances[0], artifact->description().exports[1], methods[0]) ==
               EScriptBackendResult::SUCCESS);
        std::array<std::int32_t, 64U> values;
        std::iota(values.begin(), values.end(), 0);
        std::array<lux_script_value_slot, 64U> slots;
        for (std::size_t index{}; index < slots.size(); ++index)
            slots[index] = {
                LUX_SCRIPT_VK_INT32, {}, sizeof(std::int32_t), lux::semantic::typeId("lux.i32"), &values[index]};
        std::int32_t result{};
        lux_script_value_slot output{
            LUX_SCRIPT_VK_INT32, {}, sizeof(result), lux::semantic::typeId("lux.i32"), &result};
        lux_script_call_frame frame{slots.data(), 64U, 0U, &output, 1U, 0U, nullptr, methods[0].synchronous.context};
        assert(methods[0].synchronous.invoke(&frame) == 0 && result == 2016);
        slots[0].type_id = lux::semantic::typeId("lux.f32");
        assert(methods[0].synchronous.invoke(&frame) != 0);
    }
    for (std::size_t index{}; index < population; ++index)
    {
        runtime.releaseMethod(runtime.context, instances[index], methods[index]);
        runtime.destroyInstance(runtime.context, instances[index]);
    }
    assert(backend->stats().active_prepared_methods == 0U);
    std::printf("prepared=%zu rare_arity=%u bytes=%zu generated_calls=%llu\n", population, include_wide ? 64U : 0U,
                stats.prepared_method_storage_bytes,
                static_cast<unsigned long long>(include_wide ? lux::simulation::test::wide_shape::calls
                                                             : lux::simulation::test::narrow_shape::calls));
    return stats.prepared_method_storage_bytes;
}
} // namespace

int main(int argc, char **)
{
    const auto population = argc > 1 ? 50000U : 10000U;
    const auto narrow = run(population, false);
    const auto wide = run(population, true);
    assert(narrow == wide);
}
