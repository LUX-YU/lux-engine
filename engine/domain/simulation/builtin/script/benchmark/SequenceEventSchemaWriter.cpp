#include "ScriptBenchmarkDomain.hpp"
#include <lux/engine/simulation/scripting/ScriptEventSource.hpp>
#include <lux/engine/function/script/ScriptEventSchema.hpp>

int main(int argc, char** argv)
{
    if (argc != 2) return 1;
    const auto description = lux::simulation::benchmark_domain::scriptDescription();
    auto source = lux::simulation::script::describeScriptEventSource<std::int32_t>(
        description.findEvent(lux::simulation::benchmark_domain::kSystem, lux::simulation::benchmark_domain::kEvent),
        "Benchmark", "event"
    );
    if (!source) return 2;
    return lux::script::writeScriptEventSchemaManifest(argv[1], std::span{&*source, 1U}) ? 0 : 3;
}
