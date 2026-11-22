#pragma once

#include <NRI.h>
#include <lux/engine/render/graph/CommandPackets.hpp>
#include <atomic>
#include <lux/engine/render/device/JobGpuResources.hpp>
#include <lux/engine/render/graph/execute/PassRecordInput.hpp>
#include <optional>
#include <span>

namespace lux::render::compiled { class RenderGraph; }

namespace lux::render::detail
{
    struct SelectedBindingDelta;

    struct FrozenOperandPatches final
    {
        std::size_t transfer = 0, compute = 0;
    };

    struct FrozenPatchCounts final
    {
        std::size_t resources = 0, passes = 0;
        bool operator==(const FrozenPatchCounts&) const noexcept = default;
    };

    struct RasterDrawList;

    struct FrozenBufferFill final
    {
        nri::Buffer* buffer;
        std::uint64_t offset;
        nri::Descriptor* storage_view;
    };

    struct FrozenImageBuffer final
    {
        nri::Texture* image;
        nri::Buffer* buffer;
        nri::TextureRegionDesc region;
        nri::TextureDataLayoutDesc layout;
    };

    struct TransferOperands final
    {
        std::vector<BufferCopyRegion> buffer_copies;
        std::vector<TextureCopyRegion> image_copies;
        std::vector<FrozenImageBuffer> image_buffers;
        std::vector<FrozenBufferFill> fills;
        std::vector<nri::AttachmentDesc> clears;

        void initialize(const TransferProgram&) noexcept;
    };

    struct FrameRecordInput;

    void recordTransfers(
        const TransferCommands&, const FrameRecordInput&, nri::CommandBuffer&, const nri::CoreInterface&
    ) noexcept;

    struct RecordPass final
    {
        std::span<const nri::TextureBarrierDesc> images;
        std::span<const nri::BufferBarrierDesc> buffers;
        std::optional<nri::GlobalBarrierDesc> alias_barrier;
        nri::DescriptorSet* private_descriptors = nullptr;
        std::span<const std::byte> parameters;
        PassNativeInput native;
    };

    struct RecordHistoryInitialization final
    {
        nri::TextureBarrierDesc image_before{}, image_after{};
        nri::BufferBarrierDesc buffer_before{}, buffer_after{};
        std::span<nri::Descriptor* const> views;
        nri::ClearValue clear_value{};
        std::uint64_t buffer_size = 0;
        bool depth = false;
    };

    // A synchronous recording borrow of prepared, stable FIF slices. Only the caller owns the
    // mutation window; no owner, scheduler, descriptor directory or History selection is exposed.
    // Parameters and draw bytes already reside in FIF storage, not the client's mutable vectors.
    struct FrameRecordInput final
    {
        const compiled::RenderGraph& plan;
        const TransferOperands& transfers;
        std::span<const PassResourceInput> resources;
        std::span<const RecordPass> passes;
        std::span<const RecordHistoryInitialization> history_initializations;
        std::span<const std::uint8_t> enabled_regions;
        std::span<const SelectedBindingDelta> binding_choices;
        std::span<const nri::TextureBarrierDesc> final_images;
        std::span<const nri::BufferBarrierDesc> final_buffers;
        nri::DescriptorPool* descriptor_pool = nullptr;
        std::array<nri::DescriptorSet*, 3> shared_descriptors{};
    };

    // CPU cache state for one native recording batch in a FIF slot. Its allocator and CB are device-owned.
    struct CommandBatch final
    {
        std::uint64_t revision = 1, recorded_revision = 0;
        std::atomic<bool> recording{false};

        void invalidate() noexcept
        {
            if (revision != recorded_revision) return;
            if (revision == UINT64_MAX) std::terminate();
            ++revision;
        }

        CommandBatch() noexcept = default;
        CommandBatch(const CommandBatch&) = delete;
        CommandBatch& operator=(const CommandBatch&) = delete;
        CommandBatch& operator=(CommandBatch&&) = delete;

        CommandBatch(CommandBatch&& other) noexcept
        {
            // Only cold construction may relocate a batch. Published batches keep stable addresses.
            if (other.recording.load(std::memory_order_acquire)) std::terminate();
            revision = other.revision;
            recorded_revision = other.recorded_revision;
        }
    };

    // The owner freezes a frame once. Workers borrow its command inputs and only their batch's mutable state.
    struct FrozenRecordingFrame final
    {
        FrameRecordInput input;
        std::span<CommandBatch> revisions;
        std::span<const JobFrameResources::NativeBatch> native_batches;
    };
} // namespace lux::render::detail
