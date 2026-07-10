#include <lux/engine/editor/panels/HierarchyPanel.hpp>
#include <lux/engine/editor/app/StateRegistry.hpp>
#include <lux/engine/editor/app/Selection.hpp>

#include <lux/engine/ecs/components/NameComponent.hpp>
#include <lux/engine/ecs/HierarchyView.hpp>

#include <imgui.h>

#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <utility>

namespace lux::editor
{
    HierarchyPanel::HierarchyPanel(std::string title, StateRegistry& states)
        : Panel(std::move(title), {280.f, 600.f})
        , sel_(states.ensure<Selection>())
    {
    }

    void HierarchyPanel::paint()
    {
        // Immediate-mode: read the shared selection state each frame.
        entt::registry* reg = sel_->registry();
        if (!reg)
        {
            ImGui::TextDisabled("No registry bound.");
            return;
        }

        // entt 3.16 exposes a view over all live entities directly.
        auto all = reg->view<entt::entity>();

        ImGui::TextDisabled("%zu entities", static_cast<size_t>(all.size()));
        ImGui::Separator();

        // Build parent -> children once per frame from HierarchyComponent (only
        // children carry it; a root has none). The editor's multi-mesh model import
        // already creates ONE root + N child mesh-entities linked this way — the panel
        // now renders that tree instead of a flat list.
        const lux::ecs::HierarchyView hierarchy(*reg);

        const entt::entity selected = sel_->entity();

        // Recursive node render. Branch nodes are collapsible AND click-selectable
        // (OpenOnArrow: the label click selects, the arrow toggles); leaves are
        // selectable rows. Clicking a node in the tree selects EXACTLY it (the tree is
        // the explicit drill-into-a-child path; viewport picking promotes to the root).
        std::function<void(entt::entity)> renderNode = [&](entt::entity e)
        {
            const auto raw = static_cast<std::uint32_t>(entt::to_integral(e));
            const auto* nc = reg->try_get<lux::ecs::NameComponent>(e);
            const std::string label = nc
                ? std::format("{}##entity_{}", nc->name, raw)
                : std::format("Entity {}##entity_{}", raw, raw);

            const bool has_children = hierarchy.hasChildren(e);

            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
                                     | ImGuiTreeNodeFlags_SpanAvailWidth
                                     | ImGuiTreeNodeFlags_DefaultOpen;
            if (e == selected)  flags |= ImGuiTreeNodeFlags_Selected;
            if (!has_children)  flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

            const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
            // Select on a label click (not an arrow toggle); single writer of selection.
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
                sel_->selectEntity(e);

            if (has_children && open)
            {
                for (auto c : hierarchy.childrenOf(e)) renderNode(c);
                ImGui::TreePop();
            }
        };

        // Render top-level roots: entities with no HierarchyComponent, OR whose parent
        // link is stale/invalid (orphans surface at top level rather than vanishing).
        // Children of a valid parent are rendered via that parent's recursion.
        for (auto e : all)
        {
            const auto* hc = reg->try_get<lux::ecs::HierarchyComponent>(e);
            if (hc && reg->valid(hc->parent)) continue;
            renderNode(e);
        }

        // Click on empty area below the list -> clear selection.
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        if (avail.y > 4.f)
        {
            ImGui::PushID("##hierarchy_empty_area");
            if (ImGui::InvisibleButton("clear_select", ImVec2(-FLT_MIN, avail.y)))
                sel_->clear();
            ImGui::PopID();
        }
    }

} // namespace lux::editor
