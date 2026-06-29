#pragma once
/**
 * @file ThumbnailService.hpp
 * @brief Editor-resident orchestrator for asset thumbnails (non-blocking).
 *
 * Owns the shared `PreviewScene` + the type→renderer registry + a memory cache
 * keyed by asset id. The AssetBrowser (grid mode) calls `requestThumbnail()`
 * while drawing; the first request enqueues a job and `tick()` drives it.
 *
 * Generation is fully ASYNCHRONOUS. A renderer first builds a cheap CPU
 * `ThumbnailSpec`; the service then executes it as a per-frame state machine —
 * uploadMesh / compileShader + uploadGraphMaterial → addMeshInstance →
 * makeInstanceVisibleForView + updateView + **readbackViewAsync** → encode →
 * createTexture2D — by POLLING
 * each step's `RenderRequest` from `tick()` (run with the editor frame OPEN).
 * No step ever blocks the UI thread on a render round-trip. The server settles
 * + reads back the offscreen view and notifies via a deferred reply.
 *
 * If an asset already carries an embedded thumbnail payload it is reused (only
 * the GPU display-texture upload runs). One job is processed at a time.
 */

#include <lux/engine/editor/thumbnail/PreviewScene.hpp>
#include <lux/engine/editor/thumbnail/ThumbnailSpecProvider.hpp>
#include <lux/engine/editor/thumbnail/ThumbnailSet.hpp>

#include <lux/engine/asset/Asset.hpp>   // asset_id_t, EAssetType

#include <imgui.h>                      // ImTextureID
#include <uuid.h>                       // std::hash<uuids::uuid>

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lux::render { class RenderSession; }
namespace lux::asset  { class AssetManager; }
namespace lux::exec   { class EngineExecutor; }

namespace lux::editor
{
    class ThumbnailService
    {
    public:
        ThumbnailService(lux::asset::AssetManager& assets,
                         lux::render::RenderSession& session,
                         lux::exec::EngineExecutor& exec);
        ~ThumbnailService();

        ThumbnailService(const ThumbnailService&)            = delete;
        ThumbnailService& operator=(const ThumbnailService&) = delete;

        /// Blocking one-time bring-up (PreviewScene::setup + renderer registry).
        /// Call once at editor startup, before the main loop. Returns false if
        /// the preview scene could not be created (thumbnails then stay off).
        [[nodiscard]] bool initialize(std::uint32_t render_size = 256);

        [[nodiscard]] bool ready() const noexcept { return ready_; }

        /// Request the thumbnail for @p id (of @p type). Returns a drawable
        /// `ImTextureID` once ready, else 0 (and enqueues a job on first call).
        ImTextureID requestThumbnail(const lux::asset::asset_id_t& id, lux::asset::EAssetType type);

        /// Advance the in-flight generation job by one non-blocking step. MUST
        /// be called with the editor's render frame OPEN (it pushes render
        /// commands into the live builder and polls prior replies).
        void tick();

        /// Drop the cache + abort any in-flight job. Call before the render
        /// thread stops.
        void shutdown();

    private:
        // In-flight job state (the async state machine). Defined in the .cpp to
        // keep the render-request menagerie out of this header.
        struct Job;
        // Per-distinct-graph-material bring-up inside a job (compile + textures +
        // upload). Also .cpp-private.
        struct GraphMatBringup;
        // Wraps the lux::exec::AsyncCache<asset_id_t, ThumbValue> (Pending/Ready/
        // Failed + the drawable result). Defined in the .cpp so stdexec stays out of
        // this header — ThumbnailService.hpp is included by LuxEditor/AssetBrowser,
        // which must NOT inherit the /permissive- /Zc switches stdexec needs.
        struct CacheImpl;

        void startNextJob();
        void advanceJob();              ///< one non-blocking step of `active_`
        void finishJob(bool ok);        ///< write cache + clear `active_`
        void cleanupJobResources();     ///< remove instances + destroy transient handles
        /// Watchdog: give up on `active_` (a wait stage that never settles must
        /// not starve the whole queue) — mark Failed, log the stuck wait, park
        /// the job as a zombie (outstanding replies still write into job-owned
        /// buffers, so it cannot be destroyed yet).
        void abortStuckJob();
        /// Destroy parked jobs whose outstanding requests have all settled,
        /// freeing any handles that resolved after the abort.
        void drainZombies();
        /// Find/create the bring-up for a graph-material asset id (issues its
        /// forward-frag compile + texture uploads once); nullopt => use default grey.
        std::optional<std::size_t>
        ensureJobGraphMaterial(Job& job, const lux::asset::asset_id_t& mat_id);

        lux::asset::AssetManager&   assets_;
        lux::render::RenderSession& session_;
        lux::exec::EngineExecutor*  exec_{nullptr};   ///< CPU pool for the Encode offload (C5b)
        PreviewScene                preview_;
        ThumbnailSpecProviderRegistry providers_;

        std::unique_ptr<CacheImpl>                        cache_;   ///< AsyncCache (see CacheImpl)
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
