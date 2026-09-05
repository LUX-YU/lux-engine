#include "Physics2DScriptTestSupport.hpp"

#include <lux/engine/function/script/ScriptEventSchema.hpp>

#include <array>
#include <filesystem>

int main(int argc, char** argv)
{
    if (argc != 2)
        return 1;
    const std::array sources{lux::physics2d::test::pulseEventSource()};
    return lux::script::writeScriptEventSchemaManifest(std::filesystem::path{argv[1]}, sources) ? 0 : 2;
}
