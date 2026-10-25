#pragma once

#include <lux/engine/render/commands/Frame.hpp>
#include <lux/engine/render/extensions/CommandBinding.hpp>
#include <vector>

namespace lux::render::runtime_detail
{
    // Fixed-address protocol records owned by Impl. The callback borrows the scope-owned Sender
    // operation; this record never owns or destroys a connected operation state.
    struct FrameRecord final
    {
        enum class EStage : std::uint8_t
        {
            EMPTY,
            QUEUED,
            APPLYING,
            SUBMITTING
        };

        struct Invocation final
        {
            FrameCommand* input;
            HandlerCall handler;
            void (*make_error)(platform::ipc::IpcMessage&, RenderError) noexcept;
        };

        EStage stage = EStage::EMPTY;
        FrameSubmission input;
        std::vector<Invocation> invocations;
        std::size_t group = 0;
        std::size_t dispatched = 0;
        std::size_t group_pending = 0;
        std::uint32_t slot = UINT32_MAX;
        RenderError error{};
        void* receiver = nullptr;
        void (*complete)(void*, FrameCompletion) noexcept = nullptr;
    };
} // namespace lux::render::runtime_detail
