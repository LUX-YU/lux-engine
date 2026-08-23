#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include <lux/engine/object/ObjectModel.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>
#include <vector>

namespace
{
    std::atomic_size_t allocations{0};

    class Sender final : public lux::object::Object<Sender>
    {
    public:
        static const signal_type<int> changed;
        void publish(int value) { notify<changed>(value); }
    };

    const Sender::signal_type<int> Sender::changed{lux::object::SignalIndex{0}};

    class Receiver final : public lux::object::Object<Receiver>
    {
    public:
        explicit Receiver(lux::object::ObjectDispatcherRef dispatcher)
            : Object(std::move(dispatcher))
        {
        }

        void receive(const int& value) { observed += value; }

        std::uint64_t observed{0};
    };

    using Clock = std::chrono::steady_clock;

    void benchmarkDirect(std::size_t listener_count)
    {
        Sender sender;
        std::uint64_t observed = 0;
        std::vector<lux::object::ScopedConnection> connections;
        connections.reserve(listener_count);
        for (std::size_t index = 0; index < listener_count; ++index)
        {
            auto connection =
                sender.observeScoped<Sender::changed>([&observed](const int& value)
                                                      { observed += value; });
            assert(connection);
            connections.push_back(std::move(*connection));
        }

        constexpr std::size_t iterations = 20'000;
        const auto before_allocations = allocations.load(std::memory_order_relaxed);
        const auto start = Clock::now();
        for (std::size_t index = 0; index < iterations; ++index)
            sender.publish(1);
        const auto elapsed = Clock::now() - start;
        const auto notify_allocations =
            allocations.load(std::memory_order_relaxed) - before_allocations;

        assert(observed == listener_count * iterations);
        assert(notify_allocations == 0);
        std::cout
            << "object_direct," << listener_count << ','
            << std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()
            << ',' << notify_allocations << ',' << sizeof(lux::object::Connection)
            << '\n';
    }

    void benchmarkQueued(std::size_t listener_count)
    {
        lux::object::ObjectMessageQueue queue;
        Sender sender;
        Receiver receiver{queue.dispatcherRef()};
        std::vector<lux::object::Connection> connections;
        connections.reserve(listener_count);
        for (std::size_t index = 0; index < listener_count; ++index)
        {
            auto connection = sender.observe<
                Sender::changed,
                &Receiver::receive,
                lux::object::EDelivery::QUEUED>(receiver);
            assert(connection);
            connections.push_back(std::move(*connection));
        }

        constexpr std::size_t iterations = 2'000;
        const auto start = Clock::now();
        for (std::size_t index = 0; index < iterations; ++index)
            sender.publish(1);
        const auto dispatched = queue.dispatchPending();
        const auto elapsed = Clock::now() - start;
        assert(dispatched == listener_count * iterations);
        assert(receiver.observed == listener_count * iterations);
        std::cout
            << "object_queued," << listener_count << ','
            << std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()
            << ",na," << sizeof(lux::object::Connection) << '\n';
    }

    void benchmarkChurn()
    {
        Sender sender;
        constexpr std::size_t iterations = 5'000;
        const auto start = Clock::now();
        for (std::size_t index = 0; index < iterations; ++index)
        {
            auto connection = sender.observeScoped<Sender::changed>([](const int&) {});
            assert(connection);
        }
        const auto elapsed = Clock::now() - start;
        std::cout
            << "object_churn," << iterations << ','
            << std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()
            << ",na," << sizeof(lux::object::Connection) << '\n';
    }
} // namespace

void* operator new(std::size_t size)
{
    allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(size))
        return memory;
    throw std::bad_alloc{};
}

void operator delete(void* memory) noexcept { std::free(memory); }

void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

int main()
{
    const auto before_construction = allocations.load(std::memory_order_relaxed);
    Sender sender;
    assert(allocations.load(std::memory_order_relaxed) == before_construction);
    sender.publish(1);
    assert(allocations.load(std::memory_order_relaxed) == before_construction);

    std::cout << "case,count,elapsed_ns,consumer_notify_allocations,"
                 "connection_handle_bytes\n";
    for (const auto count : std::array<std::size_t, 5>{0, 1, 4, 16, 64})
        benchmarkDirect(count);
    benchmarkQueued(1);
    benchmarkQueued(4);
    benchmarkChurn();
}
