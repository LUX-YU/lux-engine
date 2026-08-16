#pragma once
/**
 * @file MaterialPreviewHost.hpp
 * @brief 活材质预览 = 编辑器的**第二个私有 SceneRuntime**(装配归属 ADR 工作线三批 2)。
 *
 * 旧形态(已拆除的手写预览场景的 Live 模式)手工对着 RenderFrameSession 编排换刀:
 * compileShader → removeFeature/addFeature(ForwardMesh 带 graph_fragment
 * override)→ uploadGraphMaterial → addMeshInstance —— 那是把资源解析器 +
 * 网格实例子系统的职责在编辑器里重写了一遍,而且整场只有一个全场景共享的
 * forward override 槽位。
 *
 * 新形态:预览世界 = 手工 preview pack + previewProfile() 的 SceneRuntime
 * (与 ThumbnailService 同款,共用 PreviewWorldCommon 的装配小件),常驻实体
 * 三件:key light、轨道相机(SAMPLED 离屏 target 上自建 view)、**常驻球**
 * (MeshComponent{SM_Sphere, PreviewGrey})。
 *
 * 换刀 = **临时 in-memory MaterialAsset**:面板把双份 SPIR-V + 授权图凑成
 * MaterialData 递进来,这里 new UUID 注册进 AssetManager(不落盘),
 * `patch` 球实体的 material_asset_id → 新 id;resolver 走标准的
 * ensureGraphMaterial(compile 双 frag → uploadGraphMaterial,逐材质 forward
 * PSO,R1)→ 实例拆重建 → 新图。旧临时资产由 resolver 松手 → 归零广播 →
 * GPU 副本回收;本宿主监听同一条广播,把 AssetManager 侧的旧 shell
 * `removeAsset` 掉。参数拖动直呼 modifyGraphMaterial(句柄读球实体的
 * MeshGpuCacheComponent),换刀在途时「最后一次赢」。
 *
 * 编辑器私有(src/ 下,不进公共 include/ —— 对照 EditorShell.hpp 的先例)。
 * setGraphContent / updateGraphParams / orbit / requestResize 都是 RECORD-ONLY
 * (面板 paint 期、帧 CLOSED 时可调);tick() 做全部世界推进与命令发送,
 * 必须在宿主渲染帧 OPEN 时调用。
 */

#include <lux/engine/function/render/client/core/FeatureHandle.hpp>   // RenderTargetId
#include <lux/engine/resource/asset/AssetRef.hpp>              // asset_id_t(轻头)
#include <lux/engine/runtime/assets/AssetLoadService.hpp>

#include <cstdint>
#include <memory>
#include <unordered_set>

namespace lux::render { class RenderFrameSession; struct GraphMaterialData; }
namespace lux::asset  { class AssetManager; struct MaterialData; }
namespace lux::exec   { class AsyncRuntime; }

namespace lux::editor
{
    struct EditorRenderInfra;   // process-domain plan/registry (LuxEditor.hpp)

    class MaterialPreviewHost
    {
    public:
        MaterialPreviewHost(lux::asset::AssetManager&   assets,
                            lux::render::RenderFrameSession& session,
                            const EditorRenderInfra&    render_infra,
                            lux::asset_runtime::AssetClient asset_client,
                            lux::exec::AsyncRuntime& async);
        ~MaterialPreviewHost();

        MaterialPreviewHost(const MaterialPreviewHost&)            = delete;
        MaterialPreviewHost& operator=(const MaterialPreviewHost&) = delete;

        /// Blocking one-time bring-up: SAMPLED offscreen target + the private
        /// preview SceneRuntime + resident entities (key light / orbit camera /
        /// preview sphere). Call once at editor startup, before the main loop.
        /// Returns false if the preview runtime could not be brought up (the
        /// live preview then stays off; the panel shows nothing).
        [[nodiscard]] bool initialize(std::uint32_t render_size = 512);

        [[nodiscard]] bool ready() const noexcept { return ready_; }

