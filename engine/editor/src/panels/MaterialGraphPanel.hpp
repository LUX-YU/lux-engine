#pragma once
// ============================================================================
//  MaterialGraphPanel — the material graph editor, rebuilt as a THIN GraphKit
//  host (the second panel on the shared framework, after FlowGraphPanel).
//
//  The panel owns the domain SSOT (rdesc::MaterialGraph — the same object the
//  GLSL compiler lowers) plus the two domain adapters, and embeds ONE
//  lux::graphkit::GraphEditor that does everything canvas-shaped: blueprint
//  chrome, right-click fuzzy palette, drag-connect with schema validation,
//  undoable add/remove/connect/move, and position sync against the domain's
//  per-node ui_pos (persisted since LMGR v2 — reopen restores the layout).
//
//  Compile (lowerToIR -> emitGlsl -> compileToSpirv), the live preview, the
//  texture picker, the floating color picker and "Save as Asset" all stay
//  panel-side.
//
//  Private editor header (engine/editor/src — not installed).
// ============================================================================

#include <lux/engine/ui/Panel.hpp>
#include <lux/engine/ui/PreviewViewElement.hpp>
#include <lux/engine/ui/SearchPopupElement.hpp>
#include <lux/engine/authoring/assets/material/MaterialGraph.hpp>
#include <lux/engine/editor/framework/graphkit/GraphEditor.hpp>
#include <lux/engine/resource/asset/Asset.hpp>   // asset_id_t
#include <lux/engine/resource/asset/MaterialInstanceAsset.hpp>   // MaterialInstanceData(实例模式编辑副本)
#include <lux/cxx/core/move_only_function.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace lux::render { struct GraphMaterialData; }
namespace lux::asset  { class  AssetManager; }
namespace lux::events { class  DomainEvents; }

namespace lux::editor
{
    class MaterialPreviewHost;
    class AssetRegistry;
    class ThumbnailService;

    // ── 材质图编译的异步三段式(统一事件系统批H1;设计 §7.9-⑤)────────────
    //
    // 面板 paint 期通过强类型异步 client 提交图快照 → CPU 池上
    // compileMaterialJob(纯函数:lower → emitGlsl → 2×compileToSpirv)→
    // 完成事件回 frame 泵 → 面板 onCompiled 装配(预览推送/GLSL 显示)。
    // shaderc 从此不在 UI 帧里跑 —— 大图编辑不卡帧。latest-wins:面板只认
    // pending id 的完成事件,过期结果丢弃。

    /// 池上任务的输入快照(值语义,与面板状态解耦)。
    struct MaterialCompileJob
    {
        lux::rdesc::MaterialGraph graph;   ///< clone(池上只读)
        std::array<lux::asset::asset_id_t,
                   lux::asset::MaterialData::kMaxTextures> texture_slot_ids{};
    };

    /// 池上任务的输出(错误在带内:ok=false 时 status 是错误行)。
    struct MaterialCompileOutcome
    {
        bool        ok{false};
        std::string status;   ///< 面板状态行(成功/lower/emit/spirv 错误)
        std::string glsl;     ///< GBuffer GLSL(显示用;失败为空)
        /// 预览换刀载荷(图 clone + 双份 SPIR-V + 贴图槽);失败为 null。
        std::unique_ptr<lux::asset::MaterialData> payload;
    };

    /// 纯函数编译核(池上执行;测试可直接调用)。线程安全:只读
    /// job + 静态 include 目录常量,shaderc 每次调用自建编译器。
    [[nodiscard]] std::shared_ptr<MaterialCompileOutcome>
    compileMaterialJob(const MaterialCompileJob& job);

    /// IGraphView adapter over a borrowed rdesc::MaterialGraph.
    ///  - GraphNodeRef.id  = rdesc node_id (monotonic, never recycled).
    ///  - GraphPinRef      = (node id, side, index into inputs()/outputs()).
    ///  - canvas position lives in Node::ui_pos/ui_placed (persisted, LMGR v2).
    ///  - detach/attach ride MaterialGraph::extractNode / addNodeWithId — the
    ///    id-stability contract for undo.
    ///  - connect REJECTS an occupied input (port contract (b)); the command
    ///    stack pre-disconnects cap-1 pins, making replace-on-reconnect one
    ///    undoable transaction (an upgrade over the old silent overwrite).
    class MaterialGraphView final : public lux::graphkit::IGraphView
    {
    public:
        explicit MaterialGraphView(lux::rdesc::MaterialGraph& graph)
            : graph_(&graph)
        {
        }

