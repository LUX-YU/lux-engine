#pragma once
/**
 * @file PoolBridge.hpp
 * @brief Generic bridge for POOL components (Directional/Point/Spot Light): each
 *        entity owns a pooled render object created/updated/destroyed by handle.
 *        Written once; EcsRenderTraits<C> supplies `Desc` / `Handle` / `Reply` /
 *        `feature` / `Ops` / `extract` / `create·update·destroy` / `handle`. The
 *        async create-pending + bit-equal update-on-change + reap-destroy lifecycle
 *        lives here.
 */

#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

#include <lux/engine/meta/LuxObject.hpp>   // entity_id / EntityRegistry
#include <lux/engine/render/comm/client/RenderRequest.hpp>   // in-flight create handle (cancel on teardown)

#include "lux/engine/gameplay/render_bridge/IRenderableBridge.hpp"
#include "lux/engine/gameplay/render_bridge/RenderableBridgeContext.hpp"
#include "lux/engine/gameplay/render_bridge/EcsRenderTraits.hpp"

namespace lux::gameplay
{
    template <class C>
    class PoolBridge final : public IRenderableBridge
    {
        using T = EcsRenderTraits<C>;
        struct Live { typename T::Handle handle{}; typename T::Desc last_sent{}; };
        std::unordered_map<lux::meta::entity_id, Live> live_;
        // create reply in flight, keyed by entity → the request handle. We hold the
        // handle (not just a flag) so the dtor can CANCEL any still-pending create:
        // its reply may be pumped after this bridge + its context are destroyed
        // (replies share one queue; an unrelated pumpReplies can dispatch it),
        // and the continuation captures `this`. Cancel detaches the continuation
        // so that late reply is dropped; the created render object is reclaimed by
        // the reap / scene-destruction fallback.
        std::unordered_map<lux::meta::entity_id,
                           lux::render::RenderRequest<typename T::Reply>> pending_;

        // Teardown drain (see IRenderableBridge two-phase teardown): once stopping_,
        // a late create reply routes its server-created handle into orphans_ instead
        // of becoming live; flushShutdownCleanup destroys them.
        bool                            stopping_{false};
        std::vector<typename T::Handle> orphans_;

    public:
        ~PoolBridge() override
        {
            // Safety net only: a correct teardown DRAINS pending (beginShutdown +
            // pump), so pending_ is empty here. If the caller skipped the drain, cancel
            // detaches late continuations to avoid UAF — at the cost of a server orphan.
            for (auto& [e, req] : pending_) req.cancel();
        }

        void drive(lux::meta::EntityRegistry& reg, RenderableBridgeContext& ctx) override
        {
            const auto ops = ctx.features().template ops<typename T::Ops>(T::feature);

            // Require/Exclude companions (Point/Spot require WorldTransformComponent).
            auto view = componentView<C>(reg, typename T::Require{}, typename T::Exclude{});
            view.each([&](lux::meta::entity_id e, const C& c, auto&&...)
            {
                typename T::Desc d = T::extract(e, c, reg);   // Point/Spot pull WorldTransform from reg

                if (auto it = live_.find(e); it != live_.end())
                {
                    if (std::memcmp(&it->second.last_sent, &d, sizeof(d)) != 0)
                    {
                        T::update(ctx.session(), ops, ctx.scene(), it->second.handle, d);
                        it->second.last_sent = d;
                    }
                    return;
                }
                if (pending_.count(e)) return;                // createLight reply still in flight

                auto req = T::create(ctx.session(), ops, ctx.scene(), d);
                req.then(
                    [this, e, d](const typename T::Reply& r)
                    {
                        pending_.erase(e);                    // drops this request; the store keeps
                                                              // the state alive across this call
                        // Validate the HANDLE too, not just status (mirrors InstanceBridge's
                        // `|| !r.object`): a generic dispatch failure delivers a DEFAULT reply
                        // {null handle, status 0} — status-only would emplace a null-handle
                        // zombie into live_ that never renders and never retries (P1-3).
                        if (r.status != 0 || T::handle(r).is_null()) return;   // failed; entity stays absent → retries
                        if (stopping_)                        // late reply during teardown drain →
                        { orphans_.push_back(T::handle(r)); return; }   // destroy in flushShutdownCleanup
                        live_.emplace(e, Live{T::handle(r), d});
                    });
                pending_.emplace(e, std::move(req));
            });
        }

        void reap(lux::meta::EntityRegistry& reg, RenderableBridgeContext& ctx) override
        {
            const auto ops = ctx.features().template ops<typename T::Ops>(T::feature);
            // Full Require/Exclude membership, matching InstanceBridge::reap. The old
            // `valid && all_of<C>` check ignored the trait's Require/Exclude lists, so a
            // Point/Spot light that shed its WorldTransform (a Require) or gained an
            // Exclude tag stayed live → a zombie render object. `inComponentView` is the
            // single source of truth for the same set `drive`'s view iterates (G-01).
            for (auto it = live_.begin(); it != live_.end(); )
            {
                if (!inComponentView<C>(reg, it->first, typename T::Require{}, typename T::Exclude{}))
                {
                    T::destroy(ctx.session(), ops, ctx.scene(), it->second.handle);
                    it = live_.erase(it);
                }
                else ++it;
            }
            // Pending-create passthrough (entity left the view before its create reply
            // arrived) self-heals: the reply still emplaces into live_, and the next reap
            // tears it down via the check above — same as InstanceBridge. No cancel here,
            // which would risk a server-side object with no client handle to reclaim it.
        }

        void beginShutdown(RenderableBridgeContext& ctx) override
        {
            const auto ops = ctx.features().template ops<typename T::Ops>(T::feature);
            for (auto& [e, live] : live_)
                T::destroy(ctx.session(), ops, ctx.scene(), live.handle);
            live_.clear();
            stopping_ = true;   // keep pending_; late create replies route to orphans_ (drained then flushed)
        }

        [[nodiscard]] bool hasPendingShutdownWork() const override { return !pending_.empty(); }

        void flushShutdownCleanup(RenderableBridgeContext& ctx) override
        {
            const auto ops = ctx.features().template ops<typename T::Ops>(T::feature);
            for (const auto& h : orphans_)   // objects the drained late replies created while stopping
                T::destroy(ctx.session(), ops, ctx.scene(), h);
            orphans_.clear();
        }
    };

} // namespace lux::gameplay
