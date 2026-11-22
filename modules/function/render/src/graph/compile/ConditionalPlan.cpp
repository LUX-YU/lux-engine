#include <lux/engine/render/extensions/graph/RenderGraph.hpp>
#include <lux/engine/render/graph/compile/ConditionalPlan.hpp>

namespace lux::render::detail
{
    void planConditions(
        const RenderGraph& graph, std::span<const GraphPassId> order, std::span<const CompiledResource> resources,
        RecordingProgram& recording
    ) noexcept
    {
        for (std::uint32_t p = 0; p < recording.passes.size(); ++p)
        {
            auto& pass = recording.passes[p];
            const auto& condition = graph.passes()[order[p].index].condition;
            if (!condition)
            {
                continue;
            }
            const auto input_slot = condition->source == EConditionSource::HISTORY_VALID
                ? resources[condition->input].history : condition->input;
            const bool contiguous =
                !recording.conditions.empty() &&
                recording.conditions.back().first_pass + recording.conditions.back().pass_count == p;
            const bool same_predicate = contiguous &&
                                        recording.conditions.back().source == condition->source &&
                                        recording.conditions.back().input_slot == input_slot &&
                                        recording.conditions.back().expected == condition->expected;
            if (same_predicate)
            {
                ++recording.conditions.back().pass_count;
            }
            else
            {
                recording.conditions.push_back({p, 1, input_slot, condition->source, condition->expected});
            }
            pass.condition_region = static_cast<std::uint32_t>(recording.conditions.size() - 1);
        }
    }
} // namespace lux::render::detail
