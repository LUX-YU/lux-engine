#include <lux/engine/ui/detail/RootImpl.hpp>

namespace lux::ui
{
    void Root::prepareLayout() noexcept
    {
        if (impl_->dock_state.enabled)
        {
            const auto* viewport = ImGui::GetMainViewport();
            const ImGuiID root = ImGui::GetID("lux.ui.dockspace");
            if (impl_->dock_state.pending)
            {
                auto prepared = std::move(impl_->dock_state.pending);
                const auto& tree = *prepared;
                ImGui::DockBuilderRemoveNode(root);
                // Unlisted windows keep their owners and position. Removing the replaced DockSpace
                // only undocks them; the new tree never infers additional content or view creation.
                for (const auto& surface : tree.surfaces)
                {
                    auto flags = surface.floating ? ImGuiDockNodeFlags_None : ImGuiDockNodeFlags_DockSpace;
                    const auto id = ImGui::DockBuilderAddNode(surface.floating ? 0 : root, flags);
                    prepared->ids[surface.node] = id;
                    const auto position = surface.floating
                                              ? ImVec2{surface.bounds.position.x, surface.bounds.position.y}
                                              : viewport->WorkPos;
                    const auto size = surface.floating ? ImVec2{surface.bounds.size.width, surface.bounds.size.height}
                                                       : viewport->WorkSize;
                    ImGui::DockBuilderSetNodePos(id, position);
                    ImGui::DockBuilderSetNodeSize(id, size);
                }
                for (auto index : prepared->order)
                {
                    const auto& node = tree.nodes[index];
                    const auto id = prepared->ids[index];
                    if (node.split != EDockSplit::LEAF)
                    {
                        ImGui::DockBuilderSplitNode(
                            id,
                            node.split == EDockSplit::HORIZONTAL ? ImGuiDir_Left : ImGuiDir_Up,
                            node.ratio,
                            &prepared->ids[node.first],
                            &prepared->ids[node.second]
                        );
                    }
                    else
                    {
                        for (const auto& pane_id : node.panes)
                        {
                            if (auto* pane = findPane(pane_id))
                            {
                                ImGui::DockBuilderDockWindow(pane->imgui_label_.c_str(), id);
                            }
                        }
                    }
                }
                for (const auto& surface : tree.surfaces)
                {
                    ImGui::DockBuilderFinish(prepared->ids[surface.node]);
                }
            }
            ImGui::DockSpaceOverViewport(root, viewport);
        }
    }

