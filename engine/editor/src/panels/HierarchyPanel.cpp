#include <lux/engine/editor/panels/HierarchyPanel.hpp>
#include <lux/engine/editor/app/Selection.hpp>

#include <lux/engine/ecs/components/NameComponent.hpp>
#include <lux/engine/ecs/HierarchyIndex.hpp>
#include <lux/engine/ecs/render/components/RenderViewBindingComponent.hpp>   // editor-wiring guard (Delete)
#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/ecs/render/components/PrimaryCameraTag.hpp>
#include <lux/engine/ecs/render/components/2d/Camera2DComponent.hpp>
#include <lux/engine/ecs/render/components/3d/Camera3DComponent.hpp>

#include <imgui.h>

#include <cstdint>
#include <cstring>
#include <lux/cxx/core/Format.hpp>
#include <functional>
#include <string>
#include <utility>

namespace lux::editor
{
    HierarchyPanel::HierarchyPanel(
        std::string title,
        const lux::ecs::ComponentTypeCatalog& components)
        : Panel(std::move(title), {280.f, 600.f})
        , components_(components)
    {
    }

    void HierarchyPanel::setWorldActorSource(
        std::function<std::size_t()> count,
        std::function<std::vector<HierarchyWorldActorItem>(
            std::string_view, std::size_t, std::size_t)> query,
        std::function<void(lux::authoring::WorldActorId)> open)
    {
        world_actor_count_ = std::move(count);
        world_actor_query_ = std::move(query);
        world_actor_open_ = std::move(open);
        world_actor_offset_ = 0u;
        observed_world_actor_count_ = 0u;
        world_actor_page_.clear();
        world_actor_search_[0] = '\0';
        world_actor_query_dirty_ = true;
    }

    void HierarchyPanel::clearWorldActorSource() noexcept
    {
        world_actor_count_ = {};
        world_actor_query_ = {};
        world_actor_open_ = {};
        world_actor_page_.clear();
        world_actor_offset_ = 0u;
        observed_world_actor_count_ = 0u;
        world_actor_query_dirty_ = true;
    }

    void HierarchyPanel::beginRename(
        lux::meta::EntityRegistryBase& reg,
        entt::entity e)
    {
        renaming_ = e;
        rename_buf_[0] = '\0';
        if (const auto* nc = reg.try_get<lux::ecs::NameComponent>(e))
        {
            std::strncpy(rename_buf_, nc->name.c_str(), sizeof(rename_buf_) - 1);
            rename_buf_[sizeof(rename_buf_) - 1] = '\0';
        }
        rename_focus_pending_ = true;
    }

