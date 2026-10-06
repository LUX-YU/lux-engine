#pragma once
#include <lux/engine/editor/workspace/DockLayout.hpp>
#include <lux/engine/ui/Docking.hpp>
#include <map>

namespace lux::editor::workbench::detail
{
    // The caller supplies a validated tree and one resolved window for every referenced slot.
    inline lux::ui::DockTree makeDockTree(
        const workspace::DockTree& source,
        const std::map<std::uint32_t, std::string>& windows
    )
    {
        lux::ui::DockTree docking;
        std::map<std::uint32_t, std::uint32_t> indices;
        for (const auto& node : source.nodes)
        {
            indices.emplace(node.id, static_cast<std::uint32_t>(docking.nodes.size()));
            docking.nodes.emplace_back();
        }
        for (const auto& node : source.nodes)
        {
            auto& output = docking.nodes[indices.at(node.id)];
            if (node.split != workspace::EDockSplit::LEAF)
            {
                output.split = node.split == workspace::EDockSplit::HORIZONTAL ? lux::ui::EDockSplit::HORIZONTAL
                                                                               : lux::ui::EDockSplit::VERTICAL;
                output.first = indices.at(node.first);
                output.second = indices.at(node.second);
                output.ratio = static_cast<float>(node.ratio);
            }
            for (auto slot : node.slots)
                output.windows.push_back(windows.at(slot.value));
        }
        for (const auto& root : source.roots)
            docking.surfaces.push_back(
                {indices.at(root.node),
                 {{static_cast<float>(root.x), static_cast<float>(root.y)},
                  {static_cast<float>(root.width), static_cast<float>(root.height)}},
                 root.floating}
            );
        return docking;
    }
    // Pane names are local UI identities; persistent slots are allocated by the capture caller.
    // Windows outside that caller's restorable set never become implicit content-recovery entries.
    inline workspace::DockTree captureDockTree(
        const lux::ui::DockTree& source,
        const std::map<std::string, workspace::LayoutSlotId>& windows
    )
    {
        workspace::DockTree docking;
        docking.nodes.reserve(source.nodes.size());
        for (std::size_t i{}; i < source.nodes.size(); ++i)
        {
            const auto& node = source.nodes[i];
            workspace::DockNode output;
            output.id = static_cast<std::uint32_t>(i + 1);
            output.split = static_cast<workspace::EDockSplit>(node.split);
            output.ratio = node.ratio;
            if (node.split != lux::ui::EDockSplit::LEAF)
            {
                output.first = node.first + 1;
                output.second = node.second + 1;
            }
            for (const auto& window : node.windows)
            {
                if (auto found = windows.find(window); found != windows.end())
                    output.slots.push_back(found->second);
            }
            docking.nodes.push_back(std::move(output));
        }
        docking.roots.reserve(source.surfaces.size());
        for (const auto& surface : source.surfaces)
        {
            docking.roots.push_back({
                surface.node + 1,
                surface.bounds.position.x,
                surface.bounds.position.y,
                surface.bounds.size.width,
                surface.bounds.size.height,
                surface.floating
            });
        }
        return docking;
    }

}
