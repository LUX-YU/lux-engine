#include <lux/engine/simulation/scripting/cpp_static/CppStaticScriptBridge.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <vector>

namespace
{
    using namespace lux::simulation::script;
    std::size_t erased_calls{};
    void empty() noexcept {}
    template <std::size_t> using Integer = std::int32_t;
    template <class> struct Wide;
    template <std::size_t... Index>
    struct Wide<std::index_sequence<Index...>> final
    {
        static std::int32_t invoke(Integer<Index>... values) noexcept { return (values + ...); }
    };
    using WideFunction = Wide<std::make_index_sequence<64U>>;

    std::size_t run(std::size_t population, bool include_wide)
    {
        lux::meta::RefFunction zero;
        zero.is_noexcept = true;
        zero.invokable.name = "empty";
        zero.invokable.return_type = lux::meta::ref_type_of_v<void>;
        zero.invokable.invoker = [](void*, void**, void*) { ++erased_calls; };
        lux::meta::RefFunction wide;
        wide.is_noexcept = true;
        wide.invokable.name = "wide";
        wide.invokable.return_type = lux::meta::ref_type_of_v<std::int32_t>;
        wide.invokable.invoker = [](void*, void**, void*) { ++erased_calls; };
        for (std::size_t index{}; index < 64U; ++index)
            wide.invokable.parameters.push_back({"value", lux::meta::ref_type_of_v<std::int32_t>,
                lux::cxx::type_name<std::int32_t>(), lux::cxx::type_hash<std::int32_t>()});
        const std::array<const lux::meta::RefFunction*, 2U> functions{&zero, &wide};
        const std::array<lux::script::ScriptSymbolId, 2U> symbols{1U, 2U};
        const std::array typed{
            makeCppStaticMethodExport<&empty>(zero, symbols[0]),
            makeCppStaticMethodExport<&WideFunction::invoke>(wide, symbols[1])
        };
        const std::size_t count = include_wide ? 2U : 1U;
        auto projected = projectCppStaticGlobalScript("shape", "shape", std::span{functions}.first(count),
            std::span{symbols}.first(count), {}, {}, {}, {}, {}, std::span{typed}.first(count));
        assert(projected);
        auto artifact = lux::script::ScriptArtifact::create(projected->description(), {});
        assert(artifact);
        const std::array pools{CppStaticScriptPoolDescription{
            &*projected, population, 0U, 0U, alignof(std::max_align_t), population
        }};
        auto backend = CppStaticScriptBackend::create(pools);
        assert(backend);
        const auto runtime = backend->descriptor();
        std::vector<ScriptBackendInstance> instances(population);
        std::vector<ScriptBackendPreparedMethod> methods(population);
        for (std::size_t index{}; index < population; ++index)
        {
            assert(runtime.createInstance(runtime.context,
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
            assert(runtime.prepareMethod(runtime.context, instances[0], artifact->description().exports[1],
                methods[0]) == EScriptBackendResult::SUCCESS);
            std::array<std::int32_t, 64U> values;
            std::iota(values.begin(), values.end(), 0);
            std::array<lux_script_value_slot, 64U> slots;
            for (std::size_t index{}; index < slots.size(); ++index)
                slots[index] = {LUX_SCRIPT_VK_INT32, {}, sizeof(std::int32_t), lux::semantic::typeId("lux.i32"),
                    &values[index]};
            std::int32_t result{};
            lux_script_value_slot output{LUX_SCRIPT_VK_INT32, {}, sizeof(result), lux::semantic::typeId("lux.i32"),
                &result};
            lux_script_call_frame frame{slots.data(), 64U, 0U, &output, 1U, 0U, nullptr,
                methods[0].synchronous.context};
            assert(methods[0].synchronous.invoke(&frame) == 0 && result == 2016);
            slots[0].type_id = lux::semantic::typeId("lux.f32");
            assert(methods[0].synchronous.invoke(&frame) != 0);
        }
        for (std::size_t index{}; index < population; ++index)
        {
            runtime.releaseMethod(runtime.context, instances[index], methods[index]);
            runtime.destroyInstance(runtime.context, instances[index]);
        }
        assert(backend->stats().active_prepared_methods == 0U && erased_calls == 0U);
        std::printf("prepared=%zu rare_arity=%u bytes=%zu erased_calls=%zu\n", population,
            include_wide ? 64U : 0U, stats.prepared_method_storage_bytes, erased_calls);
        return stats.prepared_method_storage_bytes;
    }
}

int main(int argc, char**)
{
    const auto population = argc > 1 ? 50000U : 10000U;
    const auto narrow = run(population, false);
    const auto wide = run(population, true);
    assert(narrow == wide);
}
