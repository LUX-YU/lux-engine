#pragma once
/**
 * @file ThumbnailService.hpp
 * @brief Editor-resident orchestrator for asset thumbnails (non-blocking).
 *
 * Owns a SECOND `lux::runtime::SceneRuntime` (装配归属 ADR 工作线三):
 * a private preview world — key light + framing camera as ENTITIES, a manual
 * preview pack (Transform/Camera systems + Mesh/CameraUpload/DirectionalLight
 * render subsystems), `previewProfile()` for the forward pipeline — composed
 * onto a host-owned offscreen target. The old hand-rolled session scripting
 * (uploadMesh / compileShader / uploadGraphMaterial /
 * addMeshInstance + manual handle bookkeeping) is GONE from this path: a job
 * is now a set of ENTITIES wearing `MeshComponent{mesh_id, material_id}`, and
 * upload / refcount / teardown ride the standard resource-resolver machinery.
 *
 * Generation stays fully ASYNCHRONOUS. A provider first builds a cheap CPU
 * `ThumbnailSpec` (asset IDS + bounds); the service then executes it as a
 * per-frame state machine — spawn job entities + frame the camera → poll
 * `MeshInstanceReadyComponent` (the render subsystems' "instance is live and
 * visible" observation point) → **readbackTargetAsync** → encode (CPU pool) →
 * createTexture2D (display texture) → destroy the job entities — driven from
 * `tick()` (run with the editor frame OPEN). No step ever blocks the UI
 * thread on a render round-trip.
 *
 * If an asset already carries an embedded thumbnail payload it is reused (only
 * the GPU display-texture upload runs). One job is processed at a time.
 */

#include <lux/engine/editor/thumbnail/ThumbnailSpecProvider.hpp>
#include <lux/engine/editor/thumbnail/ThumbnailSet.hpp>
#include <lux/engine/runtime/assets/AssetLoadService.hpp>

#include <lux/engine/resource/asset/Asset.hpp>   // asset_id_t, EAssetType
#include <lux/engine/function/render/client/core/RenderResourceHandle.hpp>   // RTextureHandle (deferred destroys)

#include <imgui.h>                      // ImTextureID
#include <uuid.h>                       // std::hash<uuids::uuid>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lux::render { class RenderFrameSession; }
namespace lux::asset  { class AssetManager; }
namespace lux::exec   { class AsyncScope; class AsyncRuntime; }

namespace lux::editor
{
    struct EditorRenderInfra;   // process-domain plan/registry

    class ThumbnailService
    {
    public:
        ThumbnailService(lux::asset::AssetManager& assets,
                         lux::render::RenderFrameSession& session,
                         const EditorRenderInfra& render_infra,
                         lux::asset_runtime::AssetClient asset_client,
                         lux::exec::AsyncRuntime& async);
        ~ThumbnailService();

        ThumbnailService(const ThumbnailService&)            = delete;
        ThumbnailService& operator=(const ThumbnailService&) = delete;

        /// Blocking one-time bring-up: offscreen target + the private preview
        /// SceneRuntime (light/camera entities, settled view) + the provider
        /// registry. Call once at editor startup, before the main loop.
        /// Returns false if the preview runtime could not be brought up
        /// (thumbnails then stay off).
        [[nodiscard]] bool initialize(std::uint32_t render_size = 256);

        /// Request the thumbnail for @p id (of @p type). Returns a drawable
        /// `ImTextureID` once ready, else 0 (and enqueues a job on first call).
        ImTextureID requestThumbnail(const lux::asset::asset_id_t& id, lux::asset::EAssetType type);

        /// 作废 @p id 的缩略图(资产被删除/重导入/原地重存):归还 Ready 条目携带
        /// 的显示纹理并丢表项 —— 下次 requestThumbnail 重新生成(资产已删则生成
        /// 失败,面板回落类型字形)。**随时可调**:帧关着时纹理归还延迟到下一次
        /// tick(材质编辑器的 Save 在 paint 期调,就是这个场景)。
        void invalidate(const lux::asset::asset_id_t& id);

        /// The finished multi-size PNG set for @p id, or null while not Ready.
        /// (The GPU test decodes + inspects pixels through this.)
        [[nodiscard]] const ThumbnailSet* readySet(const lux::asset::asset_id_t& id) const;

        /// Tick the preview SceneRuntime one frame, then advance the in-flight
        /// generation job by one non-blocking step. MUST be called with the
        /// editor's render frame OPEN (the runtime's render subsystems record
        /// into the live builder; the job polls prior replies).
        void tick();

