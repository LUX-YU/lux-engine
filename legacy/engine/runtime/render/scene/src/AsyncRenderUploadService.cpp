#include <lux/engine/runtime/render/scene/AsyncRenderUploadService.hpp>
#include <lux/engine/runtime/render/scene/AsyncRenderUploadCloseSender.hpp>

#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/function/render/client/RenderUploadSession.hpp>

#include <atomic>
#include <new>
#include <utility>
#include <vector>

namespace lux::runtime
{
    namespace
    {
        using Completion =
            lux::exec::AsyncOperationCompletion<SubmitRenderUpload>;

        struct UploadAttempt final
        {
            UploadAttempt(
                std::shared_ptr<lux::render::detail::PreparedUpload> value,
                Completion terminal) noexcept
                : prepared(std::move(value))
                , completion(std::move(terminal))
            {}

            std::shared_ptr<lux::render::detail::PreparedUpload> prepared;
            Completion completion;
        };

    }

    struct AsyncRenderUploadService::State final :
        std::enable_shared_from_this<AsyncRenderUploadService::State>
    {
        class ProducerTicket final
        {
        public:
            ProducerTicket() noexcept = default;
            explicit ProducerTicket(State* state) noexcept : state_(state) {}
            ProducerTicket(const ProducerTicket&) = delete;
            ProducerTicket& operator=(const ProducerTicket&) = delete;
            ProducerTicket(ProducerTicket&& other) noexcept
                : state_(std::exchange(other.state_, nullptr))
            {}
            ~ProducerTicket()
            {
                if (state_)
                    state_->releaseProducer();
            }

            [[nodiscard]] explicit operator bool() const noexcept
            {
                return state_ != nullptr;
            }

        private:
            State* state_{nullptr};
        };

        [[nodiscard]] static lux::render::UploadSubmitNoReplyResult
        submitTrampoline(
            void* opaque,
            std::shared_ptr<lux::render::detail::PreparedUpload> prepared)
            noexcept
        {
            return static_cast<State*>(opaque)->submit(std::move(prepared));
        }

        [[nodiscard]] lux::render::UploadSubmitNoReplyResult submit(
            std::shared_ptr<lux::render::detail::PreparedUpload> prepared)
            noexcept
        {
            auto producer = acquireProducer();
            if (!producer || !prepared || !async)
            {
                return lux::cxx::unexpected(
                    lux::render::ERenderUploadSubmitError::STOPPING);
            }

            accepted_inflight.fetch_add(1u, std::memory_order_acq_rel);
            const auto bytes = prepared->packet.accountedBytes();
            auto result = async.tryNotify(
                SubmitRenderUpload{
                    std::move(prepared),
                    SubmitRenderUploadAdmission{
                        std::static_pointer_cast<void>(shared_from_this()),
                        +[](void* opaque) noexcept
                        {
                            static_cast<State*>(opaque)->finishAccepted();
                        }}},
                lux::async::SubmitOptions{
                    .accounted_bytes = bytes});
            if (result)
                return {};
            switch (result.error())
            {
            case lux::async::ESubmitError::BYTE_BUDGET_EXHAUSTED:
                return lux::cxx::unexpected(
                    lux::render::ERenderUploadSubmitError::
                        BYTE_BUDGET_EXHAUSTED);
            case lux::async::ESubmitError::PAYLOAD_INVALID:
            case lux::async::ESubmitError::UNKNOWN_OPERATION:
                return lux::cxx::unexpected(
                    lux::render::ERenderUploadSubmitError::PAYLOAD_INVALID);
            case lux::async::ESubmitError::QUEUE_FULL:
                return lux::cxx::unexpected(
                    lux::render::ERenderUploadSubmitError::QUEUE_FULL);
            case lux::async::ESubmitError::FEATURE_CLOSING:
            case lux::async::ESubmitError::STOPPING:
                return lux::cxx::unexpected(
                    lux::render::ERenderUploadSubmitError::STOPPING);
            }
            return lux::cxx::unexpected(
                lux::render::ERenderUploadSubmitError::PAYLOAD_INVALID);
        }

        void dispatch(
            SubmitRenderUpload&& operation,
            lux::exec::AsyncOperationContext& context,
            Completion completion) noexcept
        {
            operation.admission.disarm();
            if (!session || !operation.prepared ||
                !session->claimCoordinatorThread())
            {
                completion.complete(lux::cxx::unexpected(
                    lux::async::OperationFailure<
                        lux::render::ERenderUploadSubmitError>::domain(
                            lux::render::ERenderUploadSubmitError::STOPPING)));
                finishAccepted();
                return;
            }
            if (!main)
                main = context.mainThreadDispatcher();
            attempt(std::make_shared<UploadAttempt>(
                std::move(operation.prepared),
                std::move(completion)));
        }

        void attempt(const std::shared_ptr<UploadAttempt>& value) noexcept
        {
            if (!session || !value || !value->prepared)
            {
                fail(value, lux::render::ERenderUploadSubmitError::STOPPING);
                return;
            }

            if (value->prepared->expected_reply_type ==
                lux::render::kInvalidTypeId)
            {
                auto result = session->trySubmitPreparedNoReply(
                    value->prepared->packet);
                if (result)
                {
                    value->completion.complete({});
                    finishAccepted();
                    return;
                }
                if (result.error() ==
                        lux::render::ERenderUploadSubmitError::QUEUE_FULL ||
                    result.error() == lux::render::
                        ERenderUploadSubmitError::BYTE_BUDGET_EXHAUSTED)
                {
                    retainRetry(value);
                    return;
                }
                fail(value, result.error());
                return;
            }

            std::weak_ptr<State> weak_self = shared_from_this();
            lux::render::ReplyDispatchCallback callback{
                [weak_self, value](
                    lux::render::ReplyPacketView packet,
                    const lux::render::ReplyRecord& record) noexcept
                {
                    auto self = weak_self.lock();
                    if (self)
                        self->settleReply(value, packet, record);
                },
                [weak_self, value](
                    lux::render::RenderError error) noexcept
                {
                    auto self = weak_self.lock();
                    if (self)
                        self->settleFailure(value, error);
                }};

            auto result = session->trySubmitPrepared(
                value->prepared->packet,
                value->prepared->expected_reply_type,
                std::move(callback));
            if (result)
            {
                active_replies.fetch_add(1u, std::memory_order_relaxed);
                return;
            }
            if (result.error() ==
                    lux::render::ERenderUploadSubmitError::QUEUE_FULL ||
                result.error() ==
                    lux::render::ERenderUploadSubmitError::
                        BYTE_BUDGET_EXHAUSTED)
            {
                retainRetry(value);
                return;
            }
            fail(value, result.error());
        }

        void pump() noexcept
        {
            if (!session || !session->claimCoordinatorThread())
                return;
            session->pumpReplies();
            if (retries.empty())
            {
                tryCompleteClose();
                return;
            }

            retry_drain.clear();
            retry_drain.swap(retries);
            retry_count.store(0u, std::memory_order_release);
            for (auto& value : retry_drain)
                attempt(value);
            retry_drain.clear();
            tryCompleteClose();
        }

        void settleReply(
            std::shared_ptr<UploadAttempt> value,
            lux::render::ReplyPacketView packet,
            const lux::render::ReplyRecord& record) noexcept
        {
            auto adoption = value->prepared->callback.prepareMainAdoption(
                packet,
                record);
            if (!adoption)
            {
                settleFailure(std::move(value), adoption.error());
                return;
            }

            auto self = shared_from_this();
            if (!main.tryDispatchToMainThread(
                    [self,
                     value,
                     adoption = std::move(*adoption)]() mutable noexcept
                    {
                        adoption();
                        value->completion.complete({});
                        self->finishAccepted();
                        self->active_replies.fetch_sub(
                            1u,
                            std::memory_order_release);
                        (void)self->wake_signal.notify();
                    }))
            {
                fail(value, lux::render::ERenderUploadSubmitError::STOPPING);
                active_replies.fetch_sub(1u, std::memory_order_release);
                (void)wake_signal.notify();
            }
        }

        void settleFailure(
            std::shared_ptr<UploadAttempt> value,
            lux::render::RenderError error) noexcept
        {
            auto callback = std::move(value->prepared->callback);
            auto self = shared_from_this();
            if (!main.tryDispatchToMainThread(
                    [self,
                     value,
                     callback = std::move(callback),
                     error]() mutable noexcept
                    {
                        (void)callback.settleFailure(error);
                        value->completion.complete(lux::cxx::unexpected(
                            lux::async::OperationFailure<
                                lux::render::ERenderUploadSubmitError>::domain(
                                    lux::render::ERenderUploadSubmitError::
                                        PAYLOAD_INVALID)));
                        self->finishAccepted();
                        self->active_replies.fetch_sub(
                            1u,
                            std::memory_order_release);
                        (void)self->wake_signal.notify();
                    }))
            {
                fail(value, lux::render::ERenderUploadSubmitError::STOPPING);
                active_replies.fetch_sub(1u, std::memory_order_release);
                (void)wake_signal.notify();
            }
        }

        struct CloseWaiter final
        {
            explicit CloseWaiter(
                lux::cxx::move_only_function<void(
                    AsyncRenderUploadCloseReport)> value) noexcept
                : completion(std::move(value))
            {}

            lux::cxx::move_only_function<void(
                AsyncRenderUploadCloseReport)> completion;
            CloseWaiter* next{nullptr};
        };

        [[nodiscard]] static CloseWaiter* completedSentinel() noexcept
        {
            return reinterpret_cast<CloseWaiter*>(std::uintptr_t{1u});
        }

        [[nodiscard]] AsyncRenderUploadCloseReport report() const noexcept
        {
            const auto retry = retry_count.load(std::memory_order_acquire);
            const auto active = active_replies.load(std::memory_order_acquire);
            const auto accepted = accepted_inflight.load(
                std::memory_order_acquire);
            return {
                .pending_backpressure = retry,
                .active_replies = active,
                .accepted_inflight = accepted,
                .retry_attempts = retry_attempts.load(
                    std::memory_order_relaxed),
                .retry_high_water = retry_high_water.load(
                    std::memory_order_relaxed),
                .clean = retry == 0u && active == 0u && accepted == 0u};
        }

        static void subscribeCloseTrampoline(
            void* opaque,
            AsyncRenderUploadCloseSender::Completion completion) noexcept
        {
            static_cast<State*>(opaque)->subscribeClose(
                std::move(completion));
        }

        void subscribeClose(
            lux::cxx::move_only_function<void(
                AsyncRenderUploadCloseReport)> completion) noexcept
        {
            closeAdmission();
            auto* waiter = new (std::nothrow) CloseWaiter{
                std::move(completion)};
            if (waiter == nullptr)
                std::terminate();

            auto* head = close_waiters.load(std::memory_order_acquire);
            for (;;)
            {
                if (head == completedSentinel())
                {
                    auto terminal = std::move(waiter->completion);
                    delete waiter;
                    terminal(report());
                    return;
                }
                waiter->next = head;
                if (close_waiters.compare_exchange_weak(
                        head,
                        waiter,
                        std::memory_order_release,
                        std::memory_order_acquire))
                    break;
            }
            (void)wake_signal.notify();
        }

        void tryCompleteClose() noexcept
        {
            const auto close_report = report();
            if (isAccepting() || activeProducers() != 0u ||
                !close_report.clean)
                return;

            auto* waiter = close_waiters.exchange(
                completedSentinel(),
                std::memory_order_acq_rel);
            if (waiter == completedSentinel())
                return;
            while (waiter != nullptr)
            {
                auto* next = waiter->next;
                auto completion = std::move(waiter->completion);
                delete waiter;
                completion(close_report);
                waiter = next;
            }
        }

        void fail(
            const std::shared_ptr<UploadAttempt>& value,
            lux::render::ERenderUploadSubmitError error) noexcept
        {
            if (!value)
                return;
            value->completion.complete(lux::cxx::unexpected(
                lux::async::OperationFailure<
                    lux::render::ERenderUploadSubmitError>::domain(error)));
            finishAccepted();
        }

        void retainRetry(
            const std::shared_ptr<UploadAttempt>& value) noexcept
        {
            retries.push_back(value);
            const auto size = retries.size();
            retry_count.store(size, std::memory_order_release);
            retry_attempts.fetch_add(1u, std::memory_order_relaxed);
            auto high = retry_high_water.load(std::memory_order_relaxed);
            while (high < size &&
                   !retry_high_water.compare_exchange_weak(
                       high,
                       size,
                       std::memory_order_relaxed,
                       std::memory_order_relaxed))
            {}
        }

        [[nodiscard]] ProducerTicket acquireProducer() noexcept
        {
            auto gate = admission_gate.load(std::memory_order_acquire);
            while ((gate & kOpenBit) != 0u)
            {
                if (admission_gate.compare_exchange_weak(
                        gate,
                        gate + kProducerIncrement,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire))
                    return ProducerTicket{this};
            }
            return {};
        }

        void releaseProducer() noexcept
        {
            const auto previous = admission_gate.fetch_sub(
                kProducerIncrement,
                std::memory_order_acq_rel);
            if ((previous >> 1u) == 1u &&
                (previous & kOpenBit) == 0u)
            {
                (void)wake_signal.notify();
            }
        }

        void closeAdmission() noexcept
        {
            admission_gate.fetch_and(~kOpenBit, std::memory_order_acq_rel);
            (void)wake_signal.notify();
        }

        [[nodiscard]] bool isAccepting() const noexcept
        {
            return (admission_gate.load(std::memory_order_acquire) &
                    kOpenBit) != 0u;
        }

        [[nodiscard]] std::uint64_t activeProducers() const noexcept
        {
            return admission_gate.load(std::memory_order_acquire) >> 1u;
        }

        void openAdmission() noexcept
        {
            admission_gate.store(kOpenBit, std::memory_order_release);
        }

        void finishAccepted() noexcept
        {
            accepted_inflight.fetch_sub(1u, std::memory_order_acq_rel);
            (void)wake_signal.notify();
        }

        static void wake(void* opaque) noexcept
        {
            auto* self = static_cast<State*>(opaque);
            (void)self->wake_signal.notify();
        }

        lux::async::OperationPort<SubmitRenderUpload> async;
        lux::exec::MainThreadDispatcher main;
        lux::exec::CoordinatorSignal wake_signal;
        lux::render::RenderUploadSession* session{nullptr};
        std::shared_ptr<lux::render::RenderChannelSync> sync;
        std::vector<std::shared_ptr<UploadAttempt>> retries;
        std::vector<std::shared_ptr<UploadAttempt>> retry_drain;
        std::atomic<std::size_t> retry_count{0u};
        std::atomic<std::size_t> active_replies{0u};
        static constexpr std::uint64_t kOpenBit{1u};
        static constexpr std::uint64_t kProducerIncrement{2u};
        std::atomic<std::uint64_t> admission_gate{0u};
        std::atomic<std::size_t> accepted_inflight{0u};
        std::atomic<std::uint64_t> retry_attempts{0u};
        std::atomic<std::size_t> retry_high_water{0u};
        std::atomic<CloseWaiter*> close_waiters{nullptr};
        std::shared_ptr<lux::render::detail::RenderUploadClientMetrics>
            metrics{
                std::make_shared<
                    lux::render::detail::RenderUploadClientMetrics>()};
    };

