#pragma once

#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
#include <lux/engine/render/native/vulkan/transfer/TextureUpload.hpp>

namespace lux::render::runtime_detail
{
    struct GpuCompletion final
    {
        vulkan::SubmissionQueue* queue = nullptr;
        std::uint64_t submitted_before_reserve = 0;
        std::optional<vulkan::SubmissionTicket> ticket;
        std::unique_ptr<void, void (*)(void*) noexcept> owner{nullptr, [](void*) noexcept {}};
        RenderResult<void> (*ready)(void*, vulkan::DeviceSession&) noexcept = nullptr;
        std::unique_ptr<vulkan::TextureUpload> upload;
        RenderResult<void> (*texture_ready)(void*, vulkan::TextureUpload&, vulkan::DeviceSession&) noexcept = nullptr;
        std::optional<RenderError> failure;
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
