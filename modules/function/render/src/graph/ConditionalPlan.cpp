#include <lux/engine/render/graph/ConditionalPlan.hpp>

namespace lux::render::detail
{
    void planConditions(const RenderGraph& graph, RecordingProgram& recording) noexcept
    {
        for (std::uint32_t p = 0; p < recording.passes.size(); ++p)
        {
            auto& pass = recording.passes[p];
            const auto& condition = graph.passes()[pass.source_pass].condition;
            if (!condition)
            {
                continue;
            }
            const bool contiguous =
                !recording.conditions.empty() &&
                recording.conditions.back().first_pass + recording.conditions.back().pass_count == p;
            const bool same_predicate = contiguous &&
                                        recording.conditions.back().predicate.source == condition->source &&
                                        recording.conditions.back().predicate.input == condition->input &&
                                        recording.conditions.back().predicate.expected == condition->expected;
            if (same_predicate)
            {
                ++recording.conditions.back().pass_count;
            }
            else
            {
                recording.conditions.push_back({*condition, p, 1});
            }
            pass.condition_region = static_cast<std::uint32_t>(recording.conditions.size() - 1);
        }
    }
} // namespace lux::render::detail