    void detail::subscribeRenderUploadClose(
        AsyncRenderUploadService& service,
        lux::cxx::move_only_function<void(
            AsyncRenderUploadCloseReport)> completion) noexcept
    {
        if (!service.state_)
        {
            completion({});
            return;
        }
        service.state_->subscribeClose(std::move(completion));
    }

    lux::cxx::expected<
        AsyncRenderUploadService,
        lux::exec::AsyncAssemblyFailure>
    AsyncRenderUploadService::addTo(
        lux::exec::AsyncRuntimeBuilder& builder)
    {
        auto state = std::make_shared<State>();
        auto added = builder.addOperation<SubmitRenderUpload>(
            [state](
                SubmitRenderUpload&& operation,
                lux::exec::AsyncOperationContext& context,
                Completion completion) noexcept
            {
                state->dispatch(
                    std::move(operation),
                    context,
                    std::move(completion));
            }, {}, lux::exec::AsyncOperationQueueConfig{
                .capacity = 1024,
                .byte_budget = 256u * 1024u * 1024u,
                .drain_batch = 64});
        if (!added)
            return lux::cxx::unexpected(added.error());
        state->async = std::move(*added);
        return AsyncRenderUploadService{std::move(state)};
    }

    AsyncRenderUploadService::AsyncRenderUploadService(
        AsyncRenderUploadService&& other) noexcept = default;

