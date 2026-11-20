#pragma once

#include <cstdint>
#include <vector>

namespace lux::render::detail
{
    struct GraphAssemblyPlan final
    {
        std::vector<std::uint32_t> source_pass_slots;
    };
} // namespace lux::render::detail
