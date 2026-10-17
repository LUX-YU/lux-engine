#pragma once
#include <lux/engine/render/graph/Program.hpp>

namespace parameter_fixture
{
    inline lux::render::ShaderBlock float4(std::string_view field) noexcept
    {
        const lux::rdesc::DataGpuField value{field, lux::rdesc::EDataScalar::FLOAT, 0, 4, 1, 0, 0};
        return lux::render::ShaderBlock::from({lux::rdesc::EDataLayout::STD430, 16, 16, {&value, 1}, {}, {}, 0});
    }

    inline void configure(lux::render::PassProgram& program, std::string_view field,
        std::string resource = {}) noexcept
    {
        using namespace lux::render;
        program.shader.interface.parameters = float4(field);
        program.shader.interface.parameter_stages = 2;
        program.parameter_values.resize(16);
        const bool context = resource.empty();
        if (context) program.context_parameters = float4(field);
        program.parameters = {{std::string(field), std::move(resource), std::string(field),
            context ? EParameterSource::CONTEXT : EParameterSource::STORAGE}};
    }
}
