#pragma once
/**
 * @file ParamBridge.hpp
 * @brief Generic bridge for PARAM components (Grid, Skybox): component fields →
 *        a feature's SetParams op, dirty-diffed. Written once; the per-component
 *        EcsRenderTraits<C> supplies only `feature` name / `Ops` / `Payload` /
 *        `extract` / `push`. Lifecycle (resolve feature + view + bit-equal dirty
 *        compare + reap dead) lives here.
 */

#include <cstring>
#include <optional>
#include <unordered_map>

#include <lux/engine/meta/LuxObject.hpp>   // entity_id / EntityRegistry

#include "lux/engine/gameplay/render_bridge/IRenderableBridge.hpp"
#include "lux/engine/gameplay/render_bridge/RenderableBridgeContext.hpp"
#include "lux/engine/gameplay/render_bridge/EcsRenderTraits.hpp"

namespace lux::gameplay
{
    template <class C>
    class ParamBridge final : public IRenderableBridge
    {
        using T = EcsRenderTraits<C>;
        std::unordered_map<lux::meta::entity_id, typename T::Payload> last_;

    public:
        void drive(lux::meta::EntityRegistry& reg, RenderableBridgeContext& ctx) override
        {
            const auto feat = ctx.features().handle(T::feature);
            if (!feat.valid()) return;   // feature absent in this scene → no-op (graceful)
            const auto ops = ctx.features().template ops<typename T::Ops>(T::feature);

            reg.template view<C>().each([&](lux::meta::entity_id e, const C& c)
            {
                std::optional<typename T::Payload> p = T::extract(c, ctx, feat);
                if (!p) return;                          // not ready this frame (e.g. texture pending) → skip
                if (auto it = last_.find(e);
                    it != last_.end() && std::memcmp(&it->second, &*p, sizeof(*p)) == 0)
                    return;                              // unchanged → steady state, emit nothing
                T::push(ctx.session(), ops, *p);
                last_[e] = *p;
            });
        }

        void reap(lux::meta::EntityRegistry& reg, RenderableBridgeContext& /*ctx*/) override
        {
            std::erase_if(last_, [&](const auto& kv)
            {
                return !reg.valid(kv.first) || !reg.template all_of<C>(kv.first);
            });
        }

        void shutdown([[maybe_unused]] RenderableBridgeContext& ctx) override
        {
            // Optional per-trait render-state reset (e.g. Skybox nil clears its bound
            // texture) — G-06 defines Traits::clear; absent → nothing to undo on the
            // render side. Then drop the dirty-diff cache so a re-driven scene re-pushes.
            if constexpr (requires { T::clear(ctx); }) T::clear(ctx);
            last_.clear();
        }
    };

} // namespace lux::gameplay
