#include "ScriptBenchmarkDomain.hpp"
#include <lux/engine/simulation/scripting/ScriptEventSource.hpp>
#include <lux/engine/function/script/ScriptEventSchema.hpp>

int main(int argc, char** argv)
{
    if (argc != 2) return 1;
    using namespace lux::simulation;
    const auto description = benchmark_domain::scriptDescription(63U);
    std::vector<lux::script::ScriptEventSourceDescription> sources;
    const auto append = [&](EventPointId event, const std::string& name) {
        auto source = script::describeScriptEventSource<std::int32_t>(
            description.findEvent(benchmark_domain::kSystem, event), "Benchmark", name);
        if (!source) return false;
        sources.push_back(std::move(*source));
        return true;
    };
    for (std::size_t index{}; index < 63U; ++index)
        if (!append(EventPointId{0xA000U + index}, "extra" + std::to_string(index))) return 2;
    if (!append(benchmark_domain::kEvent, "event") || !append(benchmark_domain::kTargetEvent, "target_event")) return 2;
    return lux::script::writeScriptEventSchemaManifest(argv[1], sources) ? 0 : 3;
}
