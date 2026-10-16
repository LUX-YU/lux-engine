#include "../Scenarios.hpp"
#include <lux/engine/render/features/Registration.hpp>
#include <lux/engine/render/features/Texture.hpp>
#include <cstdlib>

int main(int argc, char** argv)
{
    if (argc != 2) return 1;
    const auto scenario = static_cast<unsigned>(std::atoi(argv[1]));
    auto result = fixture::scenario(scenario);
    if (!result) return 2;
    const auto expected = scenario == 0 ? 5u : 1u;
    if (result->analysis.execution_order.size() != expected) return 3;
    if (result->definition.outputs().empty()) return 4;
    if (scenario == 1 && result->external_bindings.size() != 2) return 5;
    if (scenario == 2 && result->external_bindings.size() != 1) return 6;
    if (!lux::render::validateCommand(lux::render::commandDescriptor<lux::render::SetTexture>())) return 7;
    return 0;
}
