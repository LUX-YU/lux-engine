#pragma once
// ============================================================================
//  EditorShell — the editor's PANEL COMPOSITION: owns every
//  default panel contributions, their activation host, and the
//  inter-panel signal wiring (browser double-click → graph/script editors,
//  viewport drop → spawn, content-changed → rescan). Pure composition — no
//  domain state: scene state lives in EditorScene, app services stay on
//  LuxEditor (the process-domain composition root), and this shell reaches
//  them through the host reference its build/wire methods take.
//
//  Two-phase bring-up MIRRORS the original init() order exactly (a pure code
//  move — zero change to startup order):
//    buildPanels()       — construct panels + create-menu hooks + window
//                          registrations (before the render thread starts).
//    wireAssetServices() — the wiring that needs the render session + asset
//                          services (thumbnails, registry, texture cache,
//                          material preview) — after those exist.
//  Destruction is passive RAII: subscriptions die first, then EditorToolHost
//  unregisters every panel before releasing its owner.
//
//  Private editor header (engine/editor/src/app — not installed).
// ============================================================================

#include <lux/engine/editor/extensions/EditorTools.hpp>
#include <lux/engine/events/DomainEvents.hpp>   // SubscriptionGroup panel_subs_(事件批D)

#include <memory>
#include <string>

namespace lux::asset { class AssetManager; }
namespace lux::flowforge { class NodeRegistry; }
namespace lux::ui { class Panel; class UISystem; class SceneViewportPanel; }

namespace lux::editor
{
    class LuxEditor;
    class AssetBrowser;
    class InspectorPanel;
    class HierarchyPanel;
    class SceneFeatureSettingPanel;
    class MaterialGraphPanel;
    class FlowGraphPanel;
    class LuaConsole;
    class EditorScene;
    class AssetRegistry;
    class MaterialPreviewHost;
    class ThumbnailService;
    class FlowGraphCompiler;

    class EditorShell
    {
    public:
        EditorShell();
        ~EditorShell();   // out-of-line: unique_ptrs to fwd-declared panels

        EditorShell(const EditorShell&)            = delete;
        EditorShell& operator=(const EditorShell&) = delete;

        /// Phase 1 (original init step 5): construct the default panels, the
        /// Create-menu hooks, and register every panel as a toggleable window.
        [[nodiscard]] bool buildPanels(
            LuxEditor& host,
            AssetRegistry& asset_registry,
            std::shared_ptr<lux::asset::AssetManager> assets,
            lux::events::DomainEvents& events,
            lux::flowforge::NodeRegistry& flow_nodes,
            FlowGraphCompiler& compiler);

        /// Phase 2 (original init steps 7b-8): the wiring that needs the render
        /// session + asset services — thumbnail hookup, Inspector asset fields,
        /// material/flow/script asset editors, viewport asset-drop spawn.
        void wireAssetServices(LuxEditor&           host,
                               ThumbnailService*    thumbnails,
                               AssetRegistry*       asset_registry,
                               MaterialPreviewHost* material_preview);

        /// Scene-domain re-target (C2/C11): called from the host's
        /// setOnSceneChanged — repoints the settings panel target and the
        /// panels' Selection pointers (null between scenes).
        void retargetScene(LuxEditor& host, EditorScene* scene);

        // ── Composition surface (host + controllers reach panels here) ────
        AssetBrowser*                assetBrowser() noexcept;
        InspectorPanel*              inspectorPanel() noexcept;
        lux::ui::SceneViewportPanel* viewportPanel() noexcept;
        HierarchyPanel*              hierarchyPanel() noexcept;
        SceneFeatureSettingPanel*    sceneSettingsPanel() noexcept;
        FlowGraphPanel*              flowGraphPanel() noexcept;
        MaterialGraphPanel*          materialGraphPanel() noexcept;
        EditorToolHost&              toolHost()          noexcept { return *tool_host_; }
        EditorTools                  tools() const noexcept { return tool_host_->facade(); }
        EditorPanelCatalog&          panelCatalog() noexcept
        {
            return panel_catalog_;
        }
        std::size_t processToolSafePoint(std::size_t budget = 32u) noexcept
        {
            return tool_host_ ? tool_host_->processSafePoint(budget) : 0u;
        }

    private:
        EditorPanelCatalog                          panel_catalog_;
        std::unique_ptr<EditorToolHost>             tool_host_;

        [[nodiscard]] LuaConsole* scriptEditorPanel() noexcept;

        // 装配层事实订阅(browser/registry 只对目录结构变化重扫;
        // §7.95 的「shell 的 panel_subs_」)。Declared LAST so RAII
        // unsubscribes before EditorToolHost and its panels die.
        // (曾并列一个 ConnectionGroup panel_conns_:viewport asset_dropped 的
        //  Signal 连接。信号层退役批改为面板单槽回调,槽随面板亡,无连接对象。)
        lux::events::SubscriptionGroup               panel_subs_;
    };

} // namespace lux::editor