        void forEachNode(
            const std::function<void(lux::graphkit::GraphNodeRef, std::string_view)>& fn)
            const override;
        std::uint32_t pinCount(lux::graphkit::GraphNodeRef node,
                               lux::graphkit::EPinSide side) const override;
        lux::graphkit::GraphPinView pin(lux::graphkit::GraphNodeRef node,
                                        lux::graphkit::EPinSide side,
                                        std::uint32_t index) const override;
        void forEachLink(
            const std::function<void(lux::graphkit::GraphLinkView)>& fn) const override;
        std::optional<lux::graphkit::GraphVec2>
             nodePos(lux::graphkit::GraphNodeRef node) const override;
        void setNodePos(lux::graphkit::GraphNodeRef node,
                        lux::graphkit::GraphVec2 pos) override;

        lux::graphkit::GraphNodeRef addNode(std::string_view template_id) override;
        lux::graphkit::NodeCapture  detachNode(lux::graphkit::GraphNodeRef node) override;
        bool attachNode(lux::graphkit::GraphNodeRef original,
                        lux::graphkit::NodeCapture capture) override;
        bool connect(lux::graphkit::GraphPinRef from, lux::graphkit::GraphPinRef to) override;
        bool disconnect(lux::graphkit::GraphPinRef from,
                        lux::graphkit::GraphPinRef to) override;
        void reconstructNode(lux::graphkit::GraphNodeRef node) override;

    private:
        lux::rdesc::MaterialGraph* graph_;
        /// pin() label scratch — snapshot contract: the returned string_view is
        /// only valid during the enclosing call (the editor consumes it
        /// immediately).
        mutable std::string        label_scratch_;
    };

    /// IGraphSchema adapter: connect-time type rule (exact / truncate / splat —
    /// the lowering's implicit conversions), the fixed palette table, per-value-
    /// type pin colors + per-kind header tints, and the four in-node body
    /// editors (Constant / Param / SampleTexture / Swizzle). Popup-shaped UI
    /// (texture picker, color picker) goes through the deferred popup queue;
    /// the panel draws it in panel space.
    class MaterialGraphSchema final : public lux::graphkit::IGraphSchema
    {
    public:
        MaterialGraphSchema(lux::rdesc::MaterialGraph& graph, MaterialGraphView& view)
            : graph_(&graph), view_(&view)
        {
        }

        /// Body edits that change the BAKED shader (constant literals, type
        /// selectors, swizzle picks) — the panel recompiles.
        void setOnStructureDirty(std::function<void()> fn) { on_structure_dirty_ = std::move(fn); }
        /// Param value edits — live preview update, NO recompile.
        void setOnParamsDirty(std::function<void()> fn) { on_params_dirty_ = std::move(fn); }

        /// R3 实例模式的节点体编辑钩子(面板在 ctor 接一次,闭包写
        /// inst_edit_ 的 override lane 并推预览)。lane/slot = 根图声明序。
        struct InstanceEditHooks
        {
            std::function<bool(std::uint32_t lane)>                param_overridden;
            std::function<void(std::uint32_t lane, float* out4)>   param_effective;
            std::function<void(std::uint32_t lane, bool on)>       param_toggle;
            std::function<void(std::uint32_t lane, const float*)>  param_set;
            std::function<bool(std::uint32_t slot)>                tex_overridden;
            std::function<void(std::uint32_t slot, bool on)>       tex_toggle;
            std::function<void(std::uint32_t slot)>                tex_clear;
        };
        void setInstanceEditHooks(InstanceEditHooks hooks) { inst_hooks_ = std::move(hooks); }
        /// R3:实例模式开关 —— 开着时节点体转「拓扑属于根图」的编辑面:
        /// Constant/Swizzle/类型选择只读展示;Param/SampleTexture 的值编辑
        /// 经 InstanceEditHooks 落为 override(不写图)。
        void setInstanceMode(bool on) { instance_mode_ = on; }
        /// SampleTexture body: display name of the texture bound to a slot.
        void setBoundTextureName(std::function<std::string(std::uint32_t)> fn)
        {
            bound_texture_name_ = std::move(fn);
        }

        lux::graphkit::ConnectResult canConnect(lux::graphkit::GraphPinRef from,
                                                lux::graphkit::GraphPinRef to,
                                                const lux::graphkit::IGraphView& view)
            const override;
        std::span<const lux::graphkit::NodeTemplate> palette() const override;
        lux::graphkit::PinStyleDesc pinStyle(const lux::graphkit::GraphPinType& type)
            const override;
        lux::graphkit::NodeStyleDesc nodeStyle(lux::graphkit::GraphNodeRef node,
                                               const lux::graphkit::IGraphView& view)
            const override;
        void drawNodeBody(lux::graphkit::GraphNodeRef node, lux::graphkit::IGraphView& view,
                          lux::graphkit::DeferredPopupQueue& popups) override;

