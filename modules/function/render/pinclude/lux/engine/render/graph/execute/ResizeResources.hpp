#pragma once

#include <lux/engine/render/graph/execute/FrameResources.hpp>

namespace lux::render::detail
{
    // One cold Resize transaction. References below are indices/borrowed handles; only physical owns objects.
    // Before adoption it owns candidates, afterwards the replaced objects. Both use the Session cleanup scope.
    struct LUX_RENDER_PUBLIC RenderGraphInstance::ResizeResources final
    {
        explicit ResizeResources(RenderGraphInstance&) noexcept;
        ResizeResources(ResizeResources&&) noexcept;
        ~ResizeResources() noexcept;
        ResizeResources(const ResizeResources&) = delete;
        ResizeResources& operator=(const ResizeResources&) = delete;
        ResizeResources& operator=(ResizeResources&&) = delete;

        struct Resource final
        {
            std::uint32_t index;
            Binding binding;
        };

        struct OwnershipSlots final
        {
            std::size_t live = SIZE_MAX;
            std::size_t candidate = SIZE_MAX;
        };

        struct View final
        {
            ImageViewBinding use;
            nri::Descriptor* descriptor;
            bool owned;
            OwnershipSlots ownership;
        };

        struct Frame final
        {
            std::vector<Resource> resources;
            std::vector<View> views;
            std::vector<std::uint32_t> extent_passes;
            std::vector<std::uint8_t> replaced_slots;
            std::vector<OwnershipSlots> image_adoptions;
            std::vector<OwnershipSlots> buffer_adoptions;
            std::vector<BoundMemoryPlacement> placements;
            std::vector<std::uint32_t> allocation_candidates;
            std::vector<MemoryGroupId> previous_allocations;
            std::vector<FrameResources::MemoryAllocation> adopted_memories;
            std::vector<std::optional<nri::GlobalBarrierDesc>> alias_barriers;
            FrameResources physical;
        };

        TargetOutput output;
        std::vector<Frame> frames;
        std::vector<std::pair<std::uint32_t, HistoryState>> histories;

    private:
        friend class RenderGraphInstance;
        [[nodiscard]] static nri::Descriptor*& destination(Pass&, const ImageViewBinding&) noexcept;

        const RenderGraphInstance* owner_;
        bool prepared_ = false;
        DeviceSession* session_;
    };
} // namespace lux::render::detail
