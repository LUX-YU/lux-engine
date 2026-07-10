/**
 * @file SpriteAnimationSystem.cpp
 * @brief Pure 2D frame-animation sampler (A2-01) — see the header for the
 *        resolver/system split and ordering contract.
 */

#include <lux/engine/gameplay/2d/world/systems/SpriteAnimationSystem.hpp>
#include <lux/engine/gameplay/2d/world/components/SpriteAnimationComponent.hpp>
#include <lux/engine/gameplay/2d/world/components/SpriteComponent.hpp>
#include <lux/engine/gameplay/2d/world/SpriteAtlas2D.hpp>

#include <cmath>

namespace lux::gameplay::d2
{
    namespace
    {
        /// Map a cursor (seconds) to a clip step index. Linear walk — clips are
        /// tens of steps; a prefix-sum cache is not worth its invalidation.
        /// @pre total > 0 and !clip.frames.empty().
        std::uint32_t stepAt(const lux::rdesc::SpriteAnimClip& clip, float t, float total)
        {
            if (clip.loop)
            {
                t = std::fmod(t, total);
                if (t < 0.f) t += total;
            }
            else if (t >= total)
            {
                return static_cast<std::uint32_t>(clip.frames.size()) - 1u;   // clamp on the last step
            }
            else if (t < 0.f)
            {
                return 0u;
            }
            float acc = 0.f;
            for (std::uint32_t i = 0; i < clip.frames.size(); ++i)
            {
                acc += clip.frames[i].duration;
                if (t < acc) return i;
            }
            return static_cast<std::uint32_t>(clip.frames.size()) - 1u;       // float-edge fallback
        }

        void fireEventsForStep(const lux::rdesc::SpriteAnimClip& clip,
                               std::uint32_t step,
                               std::vector<std::uint32_t>& out)
        {
            for (const auto& e : clip.events)
                if (e.frame_index == step) out.push_back(e.event_id);
        }
    }

    SpriteAnimationSystem::SpriteAnimationSystem() noexcept = default;
    SpriteAnimationSystem::~SpriteAnimationSystem()         = default;

    void SpriteAnimationSystem::update(lux::meta::EntityRegistry& registry, float dt)
    {
        auto view = registry.view<SpriteAnimationComponent, SpriteAnimCacheComponent, SpriteComponent>();
        view.each([dt](SpriteAnimationComponent& anim,
                       SpriteAnimCacheComponent& cache,
                       SpriteComponent&          sp)
        {
            anim.events_this_frame.clear();

            // Unresolved (loading / evicted / nil clip) → skip; last uv stays.
            if (cache.clip == nullptr || cache.atlas == nullptr || cache.clip->frames.empty())
                return;
            const auto& clip  = *cache.clip;
            const float total = clip.totalDuration();
            if (!(total > 0.f))
                return;   // malformed content (the codec rejects it on load; belt-and-braces)

            if (anim.playing)
                anim.time += dt * anim.speed;

            const std::uint32_t step = stepAt(clip, anim.time, total);
            if (step == cache.applied_step)
                return;   // steady state: zero writes, zero events

            // Fire events for every step ENTERED since the last applied one
            // (a slow frame can skip steps; their events must not be lost).
            // Walk forward in clip order, wrapping when the clip loops.
            const auto n = static_cast<std::uint32_t>(clip.frames.size());
            if (cache.applied_step == 0xFFFFFFFFu)
            {
                fireEventsForStep(clip, step, anim.events_this_frame);   // first sight
            }
            else
            {
                std::uint32_t i = cache.applied_step;
                // Bounded by n: at most one full lap of events per update.
                for (std::uint32_t hops = 0; hops < n; ++hops)
                {
                    i = (i + 1u) % n;
                    fireEventsForStep(clip, i, anim.events_this_frame);
                    if (i == step) break;
                    if (!clip.loop && i + 1u == n) break;   // clamped end
                }
            }

            // Apply the frame — an out-of-range atlas ordinal is content error:
            // skip the apply (sprite keeps its last uv), never sample garbage.
            const std::uint32_t atlas_ord = clip.frames[step].frame_index;
            if (atlas_ord < cache.atlas->frames.size())
                applyAtlasFrame(sp, *cache.atlas, cache.atlas->frames[atlas_ord]);
            cache.applied_step = step;
        });
    }

} // namespace lux::gameplay::d2
