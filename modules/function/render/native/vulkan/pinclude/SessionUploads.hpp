#pragma once

#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/transfer/Transfer.hpp>

namespace lux::render::vulkan
{
    struct DeviceSession::UploadState final
    {
        DeviceSession& session;
        SubmissionQueue* copy_queue = nullptr;
        TransferOptions options;
        execution::CpuScheduler cpu;
        TransferStats stats;
        VkQueryPool timestamps = VK_NULL_HANDLE;
        std::uint32_t copy_timestamp_bits = 0, graphics_timestamp_bits = 0;
        UploadState(DeviceSession& session, TransferOptions options, execution::CpuScheduler cpu) noexcept
            : session(session), options(options), cpu(cpu)
        {
        }
        ~UploadState() noexcept;
    };

} // namespace lux::render::vulkan
