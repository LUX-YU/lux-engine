#include <lux/engine/render/runtime/ResponseOutbox.hpp>

#include <algorithm>

namespace lux::render::runtime_detail
{
    platform::ipc::IpcMessage& ResponseOutbox::prepare(ResponseRoute route) noexcept
    {
        auto index = free_;
        if (index == SIZE_MAX)
        {
            index = responses_.size();
            responses_.emplace_back();
        }
        else
        {
            free_ = responses_[index].next;
        }
        auto& record = responses_[index];
        record.route = route;
        record.next = SIZE_MAX;
        if (tail_ != SIZE_MAX)
        {
            responses_[tail_].next = index;
        }
        else
        {
            head_ = index;
        }
        tail_ = index;
        ++size_;
        return record.payload;
    }

    ResponseOutbox::FlushResult
    ResponseOutbox::flushTo(platform::ipc::IpcResponder& responder, std::size_t limit) noexcept
    {
        const auto count = std::min(limit, size_);
        FlushResult flushed;
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto index = head_;
            auto& record = responses_[index];
            const auto result =
                responder.trySendResponse(record.route.requester_id, record.route.request_id.value, record.payload);
            head_ = record.next;
            if (head_ == SIZE_MAX)
            {
                tail_ = SIZE_MAX;
            }
            if (!result && result.error() == platform::ipc::EIpcError::WOULD_BLOCK)
            {
                record.next = SIZE_MAX;
                if (tail_ != SIZE_MAX)
                {
                    responses_[tail_].next = index;
                }
                else
                {
                    head_ = index;
                }
                tail_ = index;
                continue;
            }
            record.payload.reset(); // Closed destinations cannot retain ready output indefinitely.
            record.route = {};
            record.next = free_;
            free_ = index;
            --size_;
            ++flushed.settled;
            flushed.published += result.has_value();
        }
        if (flushed.settled != 0)
        {
            // One wakeup for this publication batch lets frame-input producers block on the
            // existing IPC epoch without polling replies or introducing a separate event.
            platform::ipc::IpcCore::instance().notifyWork();
        }
        return flushed;
    }

    void ResponseOutbox::clear() noexcept
    {
        responses_.clear();
        head_ = tail_ = free_ = SIZE_MAX;
        size_ = 0;
    }

} // namespace lux::render::runtime_detail