        /// The SAMPLED offscreen target the preview composes onto — the panel's
        /// PreviewViewElement samples it through the ImGui target sentinel.
        [[nodiscard]] lux::render::RenderTargetId target() const noexcept;

        /// Owner-thread readiness used by diagnostics/tests: the sphere has a
        /// live instance whose resolved material is the latest requested one.
        [[nodiscard]] bool contentReady() const noexcept;

        /// Set/replace the live graph material. @p payload is the panel-baked
        /// in-memory MaterialData (authoring graph + BOTH passes' SPIR-V +
        /// texture-slot UUIDs). Recorded; tick() registers it as a temporary
        /// asset and swaps the sphere onto it. Re-callable on each material
        /// edit — single-flight with latest-wins coalescing.
        void setGraphContent(std::unique_ptr<lux::asset::MaterialData> payload);

        /// Cheap param-only update (no shader swap) — recorded, pushed as
        /// modifyGraphMaterial by tick() against the sphere's current GPU
        /// material handle. Last write wins.
        void updateGraphParams(const lux::render::GraphMaterialData& params);

        /// 把贴图资产解析成 bindless 索引 —— MaterialGraphPanel 的贴图槽预览用
        /// (EditorTextureCache 并轨进进程域驻留表后的接班人)。
        /// **paint 期(帧关)安全**:只查共享缓存的表 + 记名;真正的
        /// ensureTexture 由下一次 tick()(帧开)代发。未就绪返回 0(引擎默认
        /// 贴图),就绪后面板的重推检查看到索引翻转再补一次 updateGraphParams。
        [[nodiscard]] std::uint32_t resolveTextureIndex(const lux::asset::asset_id_t& id);

        /// Accumulate an orbit-camera delta (yaw rad, pitch rad, zoom factor).
        void orbit(float d_yaw, float d_pitch, float d_zoom);

        /// Record a new offscreen target size (applied by tick() via
        /// resizeTarget + a ViewPresentComponent extent patch — the camera's
        /// auto_aspect follows).
        void requestResize(std::uint32_t width, std::uint32_t height);

        /// Frame-OPEN, non-blocking: drive the preview world one frame, then
        /// advance the content-swap state machine, apply a pending param
        /// refresh / resize, and push the orbit camera pose.
        void tick();

        /// Tear down the preview SceneRuntime + destroy the offscreen target.
        /// MUST run while the render thread is still serving frames (it issues
        /// render commands) — the editor calls it BEFORE stopping the thread.
        /// Idempotent.
        [[nodiscard]] bool releaseGpu();

        /// CPU-side cleanup: drop the temporary in-memory material shells +
        /// the zero-broadcast consumer registration. Safe after the render
        /// thread stopped (touches no render state). Idempotent.
        void shutdown();

    private:
        // The private SceneRuntime + resident entities + the swap state machine.
        // Defined in the .cpp so this header (included by LuxEditor /
        // MaterialGraphPanel / EditorShell) stays free of scene-runtime / ecs /
        // GraphMaterialData includes.
        struct Host;

        lux::asset::AssetManager&   assets_;
        lux::render::RenderFrameSession& session_;
        const EditorRenderInfra&    infra_;
        lux::asset_runtime::AssetClient asset_client_;
        lux::exec::AsyncRuntime&     async_;

        std::unique_ptr<Host> host_;
        bool                  ready_{false};

        /// paint 期记名、tick 期代发 ensureTexture 的贴图槽(resolveTextureIndex)。
        /// 就绪即从集合清除;集合有界(面板同时绑定的槽数)。
        std::unordered_set<lux::asset::asset_id_t> pending_textures_;

        /// Frames a content swap may stay unresolved before the watchdog gives
        /// up (reverts the sphere to the previous material and drops the stuck
        /// temp asset) — a runtime shader-compile/upload failure otherwise
        /// blocks every later edit silently (~5 s @ 60 fps).
        static constexpr int kSwapFrameBudget = 300;
    };

} // namespace lux::editor