    void HierarchyPanel::paint()
    {
        // Immediate-mode: read the SCENE'S selection each frame. Null between
        // scenes (C11 — the Selection lives and dies with the scene).
        lux::meta::EntityRegistryBase* reg =
            sel_ ? sel_->registry() : nullptr;
        if (!reg)
        {
            ImGui::TextDisabled("No scene open.");
            return;
        }

        // entt 3.16 exposes a view over all live entities directly.
        auto all = reg->view<entt::entity>();

        // ── Header: `+` (Create menu) + entity count ─────────────────────────
        if (ImGui::SmallButton("+"))
            ImGui::OpenPopup("##hierarchy_create");
        ImGui::SameLine();
        ImGui::TextDisabled("%zu entities", static_cast<size_t>(all.size()));
        if (ImGui::BeginPopup("##hierarchy_create"))
        {
            if (create_menu_hook_) create_menu_hook_();
            ImGui::EndPopup();
        }
        ImGui::Separator();

        const entt::entity selected = sel_->entity();

        // The editor camera's viewport wiring must not be deletable from here —
        // destroying it kills the viewport (RenderViewBinding = editor-owned).
        const auto isEditorWired = [&](entt::entity e)
        {
            return reg->all_of<lux::ecs::RenderViewBindingComponent>(e);
        };
        // Deletion AND duplication are DEFERRED to after the tree walk: entt
        // only guarantees destroying the CURRENT entity mid-iteration, and
        // creating entities/components mid-walk can shuffle the storage the
        // walk is reading. Context menus fire inside the recursion, so queue.
        entt::entity pending_delete    = entt::null;
        entt::entity pending_duplicate = entt::null;
        const auto destroyEntity = [&](entt::entity e)
        {
            pending_delete = e;
        };

        // ── Keyboard ops (panel focused, not mid-rename) ─────────────────────
        //   F2 = rename the selection, Del = delete it (guard editor wiring).
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
            renaming_ == entt::null && reg->valid(selected))
        {
            if (ImGui::IsKeyPressed(ImGuiKey_F2, false))
                beginRename(*reg, selected);
            else if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) &&
                     !isEditorWired(selected))
                destroyEntity(selected);
            else if (ImGui::GetIO().KeyCtrl &&
                     ImGui::IsKeyPressed(ImGuiKey_D, false))
                pending_duplicate = selected;
        }

        // Build parent -> children once per frame from ParentComponent (only
        // children carry it; a root has none). The editor's multi-mesh model import
        // already creates ONE root + N child mesh-entities linked this way — the panel
        // now renders that tree instead of a flat list.
        auto& hierarchy = lux::ecs::hierarchyIndex(*reg);

        // Recursive node render. Branch nodes are collapsible AND click-selectable
        // (OpenOnArrow: the label click selects, the arrow toggles); leaves are
        // selectable rows. Clicking a node in the tree selects EXACTLY it (the tree is
        // the explicit drill-into-a-child path; viewport picking promotes to the root).
        std::function<void(entt::entity)> renderNode = [&](entt::entity e)
        {
            const auto raw = static_cast<std::uint32_t>(entt::to_integral(e));

            // ── Inline rename row (F2 / context menu) ────────────────────────
            // Replaces the whole node row while active; children reappear on
            // commit/cancel (rename is momentary). Enter commits, Escape cancels,
            // clicking away commits.
            if (renaming_ == e)
            {
                ImGui::PushID(static_cast<int>(raw));
                if (rename_focus_pending_)
                {
                    ImGui::SetKeyboardFocusHere();
                    rename_focus_pending_ = false;
                }
                ImGui::SetNextItemWidth(-FLT_MIN);
                const bool entered = ImGui::InputText("##rename", rename_buf_,
                    sizeof(rename_buf_), ImGuiInputTextFlags_EnterReturnsTrue);
                const bool cancel = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
                if (entered || (ImGui::IsItemDeactivated() && !cancel))
                {
                    if (rename_buf_[0] != '\0')
                        reg->emplace_or_replace<lux::ecs::NameComponent>(
                            e, lux::ecs::NameComponent{rename_buf_});
                    renaming_ = entt::null;
                }
                else if (cancel)
                {
                    renaming_ = entt::null;
                }
                ImGui::PopID();
                return;
            }

            const auto* nc = reg->try_get<lux::ecs::NameComponent>(e);
            const std::string label = nc
                ? lux::format("{}##entity_{}", nc->name, raw)
                : lux::format("Entity {}##entity_{}", raw, raw);

            const bool has_children = hierarchy.hasChildren(e);

            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
                                     | ImGuiTreeNodeFlags_SpanAvailWidth
                                     | ImGuiTreeNodeFlags_DefaultOpen;
            if (e == sel_->entity()) flags |= ImGuiTreeNodeFlags_Selected;
            if (!has_children)  flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

            const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
            // Select on a label click (not an arrow toggle); single writer of selection.
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
                sel_->selectEntity(e);

            // ── Per-entity context menu (right-click the row) ────────────────
            if (ImGui::BeginPopupContextItem())
            {
                sel_->selectEntity(e);   // acting on it implies selecting it
                if (ImGui::MenuItem("Rename", "F2"))
                    beginRename(*reg, e);
                if (ImGui::MenuItem("Duplicate", "Ctrl+D"))
                    pending_duplicate = e;
                const bool wired = isEditorWired(e);
                ImGui::BeginDisabled(wired);
                if (ImGui::MenuItem("Delete", "Del"))
                    destroyEntity(e);
                ImGui::EndDisabled();
                if (wired && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Editor viewport wiring — cannot be deleted.");
                // Active-camera reassignment — only offered on camera entities.
                // One tag per scene: retagging clears every other holder. The
                // tag is what the save path records in the header and what a
                // game host binds to its primary view on start-up.
                if (reg->any_of<lux::ecs::Camera2DComponent,
                                lux::ecs::Camera3DComponent>(e))
                {
                    const bool is_active =
                        reg->all_of<lux::ecs::PrimaryCameraTag>(e);
                    ImGui::BeginDisabled(is_active);
                    if (ImGui::MenuItem(is_active ? "Active Camera (current)"
                                                  : "Set as Active Camera"))
                    {
                        reg->clear<lux::ecs::PrimaryCameraTag>();
                        reg->emplace<lux::ecs::PrimaryCameraTag>(e);
                    }
                    ImGui::EndDisabled();
                }
                ImGui::Separator();
                if (ImGui::BeginMenu("Create"))
                {
                    if (create_menu_hook_) create_menu_hook_();
                    ImGui::EndMenu();
                }
                ImGui::EndPopup();
            }

            if (has_children && open)
            {
                for (auto c : hierarchy.childrenOf(e)) renderNode(c);
                ImGui::TreePop();
            }
        };

        // Render top-level roots: entities with no ParentComponent, OR whose parent
        // link is stale/invalid (orphans surface at top level rather than vanishing).
        // Children of a valid parent are rendered via that parent's recursion.
        for (auto e : all)
        {
            const auto* hc = reg->try_get<lux::ecs::ParentComponent>(e);
            if (hc && reg->valid(hc->parent())) continue;
            renderNode(e);
        }

        if (world_actor_count_ && world_actor_query_ && world_actor_open_)
        {
            constexpr std::size_t kWorldActorPageSize = 128u;
            const auto indexed_count = world_actor_count_();
            if (indexed_count != observed_world_actor_count_)
            {
                observed_world_actor_count_ = indexed_count;
                world_actor_query_dirty_ = true;
            }
            ImGui::SeparatorText("World Actors");
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputTextWithHint(
                    "##world_actor_search",
                    "Search name or Actor class",
                    world_actor_search_,
                    sizeof(world_actor_search_)))
            {
                world_actor_offset_ = 0u;
                world_actor_query_dirty_ = true;
            }
            if (world_actor_query_dirty_)
            {
                world_actor_page_ = world_actor_query_(
                    world_actor_search_,
                    world_actor_offset_,
                    kWorldActorPageSize);
                world_actor_query_dirty_ = false;
            }
            ImGui::TextDisabled(
                "%zu indexed Actors", observed_world_actor_count_);
            ImGui::BeginChild(
                "##world_actor_page", ImVec2(0.0f, 180.0f), true);
            for (const auto& actor : world_actor_page_)
            {
                const auto id = uuids::to_string(actor.id.value());
                ImGui::PushID(id.c_str());
                const auto label = actor.actor_class.empty()
                    ? actor.display_name
                    : lux::format(
                          "{}  [{}]", actor.display_name, actor.actor_class);
                if (ImGui::Selectable(label.c_str(), false))
                    world_actor_open_(actor.id);
                ImGui::PopID();
            }
            ImGui::EndChild();
            const bool has_previous = world_actor_offset_ != 0u;
            ImGui::BeginDisabled(!has_previous);
            if (ImGui::SmallButton("Previous"))
            {
                world_actor_offset_ = world_actor_offset_ >
                        kWorldActorPageSize
                    ? world_actor_offset_ - kWorldActorPageSize
                    : 0u;
                world_actor_query_dirty_ = true;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            const bool has_next = world_actor_page_.size() ==
                kWorldActorPageSize;
            ImGui::BeginDisabled(!has_next);
            if (ImGui::SmallButton("Next"))
            {
                world_actor_offset_ += kWorldActorPageSize;
                world_actor_query_dirty_ = true;
            }
            ImGui::EndDisabled();
        }

        // Click on empty area below the list -> clear selection; right-click it
        // -> the Create menu (the blank-canvas creation path).
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        if (avail.y > 4.f)
        {
            ImGui::PushID("##hierarchy_empty_area");
            if (ImGui::InvisibleButton("clear_select", ImVec2(-FLT_MIN, avail.y)))
                sel_->clear();
            if (ImGui::BeginPopupContextItem("##hierarchy_blank_ctx"))
            {
                if (create_menu_hook_) create_menu_hook_();
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }

        // Execute the deferred delete now that no view iteration is live.
        if (pending_delete != entt::null && reg->valid(pending_delete))
        {
            if (renaming_ == pending_delete) renaming_ = entt::null;
            reg->destroy(pending_delete);
            if (sel_->entity() == pending_delete) sel_->clear();
        }

        // Deferred DUPLICATE: clone every reflected component the
        // source carries via the generated clone shims. Move-only components
        // (null clone) are skipped by design; runtime wiring (RenderViewBinding)
        // is not in the catalogue at all, so a duplicate never steals the
        // viewport. The copy is renamed "<name> Copy" and selected.
        if (pending_duplicate != entt::null && reg->valid(pending_duplicate))
        {
            const auto dst = reg->create();
            for (const auto& ce : components_.all())
                if (ce.operations.has && ce.operations.clone &&
                    ce.has(*reg, pending_duplicate))
                {
                    ce.operations.clone(
                        *reg, pending_duplicate, *reg, dst);
                }
            if (auto* nc = reg->try_get<lux::ecs::NameComponent>(dst))
                nc->name += " Copy";
            sel_->selectEntity(dst);
        }
    }

} // namespace lux::editor
