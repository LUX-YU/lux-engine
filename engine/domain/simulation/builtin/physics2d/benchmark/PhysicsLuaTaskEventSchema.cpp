#include "Physics2DScriptTestSupport.hpp"
#include <lux/engine/function/script/ScriptEventSchema.hpp>
int main(int argc, char **argv)
{
    if (argc != 2)
        return 1;
    const auto source = lux::physics2d::test::pulseEventSource();
    return lux::script::writeScriptEventSchemaManifest(argv[1], std::span{&source, 1U}) ? 0 : 2;
}