    cxx::expected<void, EDockError> Root::setDockTree(DockTree tree) noexcept
    {
        if (!isOnAffinityThread())
        {
            return cxx::unexpected(EDockError::WRONG_THREAD);
        }
        if (!checkStructureSafe())
        {
            return cxx::unexpected(EDockError::BUSY);
        }
        if (!impl_->dock_state.enabled)
        {
            return cxx::unexpected(EDockError::DISABLED);
        }
        auto invalid = [] { return cxx::unexpected(EDockError::INVALID_DATA); };
        if (tree.nodes.size() > impl_->pane_capacity || tree.surfaces.size() > impl_->pane_capacity)
        {
            return invalid();
        }
        auto prepared = std::make_unique<DockData>();
        prepared->order.reserve(tree.nodes.size());
        prepared->ids.resize(tree.nodes.size());
        std::vector<std::uint8_t> visited(tree.nodes.size());
        std::vector<std::uint32_t> pending;
        pending.reserve(tree.nodes.size());
        std::unordered_set<Pane*> panes;
        unsigned main_surfaces{};
        for (const auto& surface : tree.surfaces)
        {
            const auto& bounds = surface.bounds;
            const bool valid_geometry = std::isfinite(bounds.position.x) && std::isfinite(bounds.position.y) &&
                                        std::isfinite(bounds.size.width) && std::isfinite(bounds.size.height) &&
                                        bounds.size.width > 0 && bounds.size.height > 0;
            if (!valid_geometry || (!surface.floating && ++main_surfaces > 1))
            {
                return invalid();
            }
            pending.push_back(surface.node);
            while (!pending.empty())
            {
                const auto index = pending.back();
                pending.pop_back();
                if (index >= tree.nodes.size() || visited[index])
                {
                    return invalid();
                }
                visited[index] = 1;
                prepared->order.push_back(index);
                const auto& node = tree.nodes[index];
                if (node.split == EDockSplit::LEAF)
                {
                    if (node.first != UINT32_MAX || node.second != UINT32_MAX)
                    {
                        return invalid();
                    }
                    for (auto* pane : node.panes)
                    {
                        if (!pane || pane->root_ != this || pane->modal_ || !panes.insert(pane).second)
                        {
                            return invalid();
                        }
                    }
                }
                else
                {
                    const bool invalid_split =
                        node.split != EDockSplit::HORIZONTAL && node.split != EDockSplit::VERTICAL;
                    const bool invalid_ratio = !std::isfinite(node.ratio) || node.ratio <= 0 || node.ratio >= 1;
                    if (invalid_split || invalid_ratio || !node.panes.empty())
                    {
                        return invalid();
                    }
                    pending.push_back(node.second);
                    pending.push_back(node.first);
                }
            }
        }
        if (prepared->order.size() != tree.nodes.size())
        {
            return invalid();
        }
        prepared->nodes.reserve(tree.nodes.size());
        for (const auto& node : tree.nodes)
        {
            auto& retained =
                prepared->nodes.emplace_back(DockData::Node{node.split, node.first, node.second, node.ratio});
            retained.panes.reserve(node.panes.size());
            for (auto* pane : node.panes)
            {
                retained.panes.push_back(pane->id_);
            }
        }
        prepared->surfaces = std::move(tree.surfaces);
        impl_->dock_state.pending = std::move(prepared);
        return {};
    }
    DockTree Root::captureDockTree() const
    {
        requireOwner();
        detail::ContextActivation context{impl_->context->native()};
        if (impl_->dock_state.pending)
        {
            DockTree result;
            result.surfaces = impl_->dock_state.pending->surfaces;
            for (const auto& node : impl_->dock_state.pending->nodes)
            {
                auto& output = result.nodes.emplace_back(DockNode{node.split, node.first, node.second, node.ratio});
                for (const auto id : node.panes)
                {
                    output.panes.push_back(findPane(id));
                }
            }
            std::unordered_set<PaneId> included;
            for (const auto& node : impl_->dock_state.pending->nodes)
            {
                for (const auto& name : node.panes)
                {
                    included.insert(name);
                }
            }
            // Applying a replacement DockSpace undocks unspecified windows, without destroying
            // or hiding them. Capturing before its first draw must retain those same windows.
            for (const auto& pane_owner : impl_->panes.values())
            {
                auto* pane = pane_owner.get();

                if (pane->modal_ || included.contains(pane->id_))
                {
                    continue;
                }
                const auto* window = ImGui::FindWindowByName(pane->imgui_label_.c_str());
                const Rect bounds = window ? Rect{{window->Pos.x, window->Pos.y}, {window->Size.x, window->Size.y}}
                                           : Rect{{40, 40}, {640, 480}};
                const auto index = static_cast<std::uint32_t>(result.nodes.size());
                result.nodes.push_back({EDockSplit::LEAF, UINT32_MAX, UINT32_MAX, .5F, {pane}});
                result.surfaces.push_back({index, bounds, true});
            }
            return result;
        }
        DockTree result;
        std::map<const ImGuiDockNode*, std::uint32_t> nodes;
        std::vector<const ImGuiDockNode*> pending;
        const auto insert = [&](const ImGuiDockNode* node)
        {
            auto [it, fresh] = nodes.emplace(node, static_cast<std::uint32_t>(result.nodes.size()));
            if (fresh)
            {
                result.nodes.emplace_back();
                pending.push_back(node);
            }
            return it->second;
        };
        for (const auto& pane_owner : impl_->panes.values())
        {
            auto* pane = pane_owner.get();

            if (!pane || pane->modal_)
            {
                continue;
            }
            const auto* window = ImGui::FindWindowByName(pane->imgui_label_.c_str());
            if (!window || !window->DockNode)
            {
                const auto index = static_cast<std::uint32_t>(result.nodes.size());
                result.nodes.push_back({EDockSplit::LEAF, UINT32_MAX, UINT32_MAX, .5F, {pane}});
                const Rect bounds = window ? Rect{{window->Pos.x, window->Pos.y}, {window->Size.x, window->Size.y}}
                                           : Rect{{40, 40}, {640, 480}};
                result.surfaces.push_back({index, bounds, true});
                continue;
            }
            auto* root = window->DockNode;
            while (root->ParentNode)
            {
                root = root->ParentNode;
            }
            if (!nodes.contains(root))
            {
                result.surfaces.push_back(
                    {insert(root), {{root->Pos.x, root->Pos.y}, {root->Size.x, root->Size.y}}, !root->IsDockSpace()}
                );
            }
            const auto leaf = insert(window->DockNode);
            result.nodes[leaf].panes.emplace_back(pane);
        }
        for (std::size_t i{}; i < pending.size(); ++i)
        {
            const auto* node = pending[i];
            if (!node->ChildNodes[0] || !node->ChildNodes[1])
            {
                continue;
            }
            const auto first = insert(node->ChildNodes[0]);
            const auto second = insert(node->ChildNodes[1]);
            auto& output = result.nodes[nodes.at(node)];
            output.split = node->SplitAxis == ImGuiAxis_X ? EDockSplit::HORIZONTAL : EDockSplit::VERTICAL;
            output.first = first;
            output.second = second;
            const auto extent = [&](const ImGuiDockNode* child)
            { return node->SplitAxis == ImGuiAxis_X ? child->Size.x : child->Size.y; };
            const auto total = extent(node->ChildNodes[0]) + extent(node->ChildNodes[1]);
            output.ratio = total > 0 ? std::clamp(extent(node->ChildNodes[0]) / total, .001F, .999F) : .5F;
        }
        return result;
    }

} // namespace lux::ui
