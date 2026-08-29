#include <lux/engine/process/PortSender.hpp>

#include <stdexec/execution.hpp>

#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <utility>

namespace
{
    enum class EDomainError : std::uint8_t
    {
        FAILED
    };

    struct Read final
    {
        using Value = int;
        using Error = EDomainError;

        int value{};
    };

    struct Notify final
    {
        using Value = void;
        using Error = EDomainError;
    };

    enum class EMode : std::uint8_t
    {
        VALUE,
        DOMAIN_ERROR,
        RUNTIME_ERROR,
        DEFERRED_VALUE,
        REJECT_WITH_CALLBACK,
        REJECT_WITHOUT_CALLBACK
    };

    template <lux::async::Operation Operation>
    class Endpoint final : public lux::async::detail::OperationEndpoint<Operation>
    {
    public:
        using Outcome = typename lux::async::detail::OperationEndpoint<Operation>::Outcome;

        explicit Endpoint(EMode mode) noexcept : mode_(mode)
        {
        }

        [[nodiscard]] lux::async::SubmitResult submit(
            Operation operation,
            void* state,
            void (*complete)(void*, Outcome&&) noexcept,
            lux::async::SubmitOptions options
        ) noexcept override
        {
            ++submits;
            observed_bytes = options.accounted_bytes;
            if constexpr (std::is_same_v<Operation, Read>)
                observed_value = operation.value;

            if (mode_ == EMode::DEFERRED_VALUE)
            {
                state_ = state;
                complete_ = complete;
                if constexpr (std::is_same_v<Operation, Read>)
                    deferred_value_ = operation.value;
                return {};
            }
            if (mode_ == EMode::VALUE)
            {
                completeValue(state, complete, operation);
                return {};
            }
            if (mode_ == EMode::DOMAIN_ERROR)
            {
                complete(
                    state,
                    lux::cxx::unexpected(lux::async::OperationFailure<EDomainError>::domain(EDomainError::FAILED))
                );
                return {};
            }
            if (mode_ == EMode::RUNTIME_ERROR)
            {
                complete(
                    state,
                    lux::cxx::unexpected(
                        lux::async::OperationFailure<EDomainError>::runtime(lux::async::ESubmitError::STOPPING)
                    )
                );
                return {};
            }
            if (mode_ == EMode::REJECT_WITH_CALLBACK)
            {
                complete(
                    state,
                    lux::cxx::unexpected(
                        lux::async::OperationFailure<EDomainError>::runtime(lux::async::ESubmitError::QUEUE_FULL)
                    )
                );
            }
            return lux::cxx::unexpected(lux::async::ESubmitError::QUEUE_FULL);
        }

        void finish() noexcept
        {
            const auto complete = std::exchange(complete_, nullptr);
            const auto state = std::exchange(state_, nullptr);
            assert(complete != nullptr);
            if constexpr (std::is_void_v<typename Operation::Value>)
                complete(state, Outcome{});
            else
                complete(state, Outcome{deferred_value_});
        }

        std::size_t submits{};
        std::size_t observed_bytes{};
        int observed_value{};

    private:
        static void completeValue(
            void* state,
            void (*complete)(void*, Outcome&&) noexcept,
            const Operation& operation
        ) noexcept
        {
            if constexpr (std::is_void_v<typename Operation::Value>)
                complete(state, Outcome{});
            else
                complete(state, Outcome{operation.value});
        }

        EMode mode_{};
        void* state_{};
        void (*complete_)(void*, Outcome&&) noexcept{};
        int deferred_value_{};
    };

    enum class ETerminal : std::uint8_t
    {
        NONE,
        VALUE,
        ERROR,
        STOPPED
    };

    template <class Value>
    struct Outcome final
    {
        std::atomic<std::size_t> completions{};
        ETerminal terminal{ETerminal::NONE};
        Value value{};
        bool runtime_error{};
        lux::async::ESubmitError submit_error{lux::async::ESubmitError::UNKNOWN_OPERATION};
        EDomainError domain_error{EDomainError::FAILED};
    };

