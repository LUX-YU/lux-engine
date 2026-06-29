#pragma once
/**
 * @file MeshInstanceExtData.hpp
 * @brief Per-frame mesh instance data injected into RGFrameContext via extension slot.
 *
 * Replaces the hardcoded instance_slot_count field in RGFrameContext.
 * Populated by the Renderer from InstanceResources and read by
 * mesh/shadow kernel patch resolution functions.
 */

#include <lux/engine/render/graph/FrameExtensionRegistry.hpp>
#include <cstdint>

namespace lux::render
{
    /// Per-frame mesh instance data for the extension slot mechanism.
    struct MeshInstanceExtData
    {
        uint32_t slot_count{0};
        /// World-partition active-mask GPU device address for THIS frame (the ring
        /// slot rotates per frame and may be reallocated on growth, so it cannot be
        /// baked at graph-compile time — it is patched into the cull push-constant
        /// at record time alongside slot_count). 0 = large-world disabled / no mask
        /// → the cull shader treats every instance as active.
        uint64_t active_mask_addr{0};
    };

    /// Global slot ID for mesh instance frame extension data.
    LUX_FUNCTION_PUBLIC FrameExtensionSlotId meshInstanceExtSlot() noexcept;

} // namespace lux::render
