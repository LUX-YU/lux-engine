#pragma once
#include <lux/engine/render/core/GPUResourceTypes.hpp>
#include <lux/engine/render/core/RenderTypes.hpp>   // kMaxFramesInFlight
#include <lux/engine/render/FrameStamp.hpp>
#include <string>
#include <cstdint>
#include <vulkan/vulkan.h>

namespace lux::render
{
    /// Per-FIF descriptor revision tracker.  Bump the revision when the
    /// underlying VkBuffer/VkImage handle changes; at upload time, compare
    /// per-slot written revisions to decide whether to re-write.
    struct DescriptorRevision
    {
        uint64_t revision{0};
        uint64_t written[kMaxFramesInFlight]{};

        void bump() noexcept { ++revision; }

        /// Returns true if slot `fi` has not yet been written at the
        /// current revision.  Automatically marks it written.
        bool needsWrite(uint32_t fi) noexcept
        {
            if (written[fi] != revision) {
                written[fi] = revision;
                return true;
            }
            return false;
        }
    };

    /**
     * @brief CRTP base for GPU resources — zero virtual overhead.
     *
     * @tparam Derived      The concrete resource class (CRTP self-type).
     * @tparam ResourceType Compile-time resource type tag.
     *
     * Provides:
     *   - `isInitialized()` flag shared by all resources.
     *   - Default no-op implementations for optional hooks (`update`,
     *     `uploadData`, `getDescriptorSet`, etc.).  Derived classes shadow
     *     any method they actually need.
     *
     * ResourceRegistry stores resources as type-erased Slot entries
     * and dispatches shutdown/upload via stored function pointers — no
     * virtual call needed.
     */
    template<typename Derived, EGPUResourceType ResourceType>
    class GPUResourceBase
    {
    public:
        static constexpr EGPUResourceType resource_type = ResourceType;

        [[nodiscard]] bool isInitialized() const noexcept { return initialized_; }

        // ── Optional hooks — shadowed by Derived when needed ────────────────

        void update() {}
        void uploadData(VkCommandBuffer /*cb*/, const FrameStamp& /*stamp*/) {}
        std::string getDebugInfo() const { return "No debug info available"; }

        VkDescriptorSet       getDescriptorSet()       const { return VK_NULL_HANDLE; }
        VkDescriptorSetLayout getDescriptorSetLayout()  const { return VK_NULL_HANDLE; }
        void recordBind(VkCommandBuffer, VkPipelineBindPoint,
                        VkPipelineLayout, uint32_t = UINT32_MAX) const {}

    protected:
        bool initialized_{false};
        uint32_t frames_in_flight_{2};

        /// Per-FIF descriptor revision tracker.  Subclasses call
        /// `ds_revision_.bump()` when the underlying buffer/image handle
        /// changes, and `ds_revision_.needsWrite(slot)` at upload time
        /// to decide whether to re-write the descriptor set.
        DescriptorRevision ds_revision_;
    };

} // namespace lux::render
