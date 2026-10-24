#pragma once

#include <lux/engine/render/execution/FrameResources.hpp>

namespace lux::render::detail
{
    // One cold Resize transaction. References below are indices/borrowed handles; only physical owns objects.
    // Before adoption it owns candidates, afterwards the replaced objects. Both use the Session cleanup scope.
    struct LUX_RENDER_PUBLIC RenderGraphInstance::ResizeResources final
    {
        explicit ResizeResources(DeviceSession&) noexcept;
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

        struct View final
        {
            ImageViewUse use;
            nri::Descriptor* descriptor;
            bool owned;
        };

        struct Extent final
        {
            std::uint32_t pass;
            std::array<std::uint32_t, 3> dispatch_groups{};
            std::vector<nri::TextureDataLayoutDesc> copy_layouts;
        };

        struct Frame final
        {
            std::vector<Resource> resources;
            std::vector<View> views;
            std::vector<Extent> extents;
            std::vector<std::uint8_t> replaced_slots;
            std::vector<MemoryPlacement> placements;
            std::vector<std::uint32_t> allocation_candidates;
            std::vector<std::uint32_t> previous_allocations;
            std::vector<FrameResources::MemoryAllocation> adopted_memories;
            std::vector<std::optional<nri::GlobalBarrierDesc>> alias_barriers;
            FrameResources physical;
        };

        TargetOutput output;
        std::vector<Frame> frames;
        std::vector<std::pair<std::uint32_t, HistoryState>> histories;

    private:
        DeviceSession* session_;
    };
} // namespace lux::render::detail