    private:
        lux::rdesc::MaterialGraph* graph_;
        MaterialGraphView*         view_;

        std::function<void()>                      on_structure_dirty_;
        std::function<void()>                      on_params_dirty_;
        std::function<std::string(std::uint32_t)>  bound_texture_name_;
        InstanceEditHooks                          inst_hooks_;
        bool                                       instance_mode_{ false };
    };

    class MaterialGraphPanel : public lux::ui::Panel
    {
    public:
        using CompileDispatch = lux::cxx::move_only_function<bool(
            std::uint64_t,
            std::shared_ptr<const MaterialCompileJob>)>;

        explicit MaterialGraphPanel(std::string title);
        ~MaterialGraphPanel() override;

        /// Bind the live material-preview host (display + orbit). Wires the
        /// embedded PreviewViewElement to its SAMPLED target and pushes the
        /// current graph for preview (as an in-memory MaterialData payload).
        void setPreviewHost(MaterialPreviewHost* preview);

        /// Wire the project asset index + the texture-upload cache (for the
        /// SampleTexture picker + live texture preview) + the asset manager (for
        /// "Save as Asset") + the editor event hub (to announce a saved material).
        /// registry/events borrowed (owned by host); manager shared (used to
        /// create + register + export the baked asset).（贴图槽解析改经
        /// setPreviewHost 的预览宿主查进程域共享缓存 —— EditorTextureCache 已并轨。）
        void setAssetServices(AssetRegistry* registry,
                              std::shared_ptr<lux::asset::AssetManager> manager,
                              lux::events::DomainEvents* events,
                              ThumbnailService* thumbnails = nullptr);

        /// Open a material asset (UE-style: double-click in the browser).
        /// MATERIAL → 图模式(节点画布,现有行为);MATERIAL_INSTANCE →
        /// **实例模式**(用户裁决:复用本面板不另立):只列可覆盖的东西 ——
        /// 参数值/贴图槽/render-state,各带 override 位;**不显示节点图,
        /// 不允许改拓扑**(拓扑属于根材质)。父有效值沿 parent 链解析到根
        /// (根默认 ⊕ 各级 override,近端赢),未覆盖的行灰显父值。
        void openAsset(const lux::asset::asset_id_t& id);

        /// Author a fresh DEFAULT material asset directly into
        /// @p folder (the AssetBrowser's right-clicked location) under a free
        /// "NewMaterial[_N]" name, and open it in the editor. Replaces the
        /// current working graph (standard "New" semantics).
        bool createNewMaterialAssetAt(const std::filesystem::path& folder);

        void setCompileDispatch(CompileDispatch dispatch)
        {
            compile_dispatch_ = std::move(dispatch);
        }

        /// MainThreadScheduler completion target; stale request ids are ignored.
        void onCompiled(
            std::uint64_t request_id,
            std::shared_ptr<MaterialCompileOutcome> outcome);

    private:
        void paint() override;

        void buildDefaultGraph();
        /// 构图快照并经强类型异步 dispatch 提交；无 dispatch 的构造/测试
        /// 路径才直接调用纯编译核。
        void compile();
        /// 编译结果落地:状态行/GLSL/预览换刀 + 参数补发。
        void applyCompileOutcome(MaterialCompileOutcome& o);
        void drawToolbar();
        /// Graph-level render-state authoring (alpha mode/cutoff/double-sided).
        void drawGraphProperties();

        /// Bake the current graph into a MaterialAsset (see saveAsAsset in the
        /// cpp for the shared bake recipe) + the modal name popup. Empty
        /// @p folder = the legacy <content>/Materials/ location.
        bool saveAsAsset(const std::string& name, std::string* err,
                         const std::filesystem::path& folder = {});
        /// 原地保存:把当前图重编进 open_asset_id_ 那个资产(同 id 覆写内存
        /// payload + 覆写盘上文件)。没有打开的资产(默认图)时不可用 ——
        /// 那种情况走 Save As。paint 期安全(缩略图作废已支持帧关延迟)。
        bool saveInPlace(std::string* err);
        void drawSavePopup();

        // ---- 实例模式(openAsset 对 MATERIAL_INSTANCE 的分派;见 openAsset 文档) ----
        void openInstanceAsset(const lux::asset::asset_id_t& id);
        /// 沿 parent 链解析到根:root_id_/decls_/tex_decls_/parent_*_(父有效基线)。
        bool resolveInstanceChain(std::string* err);
        /// 换刀推送:合成有效 MaterialData(根图 clone + 有效值,SPIR-V 拷根 ——
        /// 实例永不编译)→ setGraphContent。
        void pushInstancePreview();
        /// 有效 (parent ⊕ inst_edit_) 的 GraphMaterialData(快路径/重推检查用)。
        [[nodiscard]] lux::render::GraphMaterialData buildInstanceEffective();
        bool saveInstanceInPlace(std::string* err);
        /// R3:override 参数/贴图变更的快路径推送(updateGraphParams)。
        void instanceParamsChanged();
        /// 实例模式的整个 paint 体(参数/贴图/render-state 表 + 预览)。
        /// R3:实例模式的属性条(Save/Revert/Close/parent 链 + render-state
        /// override)。参数/贴图的编辑面在**共用画布**的节点体里(schema 的
        /// 实例钩子),不再是独立列表 —— 拓扑锁定,Param/SampleTexture 可编。
        void drawInstanceProperties();

