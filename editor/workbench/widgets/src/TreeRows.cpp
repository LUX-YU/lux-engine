#include <lux/engine/editor/widgets/TreeRows.hpp>
namespace lux::editor::widgets
{
    std::vector<TreeRow> makeTreeRows(std::span<const std::size_t> parents)
    {
        const auto none = parents.size();
        std::vector<std::size_t> first(none + 1, none), next(none, none);
        for (auto index = none; index-- > 0;)
        {
            const auto head = parents[index] < none ? parents[index] : none;
            next[index] = first[head];
            first[head] = index;
        }
        std::vector<TreeRow> rows;
        rows.reserve(none);
        std::vector<bool> visited(none);
        std::vector<std::size_t> stack;
        auto source = first[none];
        std::size_t depth{}, orphan{};
        while (rows.size() < none)
        {
            if (source == none)
            {
                if (!stack.empty())
                {
                    source = next[rows[stack.back()].source];
                    rows[stack.back()].end = rows.size();
                    stack.pop_back();
                    --depth;
                    continue;
                }
                while (orphan < none && visited[orphan])
                    ++orphan;
                source = orphan;
                if (source == none)
                    break;
            }
            if (visited[source])
            {
                source = none;
                continue;
            }
            visited[source] = true;
            const auto row = rows.size();
            rows.push_back({source, depth, row + 1});
            if (first[source] != none)
            {
                stack.push_back(row);
                source = first[source];
                ++depth;
            }
            else
                source = next[source];
        }
        for (auto row : stack)
            rows[row].end = rows.size();
        return rows;
    }
}
