#pragma once

#include <lux/engine/description/UiImageToken.hpp>
#include <lux/engine/render/runtime/TargetId.hpp>
#include <lux/engine/render/runtime/Target.hpp>

namespace lux::editor
{
    // A UI image token selects a sampled offscreen Target, not a native descriptor or pointer.
    // Its producer is ordered before the UI by the Render Graph's actual Target dependency.
    struct UiTargetImage final
    {
        rdesc::UiImageToken token;
        render::TargetId target;
        render::TargetDesc description;
        bool operator==(const UiTargetImage&) const noexcept = default;
    };
} // namespace lux::editor
