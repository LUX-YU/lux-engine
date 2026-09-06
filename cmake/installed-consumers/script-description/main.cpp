#include <lux/engine/scene/script/ScriptSystemDescriptionCodec.hpp>
#include <cassert>

int main()
{
    using namespace lux::scene::script;
    lux::simulation::SimulationDescriptionBuilder simulation_builder;
    auto simulation = std::move(simulation_builder).build();
    assert(simulation);
    ScriptSystemDescriptionBuilder builder;
    auto description = std::move(builder).build(*simulation);
    assert(description);
    constexpr ScriptSystemCodecLimits limits{4096U, 4096U, 4096U};
    const auto bytes = encodeScriptSystemDescription(*description, limits);
    assert(bytes && (*bytes)[4] == std::byte{1U});
    const auto decoded = decodeScriptSystemDescription(*bytes, *simulation, limits);
    assert(decoded && decoded->mounts().empty());
    const auto encoded = encodeScriptSystemDescription(*decoded, limits);
    assert(encoded && *encoded == *bytes);
}
