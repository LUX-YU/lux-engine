#pragma once

#include <lux/engine/ecs/Schedule.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace lux::ecs::detail
{
    /// Immutable metadata snapshot consumed by the schedule topology analyser.
    ///
    /// The spans are borrowed only for the duration of analyzeScheduleTopology().
    /// Keeping lifecycle owners and Schedule slots out of this view makes the
    /// analyser reusable by both a live Schedule and an unpublished assembly.
    struct ScheduleTopologyNodeView
    {
        SystemType                 type{};
        int                        phase{kPhaseSimulation};
        std::size_t                sequence{0};
        std::span<const SystemType> prerequisites{};
        std::span<const SystemType> runs_after{};
        std::span<const SystemType> runs_before{};
        ISystem::AccessDeclaration access{};
    };

    struct ScheduleTopologyAnalysis
    {
        Schedule::SortReport                  report;
        std::vector<std::size_t>               order;
        std::vector<Schedule::ExecutionBatch>  batches;
    };

    /// Analyse one immutable topology snapshot without mutating its owner.
    ///
    /// Missing prerequisites and cycles make report.valid() false. Missing
    /// before/after targets remain diagnostics only: their edges are inactive.
    /// order and batches are produced even for invalid input so Schedule keeps
    /// its deterministic fail-visible fallback behaviour.
    ///
    /// This is a cold, small-graph analyser (current schedules contain tens of
    /// nodes). It deliberately uses deterministic linear lookup/dedup/ready
    /// selection instead of maintaining another hash index; no O(V+E)
    /// complexity claim is made for the complete routine.
    [[nodiscard]] ScheduleTopologyAnalysis analyzeScheduleTopology(std::span<const ScheduleTopologyNodeView> nodes);

} // namespace lux::ecs::detail
