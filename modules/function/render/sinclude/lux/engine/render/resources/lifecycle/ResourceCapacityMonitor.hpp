#pragma once
/**
 * @file ResourceCapacityMonitor.hpp
 * @brief Unified capacity query interface for render resources.
 *
 * Every expandable GPU resource can report a CapacityStatus snapshot so that
 * the render server can log watermarks, suppress expansion under VRAM pressure,
 * and eventually trigger pre-emptive growth.
 */

#include <cstdint>

namespace lux::render
{
    /// Point-in-time capacity snapshot for any GPU resource pool.
    struct CapacityStatus
    {
        uint64_t used{0};          ///< Currently occupied slots / bytes
        uint64_t capacity{0};      ///< Allocated capacity (can grow up to hard_limit)
        uint64_t hard_limit{0};    ///< Absolute maximum the resource will ever reach

        [[nodiscard]] float usageRatio() const
        {
            return capacity > 0 ? static_cast<float>(used) / static_cast<float>(capacity) : 0.f;
        }

        /// True when usage exceeds 85% of allocated capacity.
        [[nodiscard]] bool nearFull(float threshold = 0.85f) const
        {
            return usageRatio() >= threshold;
        }

        /// True when no more slots / bytes can be allocated.
        [[nodiscard]] bool exhausted() const { return used >= capacity; }
    };

    /**
     * @brief Optional mix-in interface for resources that support runtime expansion.
     *
     * Implementing resources should override capacityStatus() at minimum.
     * tryExpand() is optional — not all resources support online growth.
     */
    class IExpandableResource
    {
    public:
        virtual ~IExpandableResource() = default;

        /// Return a snapshot of the current capacity utilization.
        [[nodiscard]] virtual CapacityStatus capacityStatus() const = 0;

        /// Human-readable resource name (used in logs / telemetry).
        [[nodiscard]] virtual const char* resourceName() const = 0;
    };

} // namespace lux::render
