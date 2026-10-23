#pragma once

#include <lux/engine/render/runtime/detail/ResponseRoute.hpp>
#include <utility>
#include <vector>

namespace lux::render::runtime_detail
{

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
        platform::ipc::IpcMessage& prepare(ResponseRoute route) noexcept;

        template <class Response, class... Args> Response& emplaceResponse(ResponseRoute route, Args&&... args) noexcept
        {
            return prepare(route).emplace<Response>(std::forward<Args>(args)...);
        }

        // Returns settled records, including replies discarded because their destination closed.
        // limit bounds this service pass; it does not bound a frame or the number of accepted requests.
        std::size_t flushTo(platform::ipc::IpcResponder& responder, std::size_t limit) noexcept;

        [[nodiscard]] bool empty() const noexcept
        {
            return size_ == 0;
        }

        void clear() noexcept;

    private:
        std::vector<PendingResponse> responses_;
        std::size_t head_ = SIZE_MAX, tail_ = SIZE_MAX, free_ = SIZE_MAX, size_ = 0;
    };
} // namespace lux::render::runtime_detail
