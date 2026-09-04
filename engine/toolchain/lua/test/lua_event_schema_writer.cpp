#include <lux/engine/toolchain/lua/ScriptEventSchema.hpp>

#include <lux/engine/core/semantic/SemanticType.hpp>

#include <array>
#include <filesystem>
#include <string_view>

namespace
{
    [[nodiscard]] lux::script::ScriptEventSourceDescription source(std::string_view catalog)
    {
        constexpr auto payload = lux::script::ScriptEventPayloadDescription{
            "lux.i32",
            lux::semantic::typeId("lux.i32"),
            static_cast<std::uint8_t>(lux::semantic::EAbiKind::I32),
            4U,
            4U
        };
        if (catalog == "gameplay")
        {
            return {
                "Gameplay",
                "damage",
                0x4C554101U,
                0x4C554108U,
                lux::script::EScriptEventRoute::SIMULATION_BROADCAST,
                payload,
                lux::semantic::typeId("lux.i32"),
                1U
            };
        }
        if (catalog == "benchmark")
        {
            return {
                "Benchmark",
                "event",
                0xB001U,
                0xB009U,
                lux::script::EScriptEventRoute::SIMULATION_BROADCAST,
                payload,
                lux::semantic::typeId("lux.i32"),
                1U
            };
        }
        return {};
    }
}

int main(int argc, char** argv)
{
    if (argc != 3)
        return 1;
    const std::array sources{source(argv[2])};
    return sources.front().valid() &&
        lux::toolchain::lua::writeScriptEventSchemaManifest(std::filesystem::path{argv[1]}, sources)
        ? 0
        : 1;
}
