#include <lux/engine/render/native/vulkan/Readback.hpp>

namespace lux::render::vulkan
{
    ReadbackOperation::ReadbackOperation(ExecutionContext& context, void* owner, Complete complete,
        void* observer, void (*submitted)(void*) noexcept) noexcept
        : context_(&context), queue_(context.queue_), owner_(owner), complete_(complete),
          observer_(observer), submitted_(submitted) {}

    ReadbackOperation::~ReadbackOperation() noexcept
    {
        unlink();
    }

    void ReadbackOperation::start() noexcept
    {
        if (!context_)
        {
            finish(cxx::unexpected(RenderError{kWorkStopped}));
            return;
        }
        if (auto reserved = queue_->reserve(callback_); !reserved)
        {
            finish(cxx::unexpected(reserved.error()));
            return;
        }
        next_ = context_->readbacks_;
        if (next_) next_->previous_ = this;
        context_->readbacks_ = this;
        ++context_->readers_;
        linked_ = true;
        arm();
    }

    void ReadbackOperation::unlink() noexcept
    {
        if (!linked_) return;
        if (previous_) previous_->next_ = next_;
        else context_->readbacks_ = next_;
        if (next_) next_->previous_ = previous_;
        --context_->readers_;
        linked_ = false;
        previous_ = next_ = nullptr;
    }

    void ReadbackOperation::cancel() noexcept
    {
        unlink();
        context_ = nullptr;
        // The Queue still owns an armed callback borrow. Its real terminal event releases that borrow.
    }

    void ExecutionContext::cancelReadbacks() noexcept
    {
        while (readbacks_) readbacks_->cancel();
    }

    void ReadbackOperation::arm() noexcept
    {
        if (!context_->lastUse())
        {
            auto executed = context_->execute();
            if (!executed)
            {
                finish(cxx::unexpected(executed.error()));
                return;
            }
            if (*executed && submitted_) submitted_(observer_);
        }
        auto ticket = context_->lastUse();
        if (!ticket) ticket = queue_->oldestPending();
        if (!ticket)
        {
            finish(cxx::unexpected(RenderError{kBusy}));
            return;
        }
        if (!queue_->onComplete(*ticket, callback_, completed, this)) std::terminate();
    }

    void ReadbackOperation::completed(void* value, RenderResult<void> result) noexcept
    {
        auto& self = *static_cast<ReadbackOperation*>(value);
        if (!self.context_)
            self.finish(cxx::unexpected(RenderError{kWorkStopped}));
        else if (!result)
            self.finish(cxx::unexpected(result.error()));
        else
        {
            const auto ticket = self.context_->lastUse();
            if (!ticket || ticket->serial() > self.queue_->completed())
                self.arm(); // Queue capacity is not proof that this Context has a completed output.
            else
                self.finish(self.context_->readback());
        }
    }

    void ReadbackOperation::finish(Result result) noexcept
    {
        unlink();
        context_ = nullptr;
        callback_.reset();
        complete_(owner_, std::move(result));
        // Terminal publication can destroy this operation. No state access follows.
    }
}