        /// Tear down the preview SceneRuntime + destroy the offscreen target.
        /// MUST run while the render thread is still serving frames (it issues
        /// render commands) — the editor calls it BEFORE stopping the thread.
        /// A job whose readback / display upload is still in flight is parked
        /// (its buffers must outlive the server); `shutdown()` reaps it after
        /// the thread stopped. Idempotent.
        [[nodiscard]] bool releaseGpu();

        /// Drop the cache + abort any in-flight job bookkeeping. Call AFTER
        /// the render thread stopped (an in-flight async readback writes into
        /// a job-owned dst buffer when its fence signals, so that buffer must
        /// outlive the server).
        void shutdown();

    private:
        // In-flight job state (the async state machine). Defined in the .cpp to
        // keep entity ids + render requests out of this header.
        struct Job;
        // The private SceneRuntime + its resident entities + the pack registry.
        // Defined in the .cpp so this header (included by LuxEditor/AssetBrowser)
        // stays free of scene-runtime / ecs includes.
        struct RuntimeHost;
        // Wraps the main-thread thumbnail state table (Pending/Ready/
        // Failed + the drawable result). Defined in the .cpp so stdexec stays out of
        // this header — ThumbnailService.hpp is included by LuxEditor/AssetBrowser,
        // which must NOT inherit the /permissive- /Zc switches stdexec needs.
        struct CacheImpl;

        void startNextJob();
        void advanceJob();              ///< one non-blocking step of `active_`
        void finishJob(bool ok);        ///< write cache + clear `active_`
        void destroyJobEntities(Job& job); ///< kill the job's world entities (leave
                                        ///  observers reclaim instance + refcounts)
        /// Watchdog: give up on `active_` (a wait stage that never settles must
        /// not starve the whole queue) — mark Failed, log the stuck wait, park
        /// the job as a zombie only if an outstanding readback/display reply
        /// still writes into job-owned buffers.
        void abortStuckJob();
        /// Destroy parked jobs whose outstanding requests have all settled,
        /// freeing a display texture that resolved after the abort.
        void drainZombies();

        lux::asset::AssetManager&   assets_;
        lux::render::RenderFrameSession& session_;
        const EditorRenderInfra&    infra_;           ///< process-domain plan/registry
        lux::asset_runtime::AssetClient asset_client_;
        lux::exec::AsyncRuntime&     async_;
        ThumbnailSpecProviderRegistry providers_;

        std::unique_ptr<RuntimeHost>                      host_;    ///< preview SceneRuntime (see RuntimeHost)
        std::unique_ptr<CacheImpl>                        cache_;   ///< State cache (see CacheImpl)
        /// Pending requests carry their type (the cache no longer stores a Pending type).
        std::vector<std::pair<lux::asset::asset_id_t,
                              lux::asset::EAssetType>>     queue_;
        std::vector<ThumbnailSize>                        sizes_;
        std::unique_ptr<Job>                              active_;
        /// Aborted-but-unsettled jobs (see abortStuckJob).
        std::vector<std::unique_ptr<Job>>                 zombies_;
        /// W2b: per-id Spec-stage retry counter. A job whose spec is `pending`
        /// (a dependency is a data-less shell still streaming in) is re-queued
        /// rather than failed; this bounds those retries so a never-arriving
        /// dependency eventually fails the entry instead of spinning forever.
        std::unordered_map<lux::asset::asset_id_t, int>   pending_attempts_;
        bool ready_{false};

        /// Declared last so its destructor is the first member backstop:
        /// encodes are joined while Job/cache storage still exists. Normal
        /// shutdown closes it explicitly for readable phase ordering.
        std::unique_ptr<lux::exec::AsyncScope> async_scope_;

        static constexpr std::uint32_t kDisplaySize  = 128;
        static constexpr std::uint32_t kSettleFrames = 4; ///< server-side readback settle
        /// Frames a single job may live before the watchdog skips it (~10s at
        /// 60fps — generous for the largest texture, far below "forever").
        static constexpr int           kJobFrameBudget = 600;
        /// Spec-stage re-queue cap for `pending` jobs (deps still streaming).
        /// ~10s at 60fps: a dependency that never arrives fails the thumbnail
        /// instead of re-queueing forever.
        static constexpr int           kPendingRetryBudget = 600;
    };

} // namespace lux::editor
