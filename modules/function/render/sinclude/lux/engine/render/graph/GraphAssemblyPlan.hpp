#pragma once

#include <cstdint>
#include <lux/engine/description/ShaderBlock.hpp>
#include <string>
#include <vector>

namespace lux::render::detail
{
    struct ParameterUse final
    {
        std::uint32_t resource;
        std::string source_field;
        rdesc::ShaderMember destination;
        std::uint32_t source_offset = 0;
    };

    struct GraphAssemblyPlan final
    {
        std::vector<std::uint32_t> instance_recipes;
        std::vector<std::uint32_t> source_pass_slots;
        std::vector<std::vector<ParameterUse>> parameter_links;
        std::vector<rdesc::ShaderBlock> parameter_layouts;
    };
} // namespace lux::render::detail
