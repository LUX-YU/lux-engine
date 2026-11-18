#pragma once

#include <lux/engine/render/core/Ids.hpp>

namespace lux::render::detail
{
    // An execution-layer publication. The Target owns the image and its actual content revision.
    struct JobOutput final
    {
        TargetId target;
        std::uint32_t image_index;
        std::uint64_t target_epoch;
        std::uint64_t content_revision;
    };
} // namespace lux::render::detail
