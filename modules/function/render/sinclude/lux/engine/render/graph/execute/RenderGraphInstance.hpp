#pragma once

#include <lux/engine/render/visibility.h>

#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/target/TargetOutput.hpp>
#include <lux/engine/render/extensions/execution/ResourceInput.hpp>
#include <lux/engine/render/graph/RenderGraph.hpp>

namespace lux::render::runtime_detail { struct JobExecution; }

namespace lux::render::detail
{
    struct FrozenPatchCounts;
    struct FrozenOperandPatches;
    struct FrameResources;
    struct HistoryState;
    class DeviceSession;
    struct RasterDrawList;
    class JobGpuResources;
    struct FrozenRecordingFrame;

    class LUX_RENDER_PUBLIC RenderGraphInstance final
    {
    public:
        [[nodiscard]] std::uint32_t frameCount() const noexcept;
        [[nodiscard]] RenderResult<void>
        writeInlineConstant(
            std::uint32_t slot, std::uint32_t pass, std::span<const std::byte> bytes, std::uint64_t revision = 0
        ) noexcept;
        // Caller holds the writable FIF until recording and all accepted GPU uses are complete.
        // Each batch part has an exclusive allocator; encoders only borrow its prepared data.
        [[nodiscard]] RenderResult<std::span<nri::CommandBuffer* const>>
        recordBatch(std::uint32_t frame_slot, std::uint32_t batch) noexcept;

        struct ResizeResources;

        struct ImageReplacement final
        {
            GraphResourceId resource;
            std::span<const TargetFrame> frames;
        };

        // Cold candidate only; instance identity, subscriptions and Feature bindings remain unchanged.
        // Empty output is valid only for graphs with no output or Target-relative resources.
        // Surface withdrawal uses releaseOutputReferences(), not an empty Resize candidate.
        // Revalidate dependent extents after suspension; concurrent Storage publication may change buffer ranges.
        [[nodiscard]] RenderResult<void> prepareResizeAdoption(ResizeResources&) noexcept;
        // Called only after affected accepted GPU uses complete, with no intervening suspension.
        void commitResize(ResizeResources&&) noexcept;
        // After accepted uses complete, withdraw borrowed SwapChain objects before their presentation owner closes.
        void releaseOutputReferences() noexcept;
        // The selected writable FIF slot owns the list. Features fill physical packets before
        // prepareFrame seals attachments and barriers; no list is mutated during recording.
        [[nodiscard]] RasterDrawList&
        rasterDraws(const CompiledPass&, std::uint32_t slot, bool recording_changed = true) noexcept;
        // Borrowed physical inputs are installed before initialize; the caller retains them through GPU completion.
        void bindImage(GraphResourceId resource, std::uint32_t slot, nri::Texture& image) noexcept;

        [[nodiscard]] RenderResult<void>
        qualifyImportedImage(GraphResourceId, const nri::Texture&, const TargetDesc&) const noexcept;

        [[nodiscard]] bool frozenForSubmission(std::uint32_t slot) const noexcept;

        ~RenderGraphInstance() noexcept;
        RenderGraphInstance(const RenderGraphInstance&) = delete;
        RenderGraphInstance& operator=(const RenderGraphInstance&) = delete;

        [[nodiscard]] const compiled::RenderGraph& plan() const & noexcept
        {
            return graph_;
        }
        const compiled::RenderGraph& plan() const && = delete;

        [[nodiscard]] const TargetDesc& target() const noexcept
        {
            return output_.description;
        }

        // An empty selection invalidates every declared history at the next preparation boundary.
        [[nodiscard]] RenderResult<void> invalidateHistory(std::span<const GraphResourceId> previous = {}) noexcept;

        // Trusted compiled resource index and Session slot, not a business resource lookup.
        void setResource(std::uint32_t resource, std::uint32_t slot, const ResourceInput&) noexcept;
        [[nodiscard]] ResourceInput resourceInput(GraphResourceId, std::uint32_t slot) const noexcept;
        // Cold diagnostics: sum actual NRI allocation requirements for this instance's owned resources.
        [[nodiscard]] bool dumpResources(DumpSink) const noexcept;
        [[nodiscard]] nri::Buffer* nativeBuffer(std::uint32_t resource, std::uint32_t slot) const noexcept;

        // Explicit dirty notification for producers that opted out of rasterDraws' conservative default.
        void invalidateRasterDraws(const CompiledPass&, std::uint32_t frame_slot) noexcept;

        [[nodiscard]] FrozenOperandPatches lastOperandPatches(std::uint32_t slot) const noexcept;
        [[nodiscard]] FrozenPatchCounts lastFrozenPatches(std::uint32_t slot) const noexcept;

    private:
        friend struct runtime_detail::JobExecution;

        struct FrameStorage final
        {
            std::span<FrameResources> frames;
            std::span<HistoryState> histories;
        };

        RenderGraphInstance(JobGpuResources&, const compiled::RenderGraph&, FrameStorage, TargetOutput&) noexcept;

        [[nodiscard]] const FrozenRecordingFrame* recordingFrame(std::uint32_t frame_slot) const & noexcept;
        const FrozenRecordingFrame* recordingFrame(std::uint32_t) const && = delete;
        [[nodiscard]] RenderResult<void> createFrameResources(std::uint32_t slot) noexcept;
        [[nodiscard]] RenderResult<void> prepareFrameBindings(std::uint32_t slot) noexcept;
        [[nodiscard]] RenderResult<void> prepareDescriptors() noexcept;
        [[nodiscard]] RenderResult<void> rebind(std::uint32_t slot) noexcept;

        [[nodiscard]] RenderResult<void> prepareResizeHistories(ResizeResources&, const TargetDesc&) noexcept;
        [[nodiscard]] RenderResult<void> prepareResizeBindings(ResizeResources&, std::uint32_t) noexcept;

        void prepareAliasBarriers(FrameResources&) noexcept;
        [[nodiscard]] RenderResult<void> createHistories() noexcept;
        [[nodiscard]] RenderResult<void> createHistoryViews(HistoryState&) noexcept;
        [[nodiscard]] bool selectHistory() noexcept;

        DeviceSession* session_;
        JobGpuResources& gpu_;

        const std::span<FrameResources> frames_;
        const std::span<HistoryState> histories_;
        const compiled::RenderGraph& graph_;
        TargetOutput& output_;

    };
} // namespace lux::render::detail
