#pragma once

#include <lux/engine/render/device/SubmissionFrontiers.hpp>
#include <lux/engine/render/resource/StorageBinding.hpp>
#include <lux/engine/render/device/DescriptorTable.hpp>
#include <lux/engine/render/graph/GraphResource.hpp>
#include <lux/engine/render/visibility.h>
#include <NRI.h>
#include <array>
#include <memory>
#include <cstdint>
#include <vector>

namespace lux::render { struct TargetDesc; }
namespace lux::render::compiled { class RenderGraph; }

namespace lux::render { class ResourceSet; }

namespace lux::render::detail
{
    class DeviceSession;
    struct FrameResources;
    struct HistoryState;
    struct InstalledGraphBindings;
    struct TargetFrame;
    struct SubmissionBatch;
    struct HistoryPlan;
    struct CompiledResource;
    struct CompiledRasterStream;
    struct CompiledRasterProgram;
    struct RasterDescriptorLayout;
    struct RasterProgramSelection;
    // The device round keeps selected bytes alive through staging retries and partial submissions.
    struct PreparedBufferInput final
    {
        std::vector<std::byte> bytes;
        std::uint64_t revision = 0, size = 0;
    };

    struct PhysicalFrameContract;
    struct PhysicalFrameChanges;

    struct RasterDescriptorInput final
    {
        nri::Buffer* buffer = nullptr;
        std::uint64_t offset = 0, size = 0;
        nri::Texture* image = nullptr;
        nri::Descriptor* sampler = nullptr;
        nri::Format format = nri::Format::UNKNOWN;
        GraphImageRange range;
        DescriptorTable::Slot target_view{};
        bool operator==(const RasterDescriptorInput&) const noexcept = default;
    };

    struct PreparedBufferReplica final
    {
        nri::Buffer* buffer = nullptr;
        std::uint64_t capacity = 0, generation = 0;
        struct Contents final
        {
            std::uint64_t revision = 0, size = 0, prefix = 0;
        };

        Contents accepted;

        [[nodiscard]] bool contains(std::uint64_t revision, std::uint64_t size) const noexcept
        {
            return buffer && revision && accepted.revision == revision && accepted.size == size && accepted.prefix == size;
        }
    };

    // The device round owns the bytes and holds this FIF allocation until every staged range has settled.
    struct PreparedBufferUpload final
    {
        PreparedBufferReplica& destination;
        SubmissionFrontiers& allocation_uses;
        std::span<const std::byte> bytes;
        std::uint64_t revision, generation;
        std::size_t begin = 0, end = 0;
        ReplicaCopyRange ranges{};

        void settle(SubmissionPoint, RenderError) noexcept;
    };

    struct JobFrameResources final
    {
        JobFrameResources() noexcept = default;
        JobFrameResources(JobFrameResources&&) = delete;
        JobFrameResources(const JobFrameResources&) = delete;
        JobFrameResources& operator=(const JobFrameResources&) = delete;

        struct RasterBinding final
        {
            nri::DescriptorSet* set = nullptr;
            // A program replacement releases all of its descriptor rows before retiring the old layout.
            const RasterDescriptorLayout* layout = nullptr;
            std::uint32_t pass = 0, program = 0, list = 0, ordinal = 0;
            std::vector<nri::Descriptor*> views;
            std::vector<RasterDescriptorInput> inputs;
        };

        struct RasterAttachmentView final
        {
            nri::Texture* image;
            GraphImageRange range;
            bool depth;
            nri::Descriptor* view;
        };

        struct MemoryAllocation final
        {
            nri::Memory* memory;
            std::uint64_t bytes;
        };

        struct NativeBatch final
        {
            std::array<nri::CommandAllocator*, 2> allocators{};
            std::array<nri::CommandBuffer*, 2> commands{};

            void release(const nri::CoreInterface&) const noexcept;
        };

