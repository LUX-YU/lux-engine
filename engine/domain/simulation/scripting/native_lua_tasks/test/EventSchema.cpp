#include "TaskDomain.hpp"
#include <lux/engine/function/script/ScriptEventSchema.hpp>
#include <lux/engine/simulation/scripting/ScriptEventSource.hpp>
int main(int argc, char **argv)
{
    if (argc != 2)
        return 1;
    using namespace lux::simulation;
    const auto simulation = na1::domain();
    auto source =
        script::describeScriptEventSource<std::int32_t>(simulation.findEvent(na1::System, na1::Event), "Task", "event");
    if (!source)
        return 2;
    return lux::script::writeScriptEventSchemaManifest(argv[1], std::span{&*source, 1U}) ? 0 : 3;
}