    template <class Value>
    struct Receiver final
    {
        using receiver_concept = stdexec::receiver_t;
        using Failure = lux::async::OperationFailure<EDomainError>;

        void set_value(Value value) && noexcept
        {
            outcome->value = value;
            finish(ETerminal::VALUE);
        }

        void set_error(Failure failure) && noexcept
        {
            outcome->runtime_error = failure.isRuntime();
            if (failure.isRuntime())
                outcome->submit_error = failure.runtimeError();
            else
                outcome->domain_error = failure.domainError();
            finish(ETerminal::ERROR);
        }

        void set_stopped() && noexcept
        {
            finish(ETerminal::STOPPED);
        }

        [[nodiscard]] auto get_env() const noexcept
        {
            return stdexec::prop{stdexec::get_stop_token, stop_token};
        }

    private:
        void finish(ETerminal terminal) noexcept
        {
            outcome->terminal = terminal;
            outcome->completions.fetch_add(1U, std::memory_order_release);
        }

    public:
        Outcome<Value>* outcome{};
        stdexec::inplace_stop_token stop_token;
    };

    template <>
    struct Receiver<void> final
    {
        using receiver_concept = stdexec::receiver_t;
        using Failure = lux::async::OperationFailure<EDomainError>;

        void set_value() && noexcept
        {
            finish(ETerminal::VALUE);
        }

        void set_error(Failure failure) && noexcept
        {
            outcome->runtime_error = failure.isRuntime();
            if (failure.isRuntime())
                outcome->submit_error = failure.runtimeError();
            else
                outcome->domain_error = failure.domainError();
            finish(ETerminal::ERROR);
        }

        void set_stopped() && noexcept
        {
            finish(ETerminal::STOPPED);
        }

        [[nodiscard]] auto get_env() const noexcept
        {
            return stdexec::prop{stdexec::get_stop_token, stop_token};
        }

    private:
        void finish(ETerminal terminal) noexcept
        {
            outcome->terminal = terminal;
            outcome->completions.fetch_add(1U, std::memory_order_release);
        }

    public:
        Outcome<int>* outcome{};
        stdexec::inplace_stop_token stop_token;
    };

    template <class Operation, class Result>
    void runImmediate(EMode mode, Operation operation, Result& result)
    {
        auto endpoint = std::make_shared<Endpoint<Operation>>(mode);
        lux::async::OperationPort<Operation> port{endpoint};
        stdexec::inplace_stop_source stop;
        auto sender = lux::process::portSender(port, std::move(operation), {.accounted_bytes = 17U});
        auto state = stdexec::connect(std::move(sender), Receiver<typename Operation::Value>{&result, stop.get_token()});
        stdexec::start(state);
        assert(result.completions.load(std::memory_order_acquire) == 1U);
        assert(endpoint->submits == 1U);
        assert(endpoint->observed_bytes == 17U);
    }
}

