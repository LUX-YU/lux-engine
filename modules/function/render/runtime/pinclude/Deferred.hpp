#pragma once

#include "Request.hpp"
#include <lux/engine/render/native/vulkan/CommandCompletion.hpp>
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
#include <lux/engine/render/native/vulkan/transfer/GpuWork.hpp>

namespace lux::render::runtime_detail
{
    struct GpuCompletion final
    {
        vulkan::SubmissionQueue* queue = nullptr;
        std::uint64_t submitted_before_reserve = 0;
        vulkan::GpuWork work;
        platform::ipc::IpcMessage reply;
        std::size_t reply_charge = 0;
        RenderRequest* reserved_request = nullptr;
        vulkan::DeviceSession* native = nullptr;
    };

    // The vector belongs only to RenderServer::Impl. monostate = free, GPU without request = reserved.
    using VCompletionCondition = std::variant<std::monostate, vulkan::ExecutionContext*, GpuCompletion>;
    struct DeferredCompletion final
    {
        std::optional<RenderRequest> request;
        VCompletionCondition condition;
    };

    // Valid only while the handler borrows the existing Server and its bounded slots.
    struct GpuAdmission final
    {
        vulkan::DeviceSession& native;
        std::span<DeferredCompletion> slots;
        RenderRequest& request;
        bool reserved = false;
    };

    extern const vulkan::GpuCompletionFns gpu_completion_functions;
}

#ifdef LUX_RUNTIME_TEST_SEAM
namespace lux::render::runtime_detail::test
{
    bool observeGpuCompletion() noexcept;
    void gpuReplyPublicationStarted() noexcept;
}
#endif
