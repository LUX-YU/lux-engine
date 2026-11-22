#include <lux/engine/render/device/JobGpuResources.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <utility>

namespace lux::render::detail
{
    JobGpuResources::JobGpuResources(DeviceSession& device, std::uint32_t frame_count, std::uint64_t serial) noexcept
        : session(device), generation(serial), accepted(device.makeFrontiers()), frames(frame_count)
    {
    }

    JobGpuResources::~JobGpuResources() noexcept
    {
        auto release = [frames = std::move(frames), histories = std::move(histories),
                        core = &session.nriCore()]() mutable noexcept
        {
            for (auto& frame : frames) frame.release(*core);
            for (auto& history : histories) history.release(*core);
        };
        session.deferRelease(std::move(accepted), std::move(release));
    }

    void JobHistoryResources::release(const nri::CoreInterface& core) noexcept
    {
        for (auto& slot : slots)
        {
            for (auto* view : slot.views)
            {
                core.DestroyDescriptor(view);
            }
            for (auto* view : slot.initial_views)
            {
                core.DestroyDescriptor(view);
            }
            core.DestroyTexture(slot.image);
            core.DestroyBuffer(slot.buffer);
        }
    }

    void JobFrameResources::release(const nri::CoreInterface& nri_core) noexcept
    {
        for (const auto& batch : batches)
        {
            for (auto* command : batch.commands) if (command) nri_core.DestroyCommandBuffer(command);
            for (auto* allocator : batch.allocators) if (allocator) nri_core.DestroyCommandAllocator(allocator);
        }
        nri_core.DestroyDescriptorPool(descriptor_pool);
        for (const auto& binding : raster_bindings)
            for (auto* view : binding.views) nri_core.DestroyDescriptor(view);
        for (const auto& attachment : raster_attachment_views)
            nri_core.DestroyDescriptor(attachment.view);
        for (const auto& pass_views : private_views)
        {
            for (auto* view : pass_views)
            {
                nri_core.DestroyDescriptor(view);
            }
        }
        for (auto* view : views)
        {
            nri_core.DestroyDescriptor(view);
        }
        for (auto* image : images)
        {
            nri_core.DestroyTexture(image);
        }
        for (auto* buffer : buffers)
        {
            nri_core.DestroyBuffer(buffer);
        }
        for (const auto& buffer : prepared_buffers)
            if (buffer.buffer) nri_core.DestroyBuffer(buffer.buffer);
        for (const auto& allocation : memories)
        {
            nri_core.FreeMemory(allocation.memory);
        }
    }
} // namespace lux::render::detail
