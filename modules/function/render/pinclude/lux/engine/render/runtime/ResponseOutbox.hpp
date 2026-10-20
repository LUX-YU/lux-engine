#pragma once

#include <algorithm>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <lux/engine/render/commands/Command.hpp>
#include <vector>

namespace lux::render::runtime_detail
{

    struct ResponseRoute final
    {
        platform::ipc::RequesterId requester_id;
        RequestId request_id;
    };

    struct PendingResponse final
    {
        ResponseRoute route;
        platform::ipc::IpcMessage payload;
        std::size_t next = SIZE_MAX;
    };

    // RenderThread only. Reusable records, one ready FIFO and one free chain in the same storage.
    // Dispatch is non-reentrant: no append/flush occurs while a synchronous Handler borrows its response.
    class ResponseOutbox final
    {
    public:
        platform::ipc::IpcMessage& prepare(ResponseRoute route) noexcept
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

        template <class Response, class... Args> Response& emplaceResponse(ResponseRoute route, Args&&... args) noexcept
        {
            return prepare(route).emplace<Response>(std::forward<Args>(args)...);
        }

        void enqueue(ResponseRoute route, platform::ipc::IpcMessage&& payload) noexcept
        {
            prepare(route) = std::move(payload);
        }

        std::size_t flushTo(platform::ipc::IpcResponder& responder, std::size_t budget) noexcept
        {
            const auto count = std::min(budget, size_);
            std::size_t delivered = 0;
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
                ++delivered;
            }
            return delivered;
        }

        [[nodiscard]] bool empty() const noexcept
        {
            return size_ == 0;
        }

        void clear() noexcept
        {
            responses_.clear();
            head_ = tail_ = free_ = SIZE_MAX;
            size_ = 0;
        }

    private:
        std::vector<PendingResponse> responses_;
        std::size_t head_ = SIZE_MAX, tail_ = SIZE_MAX, free_ = SIZE_MAX, size_ = 0;
    };
} // namespace lux::render::runtime_detail
