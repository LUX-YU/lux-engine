#pragma once

#include <array>
#include <cstddef>
#include <lux/engine/description/SpirvArtifact.hpp>
#include <lux/engine/render/graph/Draw.hpp>
#include <string>

namespace lux::render
{
    struct ProgramResourceBinding final
    {
        std::string shader_name;
        std::string resource_key;
    };

    enum class EParameterSource : std::uint8_t
    {
        STORAGE,
        CONTEXT
    };

    struct ProgramParameterBinding final
    {
        std::string shader_field;
        std::string resource_key;
        std::string source_field;
        EParameterSource source = EParameterSource::STORAGE;
    };

    // Cold compiler input, one binding per live Pass. It is the only Shader binding authority.
    // Feature registration uses a local name; assembly's caller expands it with featureKey().
    struct PassProgram final
    {
        std::string pass_key;
        rdesc::SpirvArtifact shader; // empty for transfer-only commands
        std::array<float, 4> clear_color{0, 0, 0, 1};
        VDrawCommand draw = DrawVertices{};
        GraphicsState graphics;
        float clear_depth = 1;
        std::vector<ProgramVertexBuffer> vertex_buffers;
        std::vector<ProgramResourceBinding> resources;
        std::array<std::uint32_t, 3> dispatch{1, 1, 1}; // Actual Compute workgroup count.
        std::vector<std::byte> parameter_values; // Complete initial Shader ABI bytes, owned by the compiled result.
        rdesc::ShaderBlock context_parameters;   // Declared frame input ABI, shared by this Graph's Context.
        std::vector<ProgramParameterBinding> parameters; // Named Storage/Context fields override initial bytes.
    };

} // namespace lux::render
