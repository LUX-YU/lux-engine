#pragma once
/**
 * @file TextureAtlas.hpp
 * @brief Pure-data descriptions for traditional 2D: image ATLAS (named
 *        sub-rects of one texture) and image ANIMATION CLIP (a frame
 *        sequence over an atlas). A2-00 / A2-01.
 *
 * Both are pure metadata — the pixels live in independently loadable assets.
 * Cross-Asset relations use the stable Resource identity directly; storage
 * codecs may still encode that identity as the same compact 16-byte UUID.
 *
 * The two types share one header deliberately: a clip is meaningless without
 * the atlas vocabulary, and they are always consumed together by the 2D kit.
 */

#include <lux/engine/resource/identity/AssetId.hpp>

#include <Eigen/Core>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lux::rdesc
{
    // ── A2-00: image atlas ─────────────────────────────────────────────────

    /// One named frame: a uv sub-rect (x, y, w, h in [0,1]) + a normalised
    /// pivot — exactly the fields Image2DComponent consumes (uv_rect / pivot),
    /// so applying a frame is a plain copy, no maths.
    struct AtlasFrame
    {
        std::string name;
        Eigen::Vector4f uv_rect{0.f, 0.f, 1.f, 1.f};
        Eigen::Vector2f pivot{0.5f, 0.5f};
    };

    /// A texture partitioned into named frames. Frame order is stable and
    /// load-bearing (FlipbookClip indexes frames by ordinal).
    struct TextureAtlas
    {
        std::string name;
        lux::asset::AssetId texture; ///< the independently loadable TextureAsset
        std::vector<AtlasFrame> frames;

        /// Linear scan by name (atlases hold tens of frames; a map is not
        /// worth its allocation). Null when absent.
        [[nodiscard]] const AtlasFrame* findFrame(std::string_view frame_name) const noexcept
        {
            for (const auto& f : frames)
                if (f.name == frame_name)
                    return &f;
            return nullptr;
        }
    };

    // ── A2-01: image animation clip ────────────────────────────────────────

    /// One animation step: which atlas frame (ordinal into TextureAtlas::frames)
    /// and for how long it is shown.
    struct FlipbookFrame
    {
        std::uint32_t frame_index{0};
        float duration{0.1f}; ///< seconds; must be > 0
    };

    /// A gameplay marker attached to a step: fires when playback ENTERS that
    /// step (footstep sounds, hit windows, ...). `event_id` is game-defined.
    struct FlipbookEvent
    {
        std::uint32_t frame_index{0}; ///< ordinal into FlipbookClip::frames
        std::uint32_t event_id{0};
    };

    /// A frame sequence over ONE atlas. NOT the skeletal AnimationClip — the
    /// two share nothing (design §3B.4: never reuse ANIMATION_CLIP for 2D).
    struct FlipbookClip
    {
        std::string name;
        lux::asset::AssetId atlas; ///< the independently loadable TextureAtlasAsset
        std::vector<FlipbookFrame> frames;
        std::vector<FlipbookEvent> events;
        bool loop{true};

        [[nodiscard]] float totalDuration() const noexcept
        {
            float t = 0.f;
            for (const auto& f : frames)
                t += f.duration;
            return t;
        }
    };

} // namespace lux::rdesc