        // One owned image and its single-layer attachment views per compiled runtime-layer slot.
        struct LayeredImage final
        {
            GraphResourceId resource;
            nri::Texture* texture;
            std::vector<nri::Descriptor*> views;
            std::uint32_t requested_layers = 0;
        };

        std::vector<NativeBatch> batches;
        std::vector<LayeredImage> layered_images;
        std::vector<RasterAttachmentView> raster_attachment_views;
        std::vector<RasterBinding> raster_bindings;
        std::vector<std::vector<nri::Descriptor*>> private_views;
        std::vector<nri::Buffer*> buffers;
        std::vector<nri::Texture*> images;
        std::vector<nri::Descriptor*> views;
        std::vector<PreparedBufferReplica> prepared_buffers;
        std::vector<MemoryAllocation> memories;
        nri::DescriptorPool* descriptor_pool = nullptr;

        [[nodiscard]] RenderResult<PhysicalFrameChanges>
        preparePhysical(DeviceSession&, const PhysicalFrameContract&) noexcept;
        [[nodiscard]] RenderResult<void> createBatches(DeviceSession&, std::span<const SubmissionBatch>) noexcept;
        void release(const nri::CoreInterface&) noexcept;
    };

    struct JobHistoryResources final
    {
        JobHistoryResources() noexcept = default;
        JobHistoryResources(JobHistoryResources&&) noexcept = default;
        JobHistoryResources& operator=(JobHistoryResources&&) noexcept = default;
        JobHistoryResources(const JobHistoryResources&) = delete;
        JobHistoryResources& operator=(const JobHistoryResources&) = delete;

        struct Slot final
        {
            nri::Texture* image = nullptr;
            nri::Buffer* buffer = nullptr;
            std::uint64_t buffer_size = 0;
            const nri::TextureDesc* image_description = nullptr;
            const nri::BufferDesc* buffer_description = nullptr;
            std::vector<nri::Descriptor*> views;
            std::vector<nri::Descriptor*> initial_views;
            SubmissionFrontiers last_use;
            bool initialized = false;
        };

        std::vector<Slot> slots;
        [[nodiscard]] RenderResult<void>
        initialize(DeviceSession&, const HistoryPlan&, const CompiledResource&, const TargetDesc&) noexcept;
        void release(const nri::CoreInterface&) noexcept;
    };

    class LUX_RENDER_PUBLIC JobGpuResources final
    {
    public:
        JobGpuResources(DeviceSession&, std::uint32_t frame_count, std::uint64_t generation = 0) noexcept;
        ~JobGpuResources() noexcept;
        JobGpuResources(const JobGpuResources&) = delete;
        JobGpuResources& operator=(const JobGpuResources&) = delete;

        [[nodiscard]] RenderResult<void>
        installFrame(InstalledGraphBindings&, std::uint32_t slot, std::span<const HistoryState>, const TargetFrame*) noexcept;
        void releaseOutputViews(InstalledGraphBindings&) noexcept;

        [[nodiscard]] RenderResult<RasterProgramSelection> programSlots(const CompiledRasterStream&) const noexcept;
        [[nodiscard]] RenderResult<void> bindProgramSlots(
            const compiled::RenderGraph&, const CompiledRasterStream&, std::span<FrameResources>,
            std::span<const std::shared_ptr<const CompiledRasterProgram>>
        ) noexcept;

        std::unique_ptr<ResourceSet> private_resources;
        DeviceSession& session;
        const std::uint64_t generation;
        SubmissionFrontiers accepted;
        std::vector<JobFrameResources> frames;
        std::vector<JobHistoryResources> histories;

    private:
        struct BoundProgramSlots final
        {
            std::vector<std::shared_ptr<const CompiledRasterProgram>> slots;
            std::uint64_t revision = 0;
        };
        std::vector<BoundProgramSlots> program_slots_;
        void releaseProgramUses(const BoundProgramSlots&) noexcept;
    };
}
