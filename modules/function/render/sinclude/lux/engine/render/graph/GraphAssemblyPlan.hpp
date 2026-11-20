#pragma once

#include <cstdint>
#include <lux/engine/render/extensions/graph/RenderGraph.hpp>
#include <vector>

namespace lux::render::detail
{
    struct CompiledPassSlot final
    {
        std::uint32_t index = kGraphInvalidIndex;

        [[nodiscard]] bool valid() const noexcept { return index != kGraphInvalidIndex; }
        [[nodiscard]] bool operator==(const CompiledPassSlot&) const noexcept = default;
    };

    struct GraphAssemblyPlan final
    {
        std::vector<CompiledPassSlot> source_pass_slots;
    };
} // namespace lux::render::detail