        lux::render::GraphMaterialData buildGraphMaterial();
        void                          openTexturePicker(std::uint32_t slot);
        std::string                   boundTextureName(std::uint32_t slot) const;

        /// Reset transient editor state + recompile after graph_ was replaced.
        void        resetEditorForNewGraph();
        std::string textureDisplayName(const lux::asset::asset_id_t& id) const;

        // Domain SSOT + the two stateless adapters + the shared editor.
        lux::rdesc::MaterialGraph           graph_;
        MaterialGraphView                   view_{ graph_ };
        MaterialGraphSchema                 schema_{ graph_, view_ };
        lux::graphkit::GraphEditor          editor_;
        std::uint64_t                       last_seen_revision_{ 0 }; ///< bake-on-edit poll

        std::string                         status_{ "Press Compile" };
        std::string                         glsl_;
        bool                                show_glsl_{ false };

        // Param whose color is being edited in the floating picker (invalid = none).
        lux::rdesc::node_id                 color_pick_node_{ lux::rdesc::invalid_node };

        // Live preview (display + orbit input) + the host it drives (borrowed).
        lux::ui::PreviewViewElement         preview_element_{ "MaterialPreview" };
        MaterialPreviewHost*                preview_{ nullptr };
        bool                                show_preview_{ false }; // opt-in via the toolbar button

        // SampleTexture slot -> bound texture (uuid + cached display name) + the
        // async-upload re-push tracker; borrowed asset services + the picker popup.
        struct TexBinding { lux::asset::asset_id_t uuid; std::string name; };
        std::unordered_map<std::uint32_t, TexBinding> slot_texture_;
        std::uint32_t                       last_tex_bindless_[8]{};
        AssetRegistry*                      registry_{ nullptr };
        lux::events::DomainEvents*              events_{ nullptr }; ///< borrowed; committed editor facts only
        CompileDispatch                         compile_dispatch_;
        std::uint64_t                       next_compile_id_{ 0 };     ///< 批H1:请求配对
        std::uint64_t                       pending_compile_id_{ 0 };  ///< 0 = 无在途
        lux::ui::SearchPopupElement         texture_popup_{ "Pick Texture##matgraph" };

        // "Save as Asset": the manager (create + register + export) + the modal
        // name-input popup state.
        std::shared_ptr<lux::asset::AssetManager> asset_manager_;
        bool                                save_popup_open_{ false };
        std::string                         save_name_;
        std::string                         save_status_;   // popup error feedback

        // 当前打开的资产(openAsset / saveAsAsset 成功时记下,New/默认图清空)。
        // 没有它,「保存」只能永远走「起名→新 UUID→新资产」,重存一次已有材质
        // = 生成孤儿资产,场景里的引用全指旧 id —— Save 的存在以此为前提。
        // 实例模式复用同一对(语义一致:「当前打开的资产」),Save 按模式分叉。
        lux::asset::asset_id_t              open_asset_id_{};
        std::filesystem::path               open_asset_path_;

        // 借来的缩略图服务(可空):原地保存后作废旧缩略图。
        ThumbnailService*                   thumbnails_{ nullptr };

        // ---- 实例模式状态(mode_==Instance 时有效;图模式路径零触碰) ----
        enum class EEditMode : std::uint8_t { GRAPH, INSTANCE };
        EEditMode                           mode_{ EEditMode::GRAPH };
        lux::asset::MaterialInstanceData    inst_edit_{};   ///< 编辑副本,Save 写回
        lux::asset::asset_id_t              inst_root_id_{};
        std::vector<lux::rdesc::ParamSlotDecl>   inst_decls_;     ///< 根图参数声明
        std::vector<lux::rdesc::TextureSlotDecl> inst_tex_decls_; ///< 根图贴图槽声明
        float                  inst_parent_params_[lux::asset::MaterialInstanceData::kMaxParams][4]{};
        lux::asset::asset_id_t inst_parent_tex_[lux::asset::MaterialInstanceData::kMaxTextures]{};
        std::uint32_t          inst_parent_alpha_{ 0 };
        bool                   inst_parent_dbl_{ false };
    };
}
