#pragma once

#include "Support.hpp"
#include <lux/engine/render/device/SubmissionQueue.hpp>

namespace foundation_test
{
    template <class Record>
    RenderResult<void> submitFixtureCommands(DeviceSession& session, SubmissionQueue& queue, Record&& record) noexcept
    {
        const auto& core = session.nriCore();
        nri::CommandAllocator* allocator = nullptr;
        nri::CommandBuffer* command = nullptr;
        lux::cxx::scope_exit rollback([&]() noexcept
        { core.DestroyCommandBuffer(command); core.DestroyCommandAllocator(allocator); });
        auto status = core.CreateCommandAllocator(queue.native(), allocator);
        if (status == nri::Result::SUCCESS) status = core.CreateCommandBuffer(*allocator, command);
        if (status == nri::Result::SUCCESS) status = core.BeginCommandBuffer(*command, nullptr);
        if (status != nri::Result::SUCCESS)
            return lux::cxx::unexpected(RenderError{kDeviceFailure, {static_cast<std::uint32_t>(status)}});
        auto recorded = std::invoke(record, *command, 0u);
        status = core.EndCommandBuffer(*command);
        if (!recorded) return recorded;
        if (status != nri::Result::SUCCESS)
            return lux::cxx::unexpected(RenderError{kDeviceFailure, {static_cast<std::uint32_t>(status)}});
        auto submitted = runTask(queue.submit({&command, 1}, {}, {}));
        if (!submitted) return lux::cxx::unexpected(submitted.error());
        auto frontier = session.makeFrontiers();
        frontier.accept(session.submissionPoint(session.queueIndex(queue), *submitted));
        session.deferRelease(std::move(frontier), [command, allocator, destroy_command = core.DestroyCommandBuffer,
            destroy_allocator = core.DestroyCommandAllocator]() noexcept
        { destroy_command(command); destroy_allocator(allocator); });
        rollback.release();
        return {};
    }
}
