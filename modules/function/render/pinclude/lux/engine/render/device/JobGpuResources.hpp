#pragma once

#include <lux/engine/render/extensions/device/SubmissionFrontiers.hpp>
#include <lux/engine/render/extensions/graph/GraphResource.hpp>
#include <lux/engine/render/visibility.h>
#include <NRI.h>
#include <array>
#include <memory>
#include <cstdint>
#include <vector>

namespace lux::render::detail
{
    class DeviceSession;
    struct SubmissionBatch;
    struct CompiledRasterStream;
    struct DynamicRasterPrograms;
    struct RasterProgramSelection;
    struct PhysicalFrameContract;
    struct PhysicalFrameChanges;

    struct RasterBindingKey final
    {
        std::uint32_t pass, program;
        std::uint64_t generation;

        bool operator==(const RasterBindingKey&) const noexcept = default;
    };

    struct RasterDescriptorInput final
    {
        nri::Buffer* buffer = nullptr;
        std::uint64_t offset = 0, size = 0;
        nri::Texture* image = nullptr;
        nri::Descriptor* sampler = nullptr;
        nri::Format format = nri::Format::UNKNOWN;
        GraphImageRange range;
        bool operator==(const RasterDescriptorInput&) const noexcept = default;
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
            RasterBindingKey program;
            std::uint32_t list = 0, ordinal = 0;
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

        struct PreparedBufferReplica final
        {
            nri::Buffer* buffer = nullptr;
            std::uint64_t capacity = 0, generation = 0;
            std::uint64_t accepted_revision = 0, accepted_size = 0;
            std::size_t staged_bytes = 0;

            [[nodiscard]] bool contains(std::uint64_t revision, std::uint64_t size) const noexcept
            {
                return buffer && revision != 0 && accepted_revision == revision && accepted_size == size;
            }
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

        std::vector<NativeBatch> batches;
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
        void collectRasterViews(nri::Buffer*, nri::Texture*, std::vector<nri::Descriptor*>&) noexcept;
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
        void release(const nri::CoreInterface&) noexcept;
    };

    class LUX_RENDER_PUBLIC JobGpuResources final
    {
    public:
        JobGpuResources(DeviceSession&, std::uint32_t frame_count, std::uint64_t generation = 0) noexcept;
        ~JobGpuResources() noexcept;
        JobGpuResources(const JobGpuResources&) = delete;
        JobGpuResources& operator=(const JobGpuResources&) = delete;

        [[nodiscard]] const DynamicRasterPrograms* dynamicRasterPrograms(std::uint32_t list) const noexcept;
        [[nodiscard]] RenderResult<RasterProgramSelection>
        selectRasterPrograms(std::uint32_t pass, const CompiledRasterStream&) const noexcept;
        void adoptRasterPrograms(std::uint32_t list, std::unique_ptr<DynamicRasterPrograms>) noexcept;

        DeviceSession& session;
        const std::uint64_t generation;
        SubmissionFrontiers accepted;
        std::vector<JobFrameResources> frames;
        std::vector<JobHistoryResources> histories;

    private:
        std::vector<std::unique_ptr<DynamicRasterPrograms>> raster_programs_;
        void releaseRasterPrograms(const DynamicRasterPrograms&) noexcept;
    };
}
