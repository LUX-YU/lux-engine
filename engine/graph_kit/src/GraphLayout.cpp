#include <lux/engine/graph_kit/GraphLayout.hpp>

#include <vector>

namespace lux::graphkit
{
    std::size_t layoutUnplacedNodes(IGraphView& view, GraphVec2 origin, GraphVec2 cell,
                                    std::uint32_t columns)
    {
        if (columns == 0)
        {
            columns = 1;
        }

        // Collect first (stable forEachNode order), assign after — don't
        // mutate while the view iterates its own container.
        std::vector<GraphNodeRef> unplaced;
        view.forEachNode(
            [&](GraphNodeRef node, std::string_view)
            {
                if (!view.nodePos(node).has_value())
                {
                    unplaced.push_back(node);
                }
            }
        );

        for (std::size_t i = 0; i < unplaced.size(); ++i)
        {
            const auto col = static_cast<std::uint32_t>(i % columns);
            const auto row = static_cast<std::uint32_t>(i / columns);
            view.setNodePos(
                unplaced[i],
                GraphVec2{ origin.x + cell.x * static_cast<float>(col),
                           origin.y + cell.y * static_cast<float>(row) }
            );
        }
        return unplaced.size();
    }

} // namespace lux::graphkit
