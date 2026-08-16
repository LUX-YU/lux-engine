/**
 * @file FlipbookAnimationSystem.cpp
 * @brief Pure 2D frame-animation sampler (A2-01) — see the header for the
 *        resolver/system split and ordering contract.
 */

#include <lux/engine/ecs/animation/systems/FlipbookAnimationSystem.hpp>
#include <lux/engine/ecs/animation/components/FlipbookAnimationComponent.hpp>
#include <lux/engine/ecs/render/components/2d/Image2DComponent.hpp>
#include <lux/engine/ecs/render/subsystems/2d/TextureAtlas2D.hpp>

// Sampling machinery lives in the animation capability module (ADR split:
// pure clip×time functions there, the component-writing APPLY side here).
#include <lux/engine/animation/FlipbookSampling.hpp>

namespace lux::ecs
{
    namespace
    {
        using lux::animation::sampleFlipbookStep;
        using lux::animation::appendFlipbookStepEvents;
    }

    FlipbookAnimationSystem::FlipbookAnimationSystem() noexcept = default;
    FlipbookAnimationSystem::~FlipbookAnimationSystem()         = default;

    void FlipbookAnimationSystem::update(const lux::ecs::SystemUpdateContext& ctx)
    {
        auto& registry = ctx.registry();
        const float dt = ctx.dt();
        auto view = registry.view<FlipbookAnimationComponent, FlipbookAnimCacheComponent, Image2DComponent>();
        view.each([dt, &registry](lux::meta::entity_id       e,
                                  FlipbookAnimationComponent& anim,
                                  FlipbookAnimCacheComponent& cache,
                                  Image2DComponent&          sp)
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

            const std::uint32_t step = sampleFlipbookStep(clip, anim.time, total);
            if (step == cache.applied_step)
                return;   // steady state: zero writes, zero events

            // Fire events for every step ENTERED since the last applied one
            // (a slow frame can skip steps; their events must not be lost).
            // Walk forward in clip order, wrapping when the clip loops.
            const auto n = static_cast<std::uint32_t>(clip.frames.size());
            if (cache.applied_step == 0xFFFFFFFFu)
            {
                appendFlipbookStepEvents(clip, step, anim.events_this_frame);   // first sight
            }
            else
            {
                std::uint32_t i = cache.applied_step;
                // Bounded by n: at most one full lap of events per update.
                for (std::uint32_t hops = 0; hops < n; ++hops)
                {
                    i = (i + 1u) % n;
                    appendFlipbookStepEvents(clip, i, anim.events_this_frame);
                    if (i == step) break;
                    if (!clip.loop && i + 1u == n) break;   // clamped end
                }
            }

            // Apply the frame — an out-of-range atlas ordinal is content error:
            // skip the apply (image keeps its last uv), never sample garbage.
            const std::uint32_t atlas_ord = clip.frames[step].frame_index;
            if (atlas_ord < cache.atlas->frames.size())
            {
                applyAtlasFrame(sp, *cache.atlas, cache.atlas->frames[atlas_ord]);
                // ⚠️ 上面是**直写**组件字段,不发 `on_update`。而 Image2D 的抽取
                //   已经是变更驱动的(批 R4),没有信号 = 那一帧的 uv 永远送不到
                //   GPU,症状是「精灵动画卡住」。这里补发一次。
                //   只在帧**真的变了**时走到这里(上面的 `step == applied_step`
                //   提前返回),所以稳态零信号,与本系统「零写入零事件」一致。
                //   由 Image2D 的抽取 oracle 在 pixel_world 上抓出。
                registry.patch<Image2DComponent>(e);
            }
            cache.applied_step = step;
        });
    }

} // namespace lux::ecs
