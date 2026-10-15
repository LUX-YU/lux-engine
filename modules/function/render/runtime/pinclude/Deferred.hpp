#pragma once

#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
#include <lux/engine/render/native/vulkan/transfer/GpuWork.hpp>

namespace lux::render::runtime_detail
{
    struct GpuCompletion final
    {
        vulkan::SubmissionQueue* queue = nullptr;
        std::uint64_t submitted_before_reserve = 0;
        vulkan::GpuWork work;
        std::array<std::byte, 128> reply;
        std::size_t size = 0;
        OwnedWireBytes attachment;
    };

    // The vector belongs only to RenderServer::Impl. monostate = free, GPU without request = reserved.
    using VCompletionCondition = std::variant<std::monostate, vulkan::ExecutionContext*, GpuCompletion>;
    struct DeferredCompletion final
    {
        std::optional<RenderRequest> request;
        VCompletionCondition condition;
    };
}

#ifdef LUX_RUNTIME_TEST_SEAM
namespace lux::render::runtime_detail::test
{
    bool observeGpuCompletion() noexcept;
    void gpuReplyPublicationStarted() noexcept;
}
#endif
