#pragma once
/**
 * @file InspectorPanel.hpp
 * @brief Reflection-driven inspector: shows all registered ECS components on
 *        the currently-selected entity.
 *
 * Lives in the editor layer (NOT the generic `ui` framework): it is coupled to
 * ECS (`entt`) + gameplay's `ComponentTypeRegistry`, so it belongs with the
 * other editor panels, not in the reusable imgui/render/Panel framework. It
 * reads the shared `Selection` state from the editor's `StateRegistry` each
 * frame (immediate-mode) — no push API.
 *
 * Field rendering is delegated to `lux::ui::WidgetDispatch` (the reflection-aware
 * widget mechanism, which stays in `ui` as a reusable primitive).
 *
 * A struct annotated with `LUX_COMPONENT()` auto-appears here once its module's
 * reflection sidecar is loaded — no per-type editor wiring. Display label:
 * class-level `display_name=` annotation, else the short class name.
 */

#include <lux/engine/editor/visibility.h>
#include <lux/engine/ui/Panel.hpp>
#include <lux/engine/ui/WidgetDispatch.hpp>
#include <lux/engine/ui/SearchPopupElement.hpp>
#include <lux/engine/asset/Asset.hpp>   // asset_id_t, EAssetType

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace lux::meta  { struct RefClass; struct RefField; }
namespace lux::asset { class AssetManager; }

namespace lux::editor
{
    class StateRegistry;
    class Selection;
    class AssetRegistry;

    class LUX_EDITOR_PUBLIC InspectorPanel : public lux::ui::Panel
    {
    public:
        /// @param states  The editor state registry; the panel ensures + caches
        ///        the shared `Selection` and reads it each paint.
        explicit InspectorPanel(std::string title, StateRegistry& states);
        ~InspectorPanel() override = default;

        /// Access the widget dispatch table to register custom type widgets
        /// (built-ins are registered during construction).
        lux::ui::WidgetDispatch& getDispatch() noexcept { return dispatch_; }

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

    private:
        void paint() override;
        void displayField(const lux::meta::RefField& field, void* base);

        /// Render an asset-reference field (`asset_id_t`): the bound asset's
        /// name + a type-validated drag-drop target + a fuzzy-search "pick"
        /// button. Returns false if the field is NOT an asset_id_t (caller then
        /// falls through to the generic widget dispatch / hex fallback).
        bool drawAssetField(const lux::meta::RefField& field, void* base);

        /// Populate the fuzzy-search popup with the assets acceptable for
        /// @p field (by its conventional name) and open it; the chosen id is
        /// written back through `pick_resolver_` + `pick_offset_`.
        void openAssetPicker(const lux::meta::RefField& field);

        std::shared_ptr<Selection> selection_; ///< ensured at ctor; refcounted; read each paint
        lux::ui::WidgetDispatch dispatch_;

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
        // Resolver for the component currently being painted (transient, rebuilt
        // each component); copied into pick_resolver_ on a "pick" press.
        std::function<void*()>      current_comp_resolver_;
    };

} // namespace lux::editor
