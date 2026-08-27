#pragma once
/**
 * @file FlipbookSampling.hpp
 * @brief Pure 2D frame-clip sampling (the sampling MACHINERY of A2-01).
 *
 * Capability-module half of the image-animation split (ADR:
 * lux-engine-pack-architecture): these are pure functions of
 * (rdesc::FlipbookClip × time) — no ECS, no components, no assets.
 * The APPLY side (writing Image2DComponent.uv_rect, the events buffer, the
 * resolver) stays in the 2D pack, which knows the domain components.
 * Sibling of PoseSampling.hpp (the 3D skeletal counterpart).
 */

#include <lux/engine/description/TextureAtlas.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

namespace lux::animation
{
    /// Map a cursor (seconds) to a clip step index. Linear walk — clips are
    /// tens of steps; a prefix-sum cache is not worth its invalidation.
    /// @pre total > 0 and !clip.frames.empty().
    [[nodiscard]] inline std::uint32_t
    sampleFlipbookStep(const lux::rdesc::FlipbookClip& clip, float t, float total) noexcept
    {
        if (clip.loop)
        {
            t = std::fmod(t, total);
            if (t < 0.f)
                t += total;
        }
        else if (t >= total)
        {
            return static_cast<std::uint32_t>(clip.frames.size()) - 1u; // clamp on the last step
        }
        else if (t < 0.f)
        {
            return 0u;
        }
        float acc = 0.f;
        for (std::uint32_t i = 0; i < clip.frames.size(); ++i)
        {
            acc += clip.frames[i].duration;
            if (t < acc)
                return i;
        }
        return static_cast<std::uint32_t>(clip.frames.size()) - 1u; // float-edge fallback
    }

    /// Append the event ids attached to @p step (fired when playback ENTERS it).
    inline void
    appendFlipbookStepEvents(const lux::rdesc::FlipbookClip& clip, std::uint32_t step, std::vector<std::uint32_t>& out)
    {
        for (const auto& e : clip.events)
            if (e.frame_index == step)
                out.push_back(e.event_id);
    }

} // namespace lux::animation
