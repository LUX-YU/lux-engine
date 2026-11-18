#include <algorithm>
#include <limits>
#include <lux/engine/render/runtime/JobRelations.hpp>

namespace lux::render::detail
{
    namespace
    {
        constexpr std::size_t missing = std::numeric_limits<std::size_t>::max();

        std::size_t find(std::span<const JobRelationEntry> jobs, RenderJobId id) noexcept
        {
            for (std::size_t i = 0; i < jobs.size(); ++i)
            {
                if (jobs[i].id == id)
                {
                    return i;
                }
            }
            return missing;
        }

        bool contains(std::span<const RenderJobId> ids, RenderJobId id) noexcept
        {
            return std::ranges::find(ids, id) != ids.end();
        }

        RenderResult<void> validateEntries(std::span<const JobRelationEntry> jobs) noexcept
        {
            for (std::size_t i = 0; i < jobs.size(); ++i)
            {
                const auto& entry = jobs[i];
                const bool is_invalid_entry = entry.id.isNull() || !entry.device || !entry.relations;
                if (is_invalid_entry)
                {
                    return cxx::unexpected(RenderError{kJobIdentity});
                }
                for (std::size_t previous = 0; previous < i; ++previous)
                {
                    const bool is_duplicate_entry =
                        jobs[previous].id == entry.id || jobs[previous].relations == entry.relations;
                    if (is_duplicate_entry)
                    {
                        return cxx::unexpected(RenderError{kJobIdentity});
                    }
                }
            }
            return {};
        }

        RenderResult<void>
        qualify(std::span<const JobRelationEntry> jobs, std::size_t changed, std::span<const RenderJobId> ids) noexcept
        {
            for (std::size_t i = 0; i < ids.size(); ++i)
            {
                const auto found = find(jobs, ids[i]);
                if (found == missing)
                {
                    return cxx::unexpected(RenderError{kJobIdentity});
                }
                if (found == changed)
                {
                    return cxx::unexpected(RenderError{kJobCycle});
                }
                if (jobs[found].device != jobs[changed].device)
                {
                    return cxx::unexpected(RenderError{kJobState});
                }
                if (contains(ids.first(i), ids[i]))
                {
                    return cxx::unexpected(RenderError{kJobState});
                }
            }
            return {};
        }
    } // namespace

    RenderResult<std::vector<RenderJobId>>
    orderRunningJobs(std::span<const JobRelationEntry> jobs, std::span<const RenderJobId> running) noexcept
    {
        if (auto valid = validateEntries(jobs); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        // Full logical ordering preserves A->B->C even when B is stopped.
        // Tie-breaking prefers the old running order, then the stable table view.
        std::vector<std::size_t> remaining(jobs.size()), priority(jobs.size());
        std::vector<bool> emitted(jobs.size(), false);
        for (std::size_t i = 0; i < jobs.size(); ++i)
        {
            if (auto valid = qualify(jobs, i, jobs[i].relations->predecessors); !valid)
            {
                return cxx::unexpected(valid.error());
            }
            if (auto valid = qualify(jobs, i, jobs[i].relations->successors); !valid)
            {
                return cxx::unexpected(valid.error());
            }
            remaining[i] = jobs[i].relations->predecessors.size();
            priority[i] = running.size() + i;
            for (const auto pred : jobs[i].relations->predecessors)
            {
                const auto p = find(jobs, pred);
                const bool is_missing_reverse_edge = !contains(jobs[p].relations->successors, jobs[i].id);
                if (is_missing_reverse_edge)
                {
                    return cxx::unexpected(RenderError{kJobIdentity});
                }
            }
            for (const auto succ : jobs[i].relations->successors)
            {
                const auto s = find(jobs, succ);
                const bool is_missing_reverse_edge = !contains(jobs[s].relations->predecessors, jobs[i].id);
                if (is_missing_reverse_edge)
                {
                    return cxx::unexpected(RenderError{kJobIdentity});
                }
            }
        }
        for (std::size_t i = 0; i < running.size(); ++i)
        {
            const auto p = find(jobs, running[i]);
            const bool is_invalid_running_job = p == missing || contains(running.first(i), running[i]);
            if (is_invalid_running_job)
            {
                return cxx::unexpected(RenderError{kJobIdentity});
            }
            priority[p] = i;
        }
        std::vector<RenderJobId> result;
        result.reserve(running.size());
        for (std::size_t step = 0; step < jobs.size(); ++step)
        {
            std::size_t picked = missing;
            for (std::size_t i = 0; i < jobs.size(); ++i)
            {
                const bool is_ready = !emitted[i] && remaining[i] == 0;
                const bool has_priority = picked == missing || priority[i] < priority[picked];
                if (is_ready && has_priority)
                {
                    picked = i;
                }
            }
            if (picked == missing)
            {
                return cxx::unexpected(RenderError{kJobCycle});
            }
            emitted[picked] = true;
            if (contains(running, jobs[picked].id))
            {
                result.push_back(jobs[picked].id);
            }
            for (const auto succ : jobs[picked].relations->successors)
            {
                const auto s = find(jobs, succ);
                if (remaining[s] == 0)
                {
                    return cxx::unexpected(RenderError{kJobState});
                }
                --remaining[s];
            }
        }
        return result;
    }

    RenderResult<void>
    setJobDependencies(
        std::span<const JobRelationEntry> jobs,
        RenderJobId changed,
        std::span<const RenderJobId> in,
        std::span<const RenderJobId> out,
        std::vector<RenderJobId>& running
    ) noexcept
    {
        if (auto valid = validateEntries(jobs); !valid)
        {
            return valid;
        }
        const auto position = find(jobs, changed);
        if (position == missing)
        {
            return cxx::unexpected(RenderError{kJobIdentity});
        }
        if (auto valid = qualify(jobs, position, in); !valid)
        {
            return valid;
        }
        if (auto valid = qualify(jobs, position, out); !valid)
        {
            return valid;
        }
        std::vector<JobRelations> candidate;
        candidate.reserve(jobs.size());
        for (const auto& entry : jobs)
        {
            candidate.push_back(*entry.relations);
            std::erase(candidate.back().predecessors, changed);
            std::erase(candidate.back().successors, changed);
        }
        candidate[position].predecessors.assign(in.begin(), in.end());
        candidate[position].successors.assign(out.begin(), out.end());
        for (auto id : in)
        {
            candidate[find(jobs, id)].successors.push_back(changed);
        }
        for (auto id : out)
        {
            candidate[find(jobs, id)].predecessors.push_back(changed);
        }
        std::vector<JobRelationEntry> view;
        view.reserve(jobs.size());
        for (std::size_t i = 0; i < jobs.size(); ++i)
        {
            view.push_back({jobs[i].id, jobs[i].device, &candidate[i]});
        }
        auto sorted = orderRunningJobs(view, running);
        if (!sorted)
        {
            return cxx::unexpected(sorted.error());
        }
        for (std::size_t i = 0; i < jobs.size(); ++i)
        {
            jobs[i].relations->predecessors.swap(candidate[i].predecessors);
            jobs[i].relations->successors.swap(candidate[i].successors);
        }
        running.swap(*sorted);
        return {};
    }
} // namespace lux::render::detail
