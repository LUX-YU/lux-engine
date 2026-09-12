#include "../test/TaskDomain.hpp"
#include <lux/engine/function/script/ScriptEventSchema.hpp>
#include <lux/engine/simulation/scripting/ScriptEventSource.hpp>
int main(int argc, char **argv)
{
    if (argc != 2)
        return 1;
    const auto domain = lux::simulation::na1::domain();
    auto source = lux::simulation::script::describeScriptEventSource<std::int32_t>(
        domain.findEvent(lux::simulation::na1::System, lux::simulation::na1::Event), "Benchmark", "event");
    if (!source)
        return 2;
    return lux::script::writeScriptEventSchemaManifest(argv[1], std::span{&*source, 1U}) ? 0 : 3;
}
