#pragma once
// ============================================================================
//  FlipbookAnimationComponent.hpp — 2D frame-animation playback state (A2-01).
//
//  Mirrors the 3D Animator/AnimatorCache split: FlipbookAnimationComponent is the
//  authored playback state (which clip, cursor, speed); FlipbookAnimCacheComponent
//  is the RUNTIME-ONLY resolution cache written by Flipbook2DResolver (the
//  app-level asset-facing half) and consumed by the pure FlipbookAnimationSystem.
//  Neither the system nor the components ever touch the AssetManager.
//
//  Events: when playback ENTERS a clip step that carries FlipbookEvents, their
//  event_ids are appended to `events_this_frame` (cleared by the system at the
//  start of each update) — game code reads them after Schedule::tick.
// ============================================================================

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/resource/asset/Asset.hpp>          // lux::asset::asset_id_t
#include <lux/engine/resource/asset/AssetRef.hpp>       // 驻留票(runtime-only cache 持有)
#include <lux/engine/description/TextureAtlas.hpp>

#include <cstdint>
#include <vector>

namespace lux::ecs
{
    struct LUX_COMPONENT() FlipbookAnimationComponent
    {
        /// The FLIPBOOK_CLIP asset to play. Nil → the image keeps whatever
        /// uv/pivot it has (the direct-authoring path stays untouched).
        LUX_MEMBER(display_name=Clip, tooltip=Flipbook clip asset)
        lux::asset::asset_id_t clip{};

        LUX_MEMBER(display_name=Time, tooltip=Playback cursor in seconds)
        float time{0.f};

        LUX_MEMBER(display_name=Speed, tooltip=Playback speed multiplier)
        float speed{1.f};

        LUX_MEMBER(display_name=Playing, tooltip=Whether the cursor advances)
        bool playing{true};

        /// Event ids fired THIS frame (steps entered this update). System-owned:
        /// cleared each update; not reflected/persisted.
        std::vector<std::uint32_t> events_this_frame;
    };

    /// Runtime-only resolution cache (Flipbook2DResolver writes it every frame;
    /// re-queried per frame so an evicted asset resolves to null and the system
    /// bails gracefully). Never authored, never persisted.
    /// (本 struct 无 LUX_COMPONENT 注解 —— 生成器按 class 级注解取,同头文件里
    ///  被反射的 FlipbookAnimationComponent 不受影响。)
    struct FlipbookAnimCacheComponent
    {
        lux::asset::asset_id_t              clip_id{};    ///< id the pointers below were resolved for
        const lux::rdesc::FlipbookClip*     clip{nullptr};
        const lux::rdesc::TextureAtlas*     atlas{nullptr};
        /// Which clip step is currently applied to the Image2DComponent
        /// (UINT32_MAX = none yet — force an apply on first sight).
        std::uint32_t                       applied_step{0xFFFFFFFFu};

        /// 驻留票(见 AnimatorCacheComponent 同名字段的说明)。atlas 的票**保留到
        /// clip 换装**而不随 clip 驱逐松手:clip 被驱逐的帧读不到 atlas_uuid,
        /// 若因此还票,会造成 clip↔atlas 交替驱逐-重载的抖动。
        lux::asset::AssetRef clip_ref{};
        lux::asset::AssetRef atlas_ref{};
    };

} // namespace lux::ecs
