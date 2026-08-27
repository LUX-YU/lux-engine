#pragma once
/**
 * @file TextureAtlas.hpp
 * @brief Pure-data descriptions for traditional 2D: image ATLAS (named
 *        sub-rects of one texture) and image ANIMATION CLIP (a frame
 *        sequence over an atlas). A2-00 / A2-01.
 *
 * Both are pure metadata — the pixels live in the referenced TEXTURE asset.
 * Like every rdesc type, these know nothing about the asset system: asset
 * references are carried as OPAQUE 16-byte uuid values (the asset layer
 * converts to/from the asset layer's strong soft identity); the description module keeps its
 * "no asset headers" boundary (the ImportedMaterialDesc convention).
 *
 * The two types share one header deliberately: a clip is meaningless without
 * the atlas vocabulary, and they are always consumed together by the 2D kit.
 */

#include <Eigen/Core>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lux::rdesc
{
    /// Opaque asset reference (the raw bytes of a uuid). All-zero = null.
    using OpaqueAssetId = std::array<std::uint8_t, 16>;

    [[nodiscard]] inline bool isNullOpaqueAssetId(const OpaqueAssetId& id) noexcept
    {
        for (const auto b : id)
            if (b != 0)
                return false;
        return true;
    }

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
        OpaqueAssetId texture_uuid{}; ///< the TEXTURE asset (opaque)
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
        OpaqueAssetId atlas_uuid{}; ///< the TEXTURE_ATLAS asset (opaque)
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
