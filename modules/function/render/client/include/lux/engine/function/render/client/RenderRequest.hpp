#pragma once
#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>
#include <lux/engine/function/render/client/RenderProgram.hpp>
#include <lux/cxx/core/move_only_function.hpp>

#include <memory>
#include <utility>

namespace lux::render
{
    template <typename T> class ScopedRenderRequest;

    template <typename T> class RenderRequest
    {
    public:
        RenderRequest() = default;

        [[nodiscard]] bool isReady() const noexcept
        {
            return state_ && state_->ready;
        }
        explicit operator bool() const noexcept
        {
            return isReady();
        }

        [[nodiscard]] bool failed() const noexcept
        {
            return state_ && state_->failed;
        }

        [[nodiscard]] RenderError error() const noexcept
        {
            return state_ ? state_->error : renderError<err::comm::RequestInvalid>();
        }

        [[nodiscard]] bool valid() const noexcept
        {
            return static_cast<bool>(state_);
        }
        [[nodiscard]] RequestId requestId() const noexcept
        {
            return state_ ? state_->request_id : kInvalidRequestId;
        }

        [[nodiscard]] Expected<std::reference_wrapper<const T>> tryResult() const noexcept
        {
            if (!state_)
                return renderFailure<err::comm::RequestInvalid>();
            if (!state_->ready)
                return renderFailure<err::comm::RequestNotReady>();
            if (state_->failed)
                return lux::cxx::unexpected<RenderError>(state_->error);
            return std::cref(state_->value);
        }

        template <typename F> bool then(F&& fn)
        {
            if (!state_)
                return false;
            if (state_->ready)
            {
                fn(state_->value);
            }
            else
            {
                state_->continuation = std::forward<F>(fn);
            }
            return true;
        }

        // Cancels observation only; submitted GPU work remains active.
        void cancel() noexcept
        {
            if (state_)
                state_->continuation.reset();
        }

    private:
        struct State
        {
            T value{};
            lux::cxx::move_only_function<void(const T&)> continuation{};
            bool ready{false};
            bool failed{false};
            RenderError error{};
            RequestId request_id{kInvalidRequestId};
        };

        std::shared_ptr<State> state_;

        explicit RenderRequest(std::shared_ptr<State> s) : state_(std::move(s))
        {
        }

        template <typename U, std::size_t A> friend struct RenderRequestFactory;
    };

    template <typename T> class ScopedRenderRequest
    {
    public:
        explicit ScopedRenderRequest(RenderRequest<T>&& request) noexcept : request_(std::move(request))
        {
        }

        ~ScopedRenderRequest()
        {
            request_.cancel();
        }

        ScopedRenderRequest(const ScopedRenderRequest&) = delete;
        ScopedRenderRequest& operator=(const ScopedRenderRequest&) = delete;

        ScopedRenderRequest(ScopedRenderRequest&& other) noexcept : request_(std::move(other.request_))
        {
        }

        ScopedRenderRequest& operator=(ScopedRenderRequest&& other) noexcept
        {
            if (this != &other)
            {
                request_.cancel();
                request_ = std::move(other.request_);
            }
            return *this;
        }

        template <typename F> bool then(F&& fn)
        {
            return request_.then(std::forward<F>(fn));
        }

        [[nodiscard]] bool valid() const noexcept
        {
            return request_.valid();
        }
        [[nodiscard]] bool isReady() const noexcept
        {
            return request_.isReady();
        }
        [[nodiscard]] bool failed() const noexcept
        {
            return request_.failed();
        }
        [[nodiscard]] RenderError error() const noexcept
        {
            return request_.error();
        }

        /// Stop scoped cancellation and transfer observation to a longer-lived
        /// owner.  The GPU request itself was never cancellable; this is used
        /// by the owner-reaper path that must observe a late resource handle
        /// and compensate it after its scene owner has gone away.
        [[nodiscard]] RenderRequest<T> release() noexcept
        {
            return std::exchange(request_, {});
        }

    private:
        RenderRequest<T> request_;
    };

    template <typename Reply, std::size_t ReplyAlignment = 64> struct RenderRequestFactory
    {
        using Packet = ReplyPacket<ReplyAlignment>;
        using Callback = ReplyDispatchCallback;

        struct Result
        {
            RenderRequest<Reply> request;
            Callback callback;
        };

        static Result make()
        {
            auto state = std::make_shared<typename RenderRequest<Reply>::State>();

            auto settle_failure = [state](RenderError error) {
                state->error = error;
                state->failed = true;
                state->ready = true;
                auto continuation = std::move(state->continuation);
                if (continuation)
                    continuation(state->value);
            };

            auto on_reply = [state, settle_failure](ReplyPacketView pkt, const ReplyRecord& rec) {
                if (rec.type_id == kReplyCommandFailedTypeId)
                {
                    auto failure = pkt.template decode<CommandFailedReply>(rec);
                    if (!failure)
                        settle_failure(failure.error());
                    else
                        settle_failure(failure->error);
                    return;
                }

                auto value = pkt.template decode<Reply>(rec);
                if (!value)
                {
                    settle_failure(value.error());
                    return;
                }

                state->value = *value;
                state->ready = true;
                auto continuation = std::move(state->continuation);
                if (continuation)
                    continuation(state->value);
            };

            auto prepare_main_adoption =
                [state](ReplyPacketView pkt, const ReplyRecord& rec) -> Expected<Callback::MainAdoption> {
                if (rec.type_id == kReplyCommandFailedTypeId)
                {
                    auto failure = pkt.template decode<CommandFailedReply>(rec);
                    if (!failure)
                        return lux::cxx::unexpected(failure.error());

                    const auto error = failure->error;
                    return Callback::MainAdoption{[state, error]() mutable noexcept {
                        state->error = error;
                        state->failed = true;
                        state->ready = true;
                        auto continuation = std::move(state->continuation);
                        if (continuation)
                            continuation(state->value);
                    }};
                }

                auto decoded = pkt.template decode<Reply>(rec);
                if (!decoded)
                    return lux::cxx::unexpected(decoded.error());

                return Callback::MainAdoption{[state, value = std::move(*decoded)]() mutable noexcept {
                    state->value = std::move(value);
                    state->ready = true;
                    auto continuation = std::move(state->continuation);
                    if (continuation)
                        continuation(state->value);
                }};
            };
            return {
                RenderRequest<Reply>(state),
                Callback{std::move(on_reply), std::move(settle_failure), std::move(prepare_main_adoption)}};
        }

        static RenderRequest<Reply> makeImmediate(Reply value)
        {
            auto state = std::make_shared<typename RenderRequest<Reply>::State>();
            state->value = std::move(value);
            state->ready = true;
            return RenderRequest<Reply>(state);
        }

        static RenderRequest<Reply> makeImmediateFailure(RenderError error)
        {
            auto state = std::make_shared<typename RenderRequest<Reply>::State>();
            state->error = error;
            state->failed = true;
            state->ready = true;
            return RenderRequest<Reply>(state);
        }

        static void bindRequestId(RenderRequest<Reply>& request, RequestId request_id) noexcept
        {
            if (request.state_)
                request.state_->request_id = request_id;
        }
    };

} // namespace lux::render