int main()
{
    static_assert(stdexec::sender<lux::process::PortSender<Read>>);

    Outcome<int> void_value;
    runImmediate(EMode::VALUE, Notify{}, void_value);
    assert(void_value.terminal == ETerminal::VALUE);

    Outcome<int> value;
    runImmediate(EMode::VALUE, Read{42}, value);
    assert(value.terminal == ETerminal::VALUE);
    assert(value.value == 42);

    Outcome<int> domain;
    runImmediate(EMode::DOMAIN_ERROR, Read{1}, domain);
    assert(domain.terminal == ETerminal::ERROR);
    assert(!domain.runtime_error);

    Outcome<int> runtime;
    runImmediate(EMode::RUNTIME_ERROR, Read{1}, runtime);
    assert(runtime.terminal == ETerminal::ERROR);
    assert(runtime.runtime_error);
    assert(runtime.submit_error == lux::async::ESubmitError::STOPPING);

    Outcome<int> rejected_callback;
    runImmediate(EMode::REJECT_WITH_CALLBACK, Read{1}, rejected_callback);
    assert(rejected_callback.terminal == ETerminal::ERROR);
    assert(rejected_callback.submit_error == lux::async::ESubmitError::QUEUE_FULL);

    Outcome<int> rejected_without_callback;
    runImmediate(EMode::REJECT_WITHOUT_CALLBACK, Read{1}, rejected_without_callback);
    assert(rejected_without_callback.terminal == ETerminal::ERROR);
    assert(rejected_without_callback.submit_error == lux::async::ESubmitError::QUEUE_FULL);

    auto deferred_endpoint = std::make_shared<Endpoint<Read>>(EMode::DEFERRED_VALUE);
    lux::async::OperationPort<Read> deferred_port{deferred_endpoint};
    stdexec::inplace_stop_source deferred_stop;
    Outcome<int> deferred;
    auto deferred_state = stdexec::connect(
        lux::process::portSender(deferred_port, Read{71}),
        Receiver<int>{&deferred, deferred_stop.get_token()}
    );
    stdexec::start(deferred_state);
    assert(deferred.completions.load(std::memory_order_acquire) == 0U);
    deferred_endpoint->finish();
    assert(deferred.completions.load(std::memory_order_acquire) == 1U);
    assert(deferred.value == 71);

    // A started operation owns its Endpoint through the Port value in the sender
    // state. Requester-side Port/Endpoint destruction and a later stop request do
    // not invalidate the completion state; a higher-level workflow may discard
    // the completed value after observing its captured stop token.
    std::weak_ptr<Endpoint<Read>> endpoint_lifetime;
    {
        auto endpoint = std::make_shared<Endpoint<Read>>(EMode::DEFERRED_VALUE);
        endpoint_lifetime = endpoint;
        lux::async::OperationPort<Read> port{endpoint};
        stdexec::inplace_stop_source stop;
        Outcome<int> late;
        auto state = stdexec::connect(
            lux::process::portSender(port, Read{91}),
            Receiver<int>{&late, stop.get_token()}
        );
        endpoint.reset();
        port = {};
        stdexec::start(state);
        static_cast<void>(stop.request_stop());
        auto retained_endpoint = endpoint_lifetime.lock();
        assert(retained_endpoint != nullptr);
        retained_endpoint->finish();
        retained_endpoint.reset();
        assert(late.completions.load(std::memory_order_acquire) == 1U);
        assert(late.terminal == ETerminal::VALUE);
        assert(late.value == 91);
    }
    assert(endpoint_lifetime.expired());

    Outcome<int> missing;
    stdexec::inplace_stop_source missing_stop;
    auto missing_state = stdexec::connect(
        lux::process::portSender(lux::async::OperationPort<Read>{}, Read{5}),
        Receiver<int>{&missing, missing_stop.get_token()}
    );
    stdexec::start(missing_state);
    assert(missing.completions.load(std::memory_order_acquire) == 1U);
    assert(missing.submit_error == lux::async::ESubmitError::UNKNOWN_OPERATION);

    auto stopped_endpoint = std::make_shared<Endpoint<Notify>>(EMode::VALUE);
    lux::async::OperationPort<Notify> stopped_port{stopped_endpoint};
    stdexec::inplace_stop_source stopped_stop;
    Outcome<int> stopped;
    auto stopped_state = stdexec::connect(
        lux::process::portSender(stopped_port, Notify{}),
        Receiver<void>{&stopped, stopped_stop.get_token()}
    );
    static_cast<void>(stopped_stop.request_stop());
    stdexec::start(stopped_state);
    assert(stopped.completions.load(std::memory_order_acquire) == 1U);
    assert(stopped.terminal == ETerminal::STOPPED);
    assert(stopped_endpoint->submits == 0U);
    return 0;
}
