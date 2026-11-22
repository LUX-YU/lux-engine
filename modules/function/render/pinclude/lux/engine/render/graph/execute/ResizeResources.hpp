#pragma once

#include <lux/engine/render/target/TargetOutput.hpp>
#include <lux/engine/render/graph/execute/PhysicalFrameAllocation.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>

namespace lux::render::runtime_detail { struct JobExecution; }

namespace lux::render::detail
{
    struct ImageReplacement final
    {
        GraphResourceId resource;
        std::span<const TargetFrame> frames;
    };

    // CPU changes reference a local native candidate. Adoption exchanges only affected physical objects.
    // Only the native payload enters Session retirement after the accepted uses of the old backing.
    struct LUX_RENDER_PUBLIC ResizeResources final
    {
        [[nodiscard]] static RenderResult<ResizeResources> prepare(
            runtime_detail::JobExecution&,
            TargetOutput,
            std::span<const ImageReplacement> = std::span<const ImageReplacement>{}
        ) noexcept;
        [[nodiscard]] RenderResult<void> prepareAdoption(runtime_detail::JobExecution&) noexcept;
        void commit(runtime_detail::JobExecution&) && noexcept;

        ResizeResources(ResizeResources&&) noexcept;
        ~ResizeResources() noexcept;
        ResizeResources(const ResizeResources&) = delete;
        ResizeResources& operator=(const ResizeResources&) = delete;
        ResizeResources& operator=(ResizeResources&&) = delete;

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
            PhysicalFrameChanges changes;
            std::vector<View> views;
            std::vector<std::uint32_t> extent_passes;
            std::vector<OwnershipSlots> image_adoptions;
            std::vector<OwnershipSlots> buffer_adoptions;
            std::vector<JobFrameResources::MemoryAllocation> adopted_memories;
            std::vector<std::optional<nri::GlobalBarrierDesc>> alias_barriers;
            explicit Frame(JobFrameResources& storage) noexcept : physical(storage) {}
            JobFrameResources& physical;
        };

        TargetOutput output;
        std::vector<Frame> frames;
        std::vector<std::pair<std::uint32_t, HistoryState>> histories;

    private:
        RenderResult<void> prepareHistories(const runtime_detail::JobExecution&) noexcept;
        RenderResult<void> prepareBindings(const runtime_detail::JobExecution&, std::uint32_t slot) noexcept;
        [[nodiscard]] static nri::Descriptor*& destination(FramePass&, const ImageViewBinding&) noexcept;

        std::vector<JobFrameResources> native_frames_;
        std::vector<JobHistoryResources> native_histories_;
        SubmissionFrontiers accepted_;
        ResizeResources(JobGpuResources&, ExecutionBindingId) noexcept;
        ExecutionBindingId owner_;
        enum class EPhase { CANDIDATE, READY, ADOPTED };
        EPhase phase_ = EPhase::CANDIDATE;
        DeviceSession* session_;
    };
} // namespace lux::render::detail
