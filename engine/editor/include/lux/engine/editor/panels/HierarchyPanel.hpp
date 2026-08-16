#pragma once
/**
 * @file HierarchyPanel.hpp
 * @brief Lists every entity in the current ECS registry; clicking one sets the
 *        scene's selection (read by InspectorPanel + the scene's F-focus/gizmo).
 *
 * The Selection is SCENE-DOMAIN state: the panel holds a raw
 * pointer to the live scene's Selection, re-targeted by the host on every
 * scene change (null between scenes → the panel paints an empty state). Reads
 * registry + current selection every frame, writes the selection on click.
 *
 * Entity AUTHORING lives here too (the mature-editor shape):
 *   - header `+` button and blank-area right-click → the Create menu (content
 *     drawn by the injected hook — the SpawnRegistry lives editor-side, the
 *     panel carries no shell dependency);
 *   - entity right-click → Rename / Delete (+ Create submenu);
 *   - keys while the panel is focused: F2 = rename, Del = delete.
 * Mutations go straight at the registry (the Inspector Add-Component idiom).
 */

#include <lux/engine/editor/visibility.h>
#include <lux/engine/meta/LuxObject.hpp>
#include <lux/engine/ui/Panel.hpp>
#include <lux/engine/resource/entity_scene/EntitySceneIdentifiers.hpp>

#include <entt/entt.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace lux::ecs { class ComponentTypeCatalog; }

namespace lux::editor
{
    class Selection;

    struct HierarchyWorldActorItem final
    {
        lux::entity_scene::PersistentEntityId id;
        std::string display_name;
        std::string actor_class;
    };

    class LUX_EDITOR_PUBLIC HierarchyPanel : public lux::ui::Panel
    {
    public:
        HierarchyPanel(
            std::string title,
            const lux::ecs::ComponentTypeCatalog& components);
        ~HierarchyPanel() override = default;

        /// Re-target the scene-domain Selection (C11). Called by the host on
        /// every scene change: the live scene's Selection, or null between
        /// scenes. The pointer lives exactly as long as that scene.
        void setSelection(Selection* sel) noexcept { sel_ = sel; }

        void setWorldActorSource(
            std::function<std::size_t()> count,
            std::function<std::vector<HierarchyWorldActorItem>(
                std::string_view, std::size_t, std::size_t)> query,
            std::function<void(lux::entity_scene::PersistentEntityId)> open);
        void clearWorldActorSource() noexcept;

        /// Inject the Create-menu CONTENT drawer (menu items + click handling),
        /// invoked inside the panel's create popups each frame they are open.
        /// LuxEditor wires this to its SpawnRegistry-backed menu.
        void setCreateMenuHook(std::function<void()> fn)
        { create_menu_hook_ = std::move(fn); }

    protected:
        void paint() override;

    private:
        const lux::ecs::ComponentTypeCatalog& components_;
        void beginRename(
            lux::meta::EntityRegistryBase& reg,
            entt::entity e);

        Selection* sel_{nullptr}; ///< scene-domain; re-targeted on scene change

        std::function<void()> create_menu_hook_;   ///< draws Create-menu items
        std::function<std::size_t()> world_actor_count_;
        std::function<std::vector<HierarchyWorldActorItem>(
            std::string_view, std::size_t, std::size_t)> world_actor_query_;
        std::function<void(lux::entity_scene::PersistentEntityId)> world_actor_open_;
        std::vector<HierarchyWorldActorItem> world_actor_page_;
        std::size_t world_actor_offset_{0u};
        std::size_t observed_world_actor_count_{0u};
        char world_actor_search_[128]{};
        bool world_actor_query_dirty_{true};

        // Inline-rename state: the entity whose row is an InputText this frame.
        entt::entity renaming_{entt::null};
        char         rename_buf_[128]{};
        bool         rename_focus_pending_{false};
    };

} // namespace lux::editor