    AsyncRenderUploadService& AsyncRenderUploadService::operator=(
        AsyncRenderUploadService&& other) noexcept = default;

    AsyncRenderUploadService::~AsyncRenderUploadService()
    {
        if (state_)
            state_->closeAdmission();
    }

    bool AsyncRenderUploadService::bind(
        lux::exec::AsyncRuntime& runtime,
        lux::render::RenderUploadSession& session,
        const std::shared_ptr<lux::render::RenderChannelSync>& sync) noexcept
    {
        if (!state_ || !sync)
            return false;
        state_->main = runtime.mainThreadDispatcher();
        state_->session = &session;
        state_->sync = sync;
        state_->openAdmission();
        session.markCoordinatorOwned();
        sync->bindExternalWake(state_.get(), &State::wake);
        std::weak_ptr<State> weak_state = state_;
        state_->wake_signal = runtime.makeCoordinatorSignal(
            [weak_state]() noexcept
            {
                if (auto state = weak_state.lock())
                    state->pump();
            });
        return static_cast<bool>(state_->wake_signal);
    }

    lux::render::RenderUploadClient
    AsyncRenderUploadService::client() const noexcept
    {
        if (!state_)
            return {};
        return lux::render::RenderUploadClient::bind(
            std::static_pointer_cast<void>(state_),
            &State::submitTrampoline,
            state_->metrics);
    }

    AsyncRenderUploadCloseSender AsyncRenderUploadService::closeAsync() noexcept
    {
        if (!state_)
            return {};
        return AsyncRenderUploadCloseSender{
            std::static_pointer_cast<void>(state_),
            &State::subscribeCloseTrampoline};
    }

    AsyncRenderUploadCloseReport
    AsyncRenderUploadService::report() const noexcept
    {
        if (!state_)
            return {};
        return state_->report();
    }

    void AsyncRenderUploadService::unbind() noexcept
    {
        if (!state_ || !state_->sync)
            return;
        state_->sync->unbindExternalWake();
        state_->wake_signal = {};
        state_->session = nullptr;
        state_->sync.reset();
    }
}
