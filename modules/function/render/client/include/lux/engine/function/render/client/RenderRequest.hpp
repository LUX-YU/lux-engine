#pragma once
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/function/render/client/RenderProgram.hpp>
#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>

#include <memory>
#include <optional>
#include <utility>

namespace lux::render
{
    template <typename T> class TScopedRenderRequest;

    template <typename T> class TRenderRequest
    {
    public:
        using Outcome = Expected<T>;

        TRenderRequest() = default;

        [[nodiscard]] bool isReady() const noexcept
        {
            return state_ && state_->outcome.has_value();
        }

        explicit operator bool() const noexcept
        {
            return isReady();
        }

        [[nodiscard]] bool failed() const noexcept
        {
            return isReady() && !state_->outcome->has_value();
        }

        [[nodiscard]] RenderError error() const noexcept
        {
            if (!state_)
            {
                return renderError<err::comm::RequestInvalid>();
            }
            return failed() ? state_->outcome->error() : RenderError{};
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
            {
                return renderFailure<err::comm::RequestInvalid>();
            }
            if (!state_->outcome)
            {
                return renderFailure<err::comm::RequestNotReady>();
            }
            if (!*state_->outcome)
            {
                return lux::cxx::unexpected<RenderError>(state_->outcome->error());
            }
            return std::cref(**state_->outcome);
        }

        // Every terminal outcome is delivered, including failure. Cancellation only
        // removes this observation; it cannot cancel accepted backend work.
        template <typename F> bool then(F&& fn)
        {
            const auto state = state_;
            if (!state)
            {
                return false;
            }
            if (state->outcome)
            {
                fn(*state->outcome);
            }
            else
            {
                state->continuation = std::forward<F>(fn);
            }
            return true;
        }

        // Cancels observation only; submitted GPU work remains active.
        void cancel() noexcept
        {
            if (state_)
            {
                state_->continuation.reset();
            }
        }

    private:
        struct State
        {
            std::optional<Outcome> outcome;
            lux::cxx::move_only_function<void(const Outcome&)> continuation;
            RequestId request_id{kInvalidRequestId};
        };

        static void settle(std::shared_ptr<State> state, Outcome outcome)
        {
            if (state->outcome)
            {
                return;
            }
            state->outcome.emplace(std::move(outcome));
            auto continuation = std::move(state->continuation);
            if (continuation)
            {
                continuation(*state->outcome);
            }
        }

        std::shared_ptr<State> state_;

        explicit TRenderRequest(std::shared_ptr<State> s) : state_(std::move(s)) {}

        template <typename U, std::size_t A> friend struct TRenderRequestFactory;
    };

    template <typename T> class TScopedRenderRequest
    {
    public:
        explicit TScopedRenderRequest(TRenderRequest<T>&& request) noexcept : request_(std::move(request)) {}

        ~TScopedRenderRequest()
        {
            request_.cancel();
        }

        TScopedRenderRequest(const TScopedRenderRequest&) = delete;
        TScopedRenderRequest& operator=(const TScopedRenderRequest&) = delete;

        TScopedRenderRequest(TScopedRenderRequest&& other) noexcept : request_(std::move(other.request_)) {}

        TScopedRenderRequest& operator=(TScopedRenderRequest&& other) noexcept
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
        [[nodiscard]] TRenderRequest<T> release() noexcept
        {
            return std::exchange(request_, {});
        }

    private:
        TRenderRequest<T> request_;
    };

    template <typename Reply, std::size_t ReplyAlignment = 64> struct TRenderRequestFactory
    {
        using Packet = TReplyPacket<ReplyAlignment>;
        using Callback = ReplyDispatchCallback;
        using Request = TRenderRequest<Reply>;

        struct Result
        {
            TRenderRequest<Reply> request;
            Callback callback;
        };

        static Result make()
        {
            auto state = std::make_shared<typename TRenderRequest<Reply>::State>();

            auto settle_failure = [state](RenderError error) { Request::settle(state, lux::cxx::unexpected(error)); };

            auto on_reply = [state](ReplyPacketView pkt, const ReplyRecord& rec)
            {
                if (rec.type_id == kReplyCommandFailedTypeId)
                {
                    auto failure = pkt.template decode<CommandFailedReply>(rec);
                    if (!failure)
                    {
                        Request::settle(state, lux::cxx::unexpected(failure.error()));
                    }
                    else
                    {
                        Request::settle(state, lux::cxx::unexpected(failure->error));
                    }
                    return;
                }

                auto value = pkt.template decode<Reply>(rec);
                if (!value)
                {
                    Request::settle(state, lux::cxx::unexpected(value.error()));
                    return;
                }

                Request::settle(state, std::move(*value));
            };

            auto prepare_main_adoption = [state](
                ReplyPacketView pkt,
                const ReplyRecord& rec
            ) -> Expected<Callback::MainAdoption>
            {
                if (rec.type_id == kReplyCommandFailedTypeId)
                {
                    auto failure = pkt.template decode<CommandFailedReply>(rec);
                    if (!failure)
                    {
                        return lux::cxx::unexpected(failure.error());
                    }

                    const auto error = failure->error;
                    return Callback::MainAdoption{
                        [state, error]() noexcept
                        {
                            Request::settle(state, lux::cxx::unexpected(error));
                        }
                    };
                }

                auto decoded = pkt.template decode<Reply>(rec);
                if (!decoded)
                {
                    return lux::cxx::unexpected(decoded.error());
                }

                return Callback::MainAdoption{
                    [state, value = std::move(*decoded)]() mutable noexcept
                    {
                        Request::settle(state, std::move(value));
                    }
                };
            };
            return {
                TRenderRequest<Reply>(state),
                Callback{std::move(on_reply), std::move(settle_failure), std::move(prepare_main_adoption)}
            };
        }

        static TRenderRequest<Reply> makeImmediate(Reply value)
        {
            auto state = std::make_shared<typename TRenderRequest<Reply>::State>();
            Request::settle(state, std::move(value));
            return TRenderRequest<Reply>(state);
        }

        static TRenderRequest<Reply> makeImmediateFailure(RenderError error)
        {
            auto state = std::make_shared<typename TRenderRequest<Reply>::State>();
            Request::settle(state, lux::cxx::unexpected(error));
            return TRenderRequest<Reply>(state);
        }

        static void bindRequestId(TRenderRequest<Reply>& request, RequestId request_id) noexcept
        {
            if (request.state_)
            {
                request.state_->request_id = request_id;
            }
        }
    };

} // namespace lux::render
