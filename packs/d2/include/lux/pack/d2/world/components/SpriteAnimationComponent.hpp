#pragma once
// ============================================================================
//  SpriteAnimationComponent.hpp — 2D frame-animation playback state (A2-01).
//
//  Mirrors the 3D Animator/AnimatorCache split: SpriteAnimationComponent is the
//  authored playback state (which clip, cursor, speed); SpriteAnimCacheComponent
//  is the RUNTIME-ONLY resolution cache written by SpriteAnim2DResolver (the
//  app-level asset-facing half) and consumed by the pure SpriteAnimationSystem.
//  Neither the system nor the components ever touch the AssetManager.
//
//  Events: when playback ENTERS a clip step that carries SpriteAnimEvents, their
//  event_ids are appended to `events_this_frame` (cleared by the system at the
//  start of each update) — game code reads them after World::tick.
// ============================================================================

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/asset/Asset.hpp>          // lux::asset::asset_id_t
#include <lux/engine/description/Sprite2D.hpp>

#include <cstdint>
#include <vector>

namespace lux::pack
{
    struct LUX_COMPONENT() SpriteAnimationComponent
    {
        /// The SPRITE_ANIM_CLIP asset to play. Nil → the sprite keeps whatever
        /// uv/pivot it has (the direct-authoring path stays untouched).
        LUX_MEMBER(display_name=Clip, tooltip=Sprite animation clip asset)
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

    /// Runtime-only resolution cache (SpriteAnim2DResolver writes it every frame;
    /// re-queried per frame so an evicted asset resolves to null and the system
    /// bails gracefully). Never authored, never persisted.
    struct SpriteAnimCacheComponent
    {
        lux::asset::asset_id_t             clip_id{};    ///< id the pointers below were resolved for
        const lux::rdesc::SpriteAnimClip*  clip{nullptr};
        const lux::rdesc::SpriteAtlas*     atlas{nullptr};
        /// Which clip step is currently applied to the SpriteComponent
        /// (UINT32_MAX = none yet — force an apply on first sight).
        std::uint32_t                      applied_step{0xFFFFFFFFu};
    };

} // namespace lux::pack
