#pragma once

#include <lux/engine/resource/spatial/Spatial.hpp>

#include <cstdint>
#include <vector>

namespace lux::ecs
{
    /// Domain-owned description of one immutable static heightfield.
    ///
    /// `origin` is the registry-space position of sample (0, 0). Quantized
    /// samples cover the inclusive `[height_min, height_max]` interval and
    /// are laid out row-major as `sample_edge * sample_edge` values. Cooked
    /// section/cell identities belong to the adapter that creates this value
    /// and never enter the physics domain.
    struct StaticHeightfield3D final
    {
        lux::spatial::Position3D origin;
        std::uint32_t sample_edge{0u};
        float sample_spacing{1.0f};
        float height_min{0.0f};
        float height_max{1.0f};
        std::vector<std::uint16_t> samples;
    };

    /// Atomic preparation input for a group of static 3D heightfields.
    /// The batch owns every sample buffer; callers may release their cooked
    /// source after background preparation has taken ownership of this value.
    struct StaticHeightfieldBatch3D final
    {
        std::vector<StaticHeightfield3D> heightfields;
    };
} // namespace lux::ecs
