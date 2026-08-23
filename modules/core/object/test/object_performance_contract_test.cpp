#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include <lux/engine/object/ObjectModel.hpp>
#include "ObjectTestSignals.hpp"

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

    using lux::object::test::fixture::IntSender;
    using lux::object::test::fixture::IntReceiver;

    class Receiver final : public lux::object::Object<Receiver>
    {
    public:
        explicit Receiver(lux::object::ObjectDispatcherRef dispatcher)
            : Object(std::move(dispatcher))
        {
        }

        void receive(const int& value) noexcept { observed += value; }

        std::uint64_t observed{0};
    };

    using Clock = std::chrono::steady_clock;

#if defined(_MSC_VER)
#define LUX_OBJECT_NOINLINE __declspec(noinline)
#else
#define LUX_OBJECT_NOINLINE __attribute__((noinline))
#endif

    struct BaselineReceiver
    {
        LUX_OBJECT_NOINLINE void member(int value) noexcept { observed += value; }
        virtual LUX_OBJECT_NOINLINE void virtualMember(int value) noexcept
        {
            observed += value;
        }
        virtual ~BaselineReceiver() = default;
        std::uint64_t observed{0};
    };

    LUX_OBJECT_NOINLINE void functionPointerCall(
        std::uint64_t* observed,
        int value
    ) noexcept
    {
        *observed += value;
    }

    void benchmarkDispatchBaselines()
    {
        constexpr std::size_t iterations = 200'000;
        BaselineReceiver receiver;
        BaselineReceiver* polymorphic = &receiver;
        void (*function_pointer)(std::uint64_t*, int) noexcept = &functionPointerCall;
        const auto measure = [&](std::string_view name, auto&& invoke)
        {
            const auto start = Clock::now();
            for (std::size_t index = 0; index < iterations; ++index)
                invoke();
            const auto elapsed = Clock::now() - start;
            std::cout << name << ",1,"
                      << std::chrono::duration_cast<std::chrono::nanoseconds>(
                             elapsed
                         ).count()
                      << ",na,0\n";
        };
        measure("noinline_member", [&] { receiver.member(1); });
        measure("virtual_member", [&] { polymorphic->virtualMember(1); });
        measure(
            "function_pointer",
            [&] { function_pointer(&receiver.observed, 1); }
        );
    }

    void benchmarkDirect(std::size_t listener_count)
    {
        IntSender sender;
        std::uint64_t observed = 0;
        std::vector<lux::object::ScopedConnection> connections;
        connections.reserve(listener_count);
        for (std::size_t index = 0; index < listener_count; ++index)
        {
            auto connection =
                sender.observeScoped<IntSender::changed>(
                    [&observed](const int& value) noexcept { observed += value; }
                );
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
        IntSender sender;
        Receiver receiver{queue.dispatcherRef()};
        std::vector<lux::object::Connection> connections;
        connections.reserve(listener_count);
        for (std::size_t index = 0; index < listener_count; ++index)
        {
            auto connection = sender.observe<
                IntSender::changed,
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

    void benchmarkTypedMember(std::size_t listener_count)
    {
        IntSender sender;
        IntReceiver receiver;
        std::vector<lux::object::Connection> connections;
        connections.reserve(listener_count);
        for (std::size_t index = 0; index < listener_count; ++index)
        {
            auto connection = sender.observe<
                IntSender::changed,
                &IntReceiver::receive,
                lux::object::EDelivery::DIRECT>(receiver);
            assert(connection);
            connections.push_back(std::move(*connection));
        }

        constexpr std::size_t iterations = 20'000;
        const auto start = Clock::now();
        for (std::size_t index = 0; index < iterations; ++index)
            sender.publish(1);
        const auto elapsed = Clock::now() - start;
        assert(receiver.observed == listener_count * iterations);
        std::cout << "object_typed_member," << listener_count << ','
                  << std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed)
                         .count()
                  << ",0," << sizeof(lux::object::Connection) << '\n';
    }

    void benchmarkDynamicReflection()
    {
        lux::meta::ReflectionRegistry::initRegistry();
        auto& registry = lux::meta::ReflectionRegistry::instance();
        const auto* sender_class =
            registry.findClass("lux::object::test::fixture::IntSender");
        const auto* receiver_class =
            registry.findClass("lux::object::test::fixture::IntReceiver");
        assert(sender_class && receiver_class);
        const auto signal = lux::object::reflection::findSignal(
            registry,
            *sender_class,
            "changed"
        );
        assert(signal);
        const lux::meta::RefMethod* method = nullptr;
        for (const auto& candidate : receiver_class->methods)
        {
            if (candidate.invokable.name == "receive")
                method = std::addressof(candidate);
        }
        assert(method);

        IntSender sender;
        IntReceiver receiver;
        auto connection = lux::object::reflection::observe(
            sender,
            signal,
            receiver,
            *method,
            lux::object::EDelivery::DIRECT
        );
        assert(connection);
        constexpr std::size_t iterations = 20'000;
        const auto start = Clock::now();
        for (std::size_t index = 0; index < iterations; ++index)
            sender.publish(1);
        const auto elapsed = Clock::now() - start;
        assert(receiver.observed == iterations);
        std::cout << "object_dynamic_reflected,1,"
                  << std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed)
                         .count()
                  << ",0," << sizeof(lux::object::Connection) << '\n';
        connection->disconnect();
        lux::meta::ReflectionRegistry::destroyRegistry();
    }

    void benchmarkChurn()
    {
        IntSender sender;
        constexpr std::size_t iterations = 5'000;
        const auto start = Clock::now();
        for (std::size_t index = 0; index < iterations; ++index)
        {
            auto connection = sender.observeScoped<IntSender::changed>(
                [](const int&) noexcept {}
            );
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
    IntSender sender;
    assert(allocations.load(std::memory_order_relaxed) == before_construction);
    sender.publish(1);
    assert(allocations.load(std::memory_order_relaxed) == before_construction);

    std::cout << "case,count,elapsed_ns,consumer_notify_allocations,"
                 "connection_handle_bytes\n";
    benchmarkDispatchBaselines();
    for (const auto count : std::array<std::size_t, 5>{0, 1, 4, 16, 64})
        benchmarkTypedMember(count);
    for (const auto count : std::array<std::size_t, 5>{0, 1, 4, 16, 64})
        benchmarkDirect(count);
    benchmarkDynamicReflection();
    benchmarkQueued(1);
    benchmarkQueued(4);
    benchmarkChurn();
}
