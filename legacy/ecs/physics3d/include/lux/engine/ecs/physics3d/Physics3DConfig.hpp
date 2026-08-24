#pragma once

#include <Eigen/Core>

#include <cstdint>

namespace lux::ecs
{
    struct Physics3DConfig final
    {
        Eigen::Vector3f gravity{0.0f, -9.81f, 0.0f};
        float fixed_dt{1.0f / 60.0f};
        float max_accumulated{0.25f};
        std::uint32_t max_substeps{4u};
        std::uint32_t maximum_bodies{131072u};
        std::uint32_t maximum_body_pairs{262144u};
        std::uint32_t maximum_contact_constraints{65536u};
        /// Jolt frame scratch storage. This is fixed-capacity by design so a
        /// pathological contact frame cannot fall back to unbudgeted heap
        /// allocations. The previous 32 MiB default was below Jolt's own
        /// worst-case preparation requirement for the capacities above.
        std::uint32_t temporary_allocator_bytes{64u * 1024u * 1024u};
    };
} // namespace lux::ecs
