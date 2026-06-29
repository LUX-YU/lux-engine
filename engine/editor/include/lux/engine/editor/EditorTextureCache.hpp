#pragma once
// ============================================================================
//  EditorTextureCache — resolves a texture asset UUID to a bindless texture
//  index, uploading on demand through the editor's RenderSession (the SAME
//  session the material-graph PREVIEW renders with, so the index is valid there:
//  the global TextureResources set-2 bindless array the forward graph pipeline
//  binds unconditionally).
//
//  Phase C3: the per-asset state (Pending/Ready/Failed + bindless index) now lives
//  in a lux::exec::AsyncCache, and the upload runs as a sender pipeline spawned on
//  the EngineExecutor (asSender(createTexture2D) | continues_on(mainScheduler) |
//  store) — the first real consumer of the C1/C2 primitives. stdexec is confined to
//  EditorTextureCache.cpp via a pimpl, so THIS header stays stdexec-free.
//
//  Frame discipline: resolve() is frame-CLOSED safe (cache lookup + enqueue only);
//  the createTexture2D push runs in tick() (frame-OPEN). shutdown() drains in-flight
//  uploads while the session can still pumpReplies (asSender is not cancellable).
// ============================================================================

#include <lux/engine/editor/visibility.h>
#include <lux/engine/asset/Asset.hpp>   // asset_id_t

#include <cstdint>
#include <memory>

namespace lux::asset  { class AssetManager; }
namespace lux::render { class RenderSession; }
namespace lux::exec   { class EngineExecutor; }

namespace lux::editor
{
    class LUX_EDITOR_PUBLIC EditorTextureCache
    {
    public:
        EditorTextureCache(lux::render::RenderSession& session,
                           std::shared_ptr<lux::asset::AssetManager> mgr,
                           lux::exec::EngineExecutor& exec);
        ~EditorTextureCache();

        EditorTextureCache(const EditorTextureCache&)            = delete;
        EditorTextureCache& operator=(const EditorTextureCache&) = delete;

        /// Frame-CLOSED safe. Returns the bindless index if ready, else 0 (and
        /// enqueues an upload that tick() will perform). 0 == the engine default
        /// texture until the real one is ready (a benign 1-2 frame transient).
        std::uint32_t resolve(const lux::asset::asset_id_t& id);

        /// Frame-OPEN. Drains the enqueue list: fetch the TextureAsset's pixels and
        /// spawn the upload pipeline on the executor; its reply caches the index.
        void tick();

        /// Drain in-flight uploads to quiescence. MUST be called while the
        /// RenderSession can still pumpReplies (before it is torn down) and the
        /// executor is still alive — the upload senders complete via pumpReplies
        /// and asSender is not cancellable, so a parked upload left on the
        /// executor's scope would hang its on_empty() at shutdown.
        void shutdown();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace lux::editor
