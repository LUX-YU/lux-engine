#pragma once

#include <lux/engine/render/native/vulkan/transfer/GpuCompletion.hpp>

namespace lux::render::vulkan
{
    // Project-only bridge: Runtime owns the one deferred slot and Native only borrows
    // its admission functions. No Runtime/IPC implementation appears in Native headers.
    struct GpuCompletionFns final
    {
        RenderResult<void*> (*reserve)(void*, SubmissionQueue&, void*) noexcept;
        RenderResult<void> (*setReply)(void*, void*) noexcept;
        RenderResult<void> (*commitWork)(void*, GpuWork&) noexcept;
        RenderResult<void> (*commitOwned)(void*, const SubmissionTicket&, void*,
            void (*)(void*) noexcept, RenderResult<void> (*)(void*, DeviceSession&) noexcept) noexcept;
        void (*rollback)(void*) noexcept;
    };

    struct GpuCompletionAccess final
    {
        const GpuCompletionFns* functions;
        void* owner;
    };
}
