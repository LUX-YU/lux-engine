#include <lux/engine/editor/EditorTextureCache.hpp>

#include <lux/engine/asset/AssetManager.hpp>
#include <lux/engine/asset/TextureAsset.hpp>
#include <lux/engine/render/comm/client/RenderSession.hpp>
#include <lux/engine/description/Texture.hpp>   // rdesc::Texture, isCompressedFormat

#include <lux/engine/execution/EngineExecutor.hpp>
#include <lux/engine/execution/EngineExecutorSenders.hpp>   // cpuScheduler/mainScheduler/spawn
#include <lux/engine/execution/RenderRequestSender.hpp>     // asSender
#include <lux/engine/execution/AsyncCache.hpp>

#include <stdexec/execution.hpp>

#include <cstddef>
#include <thread>
#include <utility>
#include <vector>

namespace lux::editor
{
    namespace ex = ::stdexec;

    // ── pimpl: keeps stdexec out of EditorTextureCache.hpp ──────────────────
    struct EditorTextureCache::Impl
    {
        lux::render::RenderSession*               session;
        std::shared_ptr<lux::asset::AssetManager> mgr;
        lux::exec::EngineExecutor*                exec;
        lux::exec::AsyncCache<lux::asset::asset_id_t, std::uint32_t> cache;  // V = bindless index
        std::vector<lux::asset::asset_id_t>       queue;     // resolve() enqueues, tick() drains
        int                                       in_flight{0};  // main-thread-only counter

        Impl(lux::render::RenderSession& s,
             std::shared_ptr<lux::asset::AssetManager> m,
             lux::exec::EngineExecutor& e)
            : session(&s), mgr(std::move(m)), exec(&e), cache(e) {}

        std::uint32_t resolve(const lux::asset::asset_id_t& id)
        {
            if (id.is_nil()) return 0;
            if (const auto* idx = cache.tryGet(id)) return *idx;            // ready
            if (cache.state(id) == lux::exec::CacheState::Failed) return 0; // failed -> default, no retry
            if (cache.markPending(id))                                      // first sighting -> enqueue once
                queue.push_back(id);
            return 0;   // pending -> default texture until the upload lands
        }

        void tick()
        {
            if (queue.empty()) return;

            std::vector<lux::asset::asset_id_t> todo;
            todo.swap(queue);

            for (const auto& id : todo)
            {
                // Guard: an invalidate()/late state change between enqueue and now.
                if (cache.state(id) != lux::exec::CacheState::Pending) continue;

                auto* ta = mgr ? mgr->fetchAssetAs<lux::asset::TextureAsset>(id) : nullptr;
                if (ta == nullptr || ta->data() == nullptr) { cache.setFailed(id); continue; }
                const lux::rdesc::Texture& tex = *ta->data();

                // Issue the GPU upload (frame-OPEN) and spawn the store pipeline on the
                // executor. asSender's set_value lands on the main thread at pumpReplies;
                // continues_on(mainScheduler) bounces the store to drainMain so it runs at a
                // well-defined point. The store is a map write only — no frame-OPEN need —
                // but we route it through mainScheduler for a uniform "stores land on drainMain"
                // invariant across Phase C.
                ++in_flight;
                lux::exec::spawn(*exec,
                      lux::exec::asSender(session->createTexture2D(
                          static_cast<const std::byte*>(tex.data()),
                          static_cast<std::uint32_t>(tex.size()),
                          tex.width(), tex.height(), tex.channel(), tex.pixelFormat(),
                          /*generate_mips=*/!lux::rdesc::isCompressedFormat(tex.pixelFormat())))
                    | ex::continues_on(lux::exec::mainScheduler(*exec))
                    | ex::then([this, id](const lux::render::Texture2DCreatedReply& r) noexcept
                      {
                          if (r.status == 0) cache.setReady(id, r.handle.index);
                          else               cache.setFailed(id);
                          --in_flight;
                      })
                    | ex::upon_stopped([this, id]() noexcept   // shutdown cancel of the main hop
                      {
                          cache.setFailed(id);
                          --in_flight;
                      }));
            }
        }

        void shutdown()
        {
            // The upload ops live on the executor's shared async_scope. asSender is NOT
            // cancellable, so request_stop won't wake a parked upload — it completes only
            // when its reply arrives via pumpReplies. Pump the session + the executor main
            // queue until our in-flight ops settle, so the executor's later
            // scope.on_empty() can't hang on a parked upload. MUST run while `session` is
            // alive. Bounded spin guards a pathological never-arriving reply.
            for (int spins = 0; spins < 1'000'000 && in_flight > 0; ++spins)
            {
                session->pumpReplies();
                exec->drainMain();
                std::this_thread::yield();
            }
        }
    };

    EditorTextureCache::EditorTextureCache(lux::render::RenderSession& session,
                                           std::shared_ptr<lux::asset::AssetManager> mgr,
                                           lux::exec::EngineExecutor& exec)
        : impl_(std::make_unique<Impl>(session, std::move(mgr), exec))
    {
    }

    EditorTextureCache::~EditorTextureCache() = default;

    std::uint32_t EditorTextureCache::resolve(const lux::asset::asset_id_t& id)
    {
        return impl_->resolve(id);
    }

    void EditorTextureCache::tick()     { impl_->tick(); }
    void EditorTextureCache::shutdown() { impl_->shutdown(); }

} // namespace lux::editor
