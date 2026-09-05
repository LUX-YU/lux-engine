#include <lux/engine/toolchain/lua/ScriptEventSchema.hpp>

#include <lux/engine/core/semantic/SemanticType.hpp>

#include <array>
#include <filesystem>

int main(int argc, char** argv)
{
    if (argc != 2)
        return 1;
    const std::array sources{
        lux::script::ScriptEventSourceDescription{
            "Inventory",
            "changed",
            51U,
            52U,
            lux::script::EScriptEventRoute::SIMULATION_BROADCAST,
            {
                "lux.i32",
                lux::semantic::typeId("lux.i32"),
                static_cast<std::uint8_t>(lux::semantic::EAbiKind::I32),
                4U,
                4U
            },
            lux::semantic::typeId("lux.i32"),
            1U, 53U, 54U, 1U
        }
    };
    return lux::toolchain::lua::writeScriptEventSchemaManifest(std::filesystem::path{argv[1]}, sources) ? 0 : 1;
}
