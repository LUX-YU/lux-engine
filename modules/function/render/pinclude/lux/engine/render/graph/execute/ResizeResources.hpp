#pragma once

#include <lux/engine/render/graph/execute/RenderGraphInstance.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>

namespace lux::render::detail
{
    // CPU changes reference a local native candidate. Adoption exchanges only affected physical objects.
    // Only the native payload enters Session retirement after the accepted uses of the old backing.
    struct LUX_RENDER_PUBLIC RenderGraphInstance::ResizeResources final
    {
        [[nodiscard]] static RenderResult<ResizeResources> prepare(
            JobGpuResources&,
            RenderGraphInstance&,
            TargetOutput,
            std::span<const ImageReplacement> = std::span<const ImageReplacement>{}
        ) noexcept;
        ResizeResources(ResizeResources&&) noexcept;
        ~ResizeResources() noexcept;
        ResizeResources(const ResizeResources&) = delete;
        ResizeResources& operator=(const ResizeResources&) = delete;
        ResizeResources& operator=(ResizeResources&&) = delete;

        struct Resource final
        {
            std::uint32_t index;
            FrameResourceBinding binding;
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
            std::vector<JobFrameResources::MemoryAllocation> adopted_memories;
            std::vector<std::optional<nri::GlobalBarrierDesc>> alias_barriers;
            explicit Frame(JobFrameResources& storage) noexcept : physical(storage) {}
            JobFrameResources& physical;
        };

        TargetOutput output;
        std::vector<Frame> frames;
        std::vector<std::pair<std::uint32_t, HistoryState>> histories;

    private:
        friend class RenderGraphInstance;
        [[nodiscard]] static nri::Descriptor*& destination(FramePass&, const ImageViewBinding&) noexcept;

        std::vector<JobFrameResources> native_frames_;
        std::vector<JobHistoryResources> native_histories_;
        SubmissionFrontiers accepted_;
        explicit ResizeResources(JobGpuResources&) noexcept;
        const JobGpuResources* owner_;
        bool prepared_ = false;
        DeviceSession* session_;
    };
} // namespace lux::render::detail
