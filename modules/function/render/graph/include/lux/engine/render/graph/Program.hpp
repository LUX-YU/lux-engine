#pragma once

#include <lux/engine/render/data/Shader.hpp>
#include <array>
#include <string>

namespace lux::render
{
    struct ProgramResourceBinding final
    {
        std::string shader_name;
        std::string resource_key;
    };

    // Cold compiler input, one binding per live Pass. It is the only Shader binding authority.
    // Feature registration uses a local name; assembly's caller expands it with featureKey().
    struct PassProgram final
    {
        std::string pass_key;
        SpirvArtifact shader; // empty for transfer-only commands
        std::array<float, 4> clear_color{0, 0, 0, 1};
        std::uint32_t vertex_count = 3;
        std::vector<ProgramResourceBinding> resources;
    };

}
