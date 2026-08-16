#pragma once
/**
 * @file InspectorPanel.hpp
 * @brief Reflection-driven inspector: shows all registered ECS components on
 *        the currently-selected entity.
 *
 * Lives in the editor layer (NOT the generic `ui` framework): it is coupled to
 * ECS (`entt`) + the injected ComponentTypeCatalog, so it belongs with the
 * other editor panels, not in the reusable imgui/render/Panel framework. It
 * reads the SCENE'S `Selection` each frame (immediate-mode) through a pointer
 * re-targeted on scene change — no push API.
 *
 * Field rendering is delegated to `lux::ui::WidgetDispatch` (the reflection-aware
 * widget mechanism, which stays in `ui` as a reusable primitive).
 *
 * A struct annotated with `LUX_COMPONENT()` auto-appears here once its module's
 * reflection sidecar is loaded — no per-type editor wiring. Display label:
 * class-level `display_name=` annotation, else the short class name.
 */

#include <lux/engine/editor/visibility.h>
#include <lux/engine/meta/LuxObject.hpp>
#include <lux/engine/ui/Panel.hpp>
#include <lux/engine/ui/WidgetDispatch.hpp>
#include <lux/engine/ui/SearchPopupElement.hpp>
#include <lux/engine/resource/asset/Asset.hpp>   // asset_id_t, EAssetType
#include <lux/engine/authoring/world/WorldIdentifiers.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace lux::meta  { struct RefClass; struct RefField; }
namespace lux::asset { class AssetManager; }
namespace lux::ecs { class ComponentTypeCatalog; }

namespace lux::editor
{
    class Selection;
    class AssetRegistry;
    class EditorScene;

    class LUX_EDITOR_PUBLIC InspectorPanel : public lux::ui::Panel
    {
    public:
        InspectorPanel(
            std::string title,
            const lux::ecs::ComponentTypeCatalog& components);
        ~InspectorPanel() override = default;

        /// Re-target the scene-domain Selection. Called by the
        /// host on every scene change: the live scene's Selection, or null
        /// between scenes (the panel then paints its empty state).
        void setSelection(Selection* sel) noexcept { selection_ = sel; }
        void setEditorScene(EditorScene* scene) noexcept { scene_ = scene; }

        /// Wire the project asset index so asset-reference fields (an
        /// `asset_id_t`, e.g. MeshComponent::material_asset_id) render as a
        /// named, type-validated drag-drop + fuzzy-search picker instead of a
        /// raw hex id. Borrowed (owned by the host).
        void setAssetRegistry(AssetRegistry* registry) noexcept { registry_ = registry; }

        /// Wire the asset manager so bound-asset tooltips can resolve the
        /// VIRTUAL PATH through its VFS (covers /Game + /Engine + future pak
        /// mounts — the registry alone only knows loose /Game files).
        /// Borrowed (owned by the host).
        void setAssetManager(lux::asset::AssetManager* mgr) noexcept { asset_mgr_ = mgr; }

        /// Wire the "which components can this scene use" provider (editor ADR
        /// §7): the Add-Component menu enables the FQNs it returns and greys the
        /// rest. Null / empty result → every reflected component is offered
        /// (no active scene to constrain against). Borrowed closure.
        void setAvailableComponentsProvider(
            std::function<std::vector<std::string>()> p) noexcept
        { available_components_ = std::move(p); }


    private:
        const lux::ecs::ComponentTypeCatalog& components_;
        void paint() override;
        void displayField(const lux::meta::RefField& field, void* base);
        void drawAddComponentMenu(
            lux::meta::EntityRegistryBase& reg,
            entt::entity e);
        void paintWorldInstance(lux::authoring::WorldInstanceId instance);

        /// Render an asset-reference field (`asset_id_t`): the bound asset's
        /// name + a type-validated drag-drop target + a fuzzy-search "pick"
        /// button. Returns false if the field is NOT an asset_id_t (caller then
        /// falls through to the generic widget dispatch / hex fallback).
        bool drawAssetField(const lux::meta::RefField& field, void* base);

        /// Populate the fuzzy-search popup with the assets acceptable for
        /// @p field (by its conventional name) and open it; the chosen id is
        /// written back through `pick_resolver_` + `pick_offset_`.
        void openAssetPicker(const lux::meta::RefField& field);

        Selection* selection_{nullptr}; ///< scene-domain; re-targeted on scene change (C11)
        EditorScene* scene_{nullptr};   ///< same scene-domain lifetime as selection_
        lux::ui::WidgetDispatch dispatch_;
        /// Per-component edit accumulator (part of the write contract): displayField ORs the
        /// widgets' "edited this frame" into it; the component loop fires
        /// ComponentTypeEntry::notify when set, so Inspector field pokes still
        /// reach entt's on_update signal.
        bool component_edited_{false};
        std::function<std::vector<std::string>()> available_components_; ///< scene's capability-gated FQNs

        // ---- asset-reference field support (material / mesh / texture) -------
        AssetRegistry*              registry_{ nullptr };   ///< borrowed; name + candidate lists
        lux::asset::AssetManager*   asset_mgr_{ nullptr };  ///< borrowed; vfs pathOf for tooltips
        lux::ui::SearchPopupElement asset_picker_{ "Pick Asset##inspector" };
        std::vector<lux::asset::asset_id_t> picker_candidates_; ///< parallel to the popup items
        // Re-resolves the component base of the field being picked (avoids a
        // dangling pointer if ECS storage moves between open + select); set when
        // the "pick" button is pressed, consumed by the popup's select callback.
        std::function<void*()>      pick_resolver_;
        std::size_t                 pick_offset_{ 0 };
        // picker 落地在组件表之外(component_edited_ 的消费点已过),写完要
        // 自己发 notify(patch<T> → on_update);与 pick_resolver_ 同期设置。
        std::function<void()>       pick_notify_;
        // Resolver for the component currently being painted (transient, rebuilt
        // each component); copied into pick_resolver_ on a "pick" press.
        std::function<void*()>      current_comp_resolver_;
        // 同上,当前组件的 notify(transient,与 resolver 同点重建)。
        std::function<void()>       current_comp_notify_;
    };

} // namespace lux::editor
