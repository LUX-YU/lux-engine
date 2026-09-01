#include <lux/engine/system/detail/SystemDependencyOrder.hpp>

#include <algorithm>
#include <new>
#include <queue>
#include <utility>

namespace lux::system::detail
{
    lux::cxx::expected<std::vector<std::size_t>, ESystemDependencyOrderError>
    deterministicSystemOrder(
        std::span<const SystemInstanceId> instances,
        std::span<const SystemDependencyOrdinalEdge> edges
    ) noexcept
    {
        try
        {
            std::vector<std::size_t> indegree(instances.size());
            std::vector<std::vector<std::size_t>> successors(instances.size());
            for (const auto edge : edges)
            {
                const bool is_invalid = edge.before >= instances.size() || edge.after >= instances.size() ||
                    edge.before == edge.after;
                if (is_invalid)
                {
                    return lux::cxx::unexpected(ESystemDependencyOrderError::INVALID_EDGE);
                }
                successors[edge.before].push_back(edge.after);
                ++indegree[edge.after];
            }

            const auto greater_instance = [instances](std::size_t left, std::size_t right) noexcept {
                return instances[left].value > instances[right].value;
            };
            std::priority_queue<std::size_t, std::vector<std::size_t>, decltype(greater_instance)> ready(
                greater_instance
            );
            for (std::size_t ordinal{}; ordinal < instances.size(); ++ordinal)
            {
                if (indegree[ordinal] == 0U)
                {
                    ready.push(ordinal);
                }
            }

            std::vector<std::size_t> result;
            result.reserve(instances.size());
            while (!ready.empty())
            {
                const std::size_t current = ready.top();
                ready.pop();
                result.push_back(current);
                for (const std::size_t successor : successors[current])
                {
                    if (--indegree[successor] == 0U)
                    {
                        ready.push(successor);
                    }
                }
            }
            if (result.size() != instances.size())
            {
                return lux::cxx::unexpected(ESystemDependencyOrderError::CYCLE);
            }
            return result;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(ESystemDependencyOrderError::ALLOCATION_FAILURE);
        }
    }
} // namespace lux::system::detail
