#include "ConsumerDomain.hpp"
#include <lux/engine/simulation/scripting/ScriptEventSource.hpp>
#include <lux/engine/function/script/ScriptEventSchema.hpp>

int main(int argc, char** argv)
{
    if (argc != 2) return 1;
    auto description = installed_consumer::makeDescription();
    if (!description) return 2;
    auto source = lux::simulation::script::describeScriptEventSource<std::int32_t>(
        description->findEvent(installed_consumer::ProbeId, installed_consumer::PulseEvent), "Gameplay", "pulse");
    if (!source) return 3;
    return lux::script::writeScriptEventSchemaManifest(argv[1], std::span{&*source, 1U}) ? 0 : 4;
}
