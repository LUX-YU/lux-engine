#include <lux/engine/core/execution/PortSender.hpp>
#include <atomic>
#include <barrier>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stop_token>
#include <thread>

namespace
{
    void check(bool valid, const char* description)
    {
        if (!valid)
        {
            std::fprintf(stderr, "PortSender lifetime: %s\n", description);
            std::abort();
        }
    }

    struct Operation final { using Value = int; using Error = int; };
    using Failure = lux::async::TOperationFailure<int>;
    using Outcome = lux::async::OperationOutcome<Operation>;
    using Port = lux::async::TOperationPort<Operation>;
    enum class EMode { VALUE, ERROR, REJECT_CALLBACK, REJECT_EMPTY, DEFERRED, OVERLAP };

    struct Observation final
    {
        void* state = nullptr;
        void (*release)(void*) noexcept = nullptr;
        int completed = 0;
        int result = 0;
        std::stop_token stop;
    };

    struct Receiver final
    {
        using receiver_concept = stdexec::receiver_t;
        struct Env final
        {
            std::stop_token token;
            std::stop_token query(stdexec::get_stop_token_t) const noexcept { return token; }
        };
        Observation* observation;
        Env get_env() const noexcept { return {observation->stop}; }
        void finish(int result) noexcept
        {
            auto* record = observation;
            auto* state = record->state;
            auto release = record->release;
            record->result = result;
            ++record->completed;
            record->state = nullptr;
            release(state); // This also destroys this Receiver. No subsequent member access is permitted.
        }
        void set_value(int value) && noexcept { finish(value); }
        void set_error(Failure error) && noexcept { finish(error.isRuntime() ? -1 : error.domainError()); }
        void set_stopped() && noexcept { finish(-2); }
    };

    struct Endpoint final : Port::Endpoint
    {
        EMode mode;
        Observation* observation;
        std::barrier<> overlap{2};
        std::thread worker;
        void* pending = nullptr;
        void (*completion)(void*, Outcome&&) noexcept = nullptr;
        unsigned submissions = 0;
        Endpoint(EMode value, Observation& record) : mode(value), observation(&record) {}
        ~Endpoint() override { if (worker.joinable()) worker.join(); }

        lux::async::SubmitResult submit(Operation, void* state,
            void (*complete)(void*, Outcome&&) noexcept, lux::async::SubmitOptions) noexcept override
        {
            ++submissions;
            switch (mode)
            {
            case EMode::VALUE: complete(state, Outcome{31}); return {};
            case EMode::ERROR: complete(state, lux::cxx::unexpected(Failure::domain(17))); return {};
            case EMode::REJECT_CALLBACK:
                complete(state, lux::cxx::unexpected(Failure::runtime(lux::async::ESubmitError::QUEUE_FULL)));
                return lux::cxx::unexpected(lux::async::ESubmitError::QUEUE_FULL);
            case EMode::REJECT_EMPTY:
                return lux::cxx::unexpected(lux::async::ESubmitError::QUEUE_FULL);
            case EMode::DEFERRED:
                pending = state;
                completion = complete;
                return {};
            case EMode::OVERLAP:
                worker = std::thread([this, state, complete] {
                    complete(state, Outcome{31});
                    overlap.arrive_and_wait();
                });
                overlap.arrive_and_wait();
                check(observation->completed == 0, "receiver ran before submit returned");
                return {};
            }
            std::terminate();
        }
    };

    void start(Port port, Observation& record)
    {
        auto sender = lux::execution::portSender(std::move(port), Operation{});
        using State = decltype(stdexec::connect(std::move(sender), Receiver{&record}));
        auto* state = new State(stdexec::connect(std::move(sender), Receiver{&record}));
        record.state = state;
        record.release = [](void* value) noexcept { delete static_cast<State*>(value); };
        stdexec::start(*state);
    }

    void run(EMode mode, int expected)
    {
        Observation record;
        auto endpoint = std::make_shared<Endpoint>(mode, record);
        start(Port{endpoint}, record);
        if (mode == EMode::DEFERRED)
        {
            check(record.state && record.completed == 0, "deferred request completed early");
            std::thread worker([&] { endpoint->completion(endpoint->pending, Outcome{31}); });
            worker.join();
        }
        if (endpoint->worker.joinable()) endpoint->worker.join();
        check(!record.state && record.completed == 1 && record.result == expected, "wrong or duplicate terminal");
    }
}

int main()
{
    run(EMode::VALUE, 31);
    run(EMode::ERROR, 17);
    run(EMode::REJECT_CALLBACK, -1);
    run(EMode::REJECT_EMPTY, -1);
    run(EMode::DEFERRED, 31);
    for (unsigned iteration = 0; iteration != 128; ++iteration) run(EMode::OVERLAP, 31);
    Observation empty;
    start({}, empty);
    check(empty.completed == 1 && empty.result == -1 && !empty.state, "empty port terminal");
    Observation stopped;
    std::stop_source stop;
    stop.request_stop();
    stopped.stop = stop.get_token();
    auto endpoint = std::make_shared<Endpoint>(EMode::VALUE, stopped);
    start(Port{endpoint}, stopped);
    check(stopped.completed == 1 && stopped.result == -2 && !stopped.state && endpoint->submissions == 0,
        "pre-stopped sender submitted or failed to settle");
    std::puts("PortSender: sync value/error, rejection, empty, stopped, async and 128 controlled overlaps passed");
}
