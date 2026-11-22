#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/graph/RenderGraph.hpp>
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

    void JobFrameResources::NativeBatch::release(const nri::CoreInterface& core) const noexcept
    {
        for (auto* command : commands) if (command) core.DestroyCommandBuffer(command);
        for (auto* allocator : allocators) if (allocator) core.DestroyCommandAllocator(allocator);
    }

    void JobFrameResources::release(const nri::CoreInterface& nri_core) noexcept
    {
        for (const auto& batch : batches) batch.release(nri_core);
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

    RenderResult<void>
    JobFrameResources::createBatches(DeviceSession& session, std::span<const SubmissionBatch> schedule) noexcept
    {
        if (!batches.empty()) return cxx::unexpected(RenderError{kDeviceArgument});
        const auto& core = session.nriCore();
        std::vector<NativeBatch> candidate(schedule.size());
        cxx::scope_exit rollback([&]() noexcept
        {
            for (const auto& batch : candidate) batch.release(core);
        });
        for (std::size_t batch = 0; batch < schedule.size(); ++batch)
        {
            const auto& description = schedule[batch];
            const bool has_tail = description.output_begin != kGraphInvalidIndex;
            const std::uint32_t first_part = has_tail && description.output_begin == description.first_pass ? 1u : 0u;
            const std::uint32_t end_part = has_tail ? 2u : 1u;
            for (auto part = first_part; part < end_part; ++part)
            {
                auto result = core.CreateCommandAllocator(
                    session.queue(description.queue).native(), candidate[batch].allocators[part]
                );
                if (result == nri::Result::SUCCESS)
                {
                    result = core.CreateCommandBuffer(
                        *candidate[batch].allocators[part], candidate[batch].commands[part]
                    );
                }
                if (result != nri::Result::SUCCESS)
                {
                    return cxx::unexpected(nativeError(result));
                }
            }
        }
        batches = std::move(candidate);
        rollback.release();
        return {};
    }
} // namespace lux::render::detail
