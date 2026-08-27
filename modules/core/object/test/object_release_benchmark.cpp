#include "ObjectBenchmarkBaseline.hpp"
#include "ObjectTestSignals.hpp"
#include "pinclude/ListenerLayoutBenchmark.hpp"
#include "pinclude/ObjectDiagnostics.hpp"
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/object/ObjectReflection.hpp>
#include <lux/engine/object/detail/ObjectState.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>
#include <numeric>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace
{
#define LUX_CHECK(condition)                                                                                           \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
            std::abort();                                                                                              \
    } while (false)

    using Clock = std::chrono::steady_clock;
    using lux::object::test::fixture::IntReceiver;
    using lux::object::test::fixture::IntSender;

    constexpr std::size_t kWarmupCount = 5;
    constexpr std::size_t kSampleCount = 30;
    constexpr std::size_t kProductionConnectionBytes = sizeof(lux::object::detail::ConnectionControl);
    std::atomic_size_t allocations{0};

    struct Sample final
    {
        std::uint64_t elapsed_ns{0};
        std::size_t allocations{0};
        std::size_t inline_messages{0};
        std::size_t heap_messages{0};
    };

    template <class Callable>
    std::vector<Sample> sample(std::string_view name, std::size_t listeners, Callable&& callable)
    {
        for (std::size_t index = 0; index < kWarmupCount; ++index)
            callable();

        std::vector<Sample> samples;
        samples.reserve(kSampleCount);
        for (std::size_t index = 0; index < kSampleCount; ++index)
        {
            const auto allocations_before = allocations.load(std::memory_order_relaxed);
            const auto inline_before = lux::object::detail::ObjectDiagnosticsAccess::inlineMessageStorageCount();
            const auto heap_before = lux::object::detail::ObjectDiagnosticsAccess::heapMessageStorageCount();
            const auto begin = Clock::now();
            callable();
            const auto elapsed = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count()
            );
            const auto allocation_delta = allocations.load(std::memory_order_relaxed) - allocations_before;
            const auto inline_delta =
                lux::object::detail::ObjectDiagnosticsAccess::inlineMessageStorageCount() - inline_before;
            const auto heap_delta =
                lux::object::detail::ObjectDiagnosticsAccess::heapMessageStorageCount() - heap_before;
            samples.push_back({elapsed, allocation_delta, inline_delta, heap_delta});
            std::cout << "raw," << name << ',' << listeners << ',' << index << ',' << elapsed << ',' << allocation_delta
                      << ',' << inline_delta << ',' << heap_delta << '\n';
        }
        return samples;
    }

    void summarize(
        std::string_view name,
        std::size_t listeners,
        std::vector<Sample> samples,
        std::size_t bytes_per_connection
    )
    {
        std::ranges::sort(samples, {}, [](const Sample& value) { return value.elapsed_ns; });
        const auto median = (samples[kSampleCount / 2 - 1].elapsed_ns + samples[kSampleCount / 2].elapsed_ns) / 2;
        constexpr std::size_t p95_index = (kSampleCount * 95 + 99) / 100 - 1;
        const auto allocation_total =
            std::accumulate(samples.begin(), samples.end(), std::size_t{0}, [](std::size_t total, const Sample& value) {
                return total + value.allocations;
            }
            );
        const auto inline_total =
            std::accumulate(samples.begin(), samples.end(), std::size_t{0}, [](std::size_t total, const Sample& value) {
                return total + value.inline_messages;
            }
            );
        const auto heap_total =
            std::accumulate(samples.begin(), samples.end(), std::size_t{0}, [](std::size_t total, const Sample& value) {
                return total + value.heap_messages;
            }
            );
        std::cout << "summary," << name << ',' << listeners << ',' << median << ',' << samples[p95_index].elapsed_ns
                  << ',' << allocation_total << ',' << bytes_per_connection << ',' << inline_total << ',' << heap_total
                  << '\n';
    }

    template <class Callable>
    void benchmark(std::string_view name, std::size_t listeners, std::size_t bytes_per_connection, Callable&& callable)
    {
        summarize(name, listeners, sample(name, listeners, std::forward<Callable>(callable)), bytes_per_connection);
    }

    void benchmarkDispatchBaselines()
    {
        using namespace lux::object::test::benchmark;
        constexpr std::size_t iterations = 200'000;
        auto* receiver = createBaselineReceiver();
        auto* polymorphic = receiver;
        const auto function_pointer = baselineFunction();
        benchmark("noinline_member", 1, 0, [&] {
            for (std::size_t index = 0; index < iterations; ++index)
                receiver->member(1);
        });
        benchmark("virtual_member", 1, 0, [&] {
            for (std::size_t index = 0; index < iterations; ++index)
                polymorphic->virtualMember(1);
        });
        benchmark("function_pointer", 1, 0, [&] {
            for (std::size_t index = 0; index < iterations; ++index)
                function_pointer(*receiver, 1);
        });
        LUX_CHECK(receiver->observed() != 0);
        destroyBaselineReceiver(receiver);
    }

    void benchmarkTypedMember(std::size_t listener_count)
    {
        IntSender sender;
        IntReceiver receiver;
        std::vector<lux::object::Connection> connections;
        connections.reserve(listener_count);
        for (std::size_t index = 0; index < listener_count; ++index)
        {
            auto connection =
                sender.observe<IntSender::changed, &IntReceiver::receive, lux::object::EDelivery::DIRECT>(receiver);
            LUX_CHECK(connection);
            connections.push_back(std::move(*connection));
        }
        constexpr std::size_t iterations = 20'000;
        const auto storage_growth_before = lux::object::detail::ObjectDiagnosticsAccess::storageGrowthCount(sender);
        benchmark("object_typed_member", listener_count, kProductionConnectionBytes, [&] {
            for (std::size_t index = 0; index < iterations; ++index)
                sender.publish(1);
        }
        );
        LUX_CHECK(lux::object::detail::ObjectDiagnosticsAccess::storageGrowthCount(sender) == storage_growth_before);
        LUX_CHECK(receiver.observed == listener_count * iterations * (kWarmupCount + kSampleCount));
    }

    void benchmarkScoped(std::size_t listener_count)
    {
        IntSender sender;
        std::uint64_t observed = 0;
        std::vector<lux::object::ScopedConnection> connections;
        connections.reserve(listener_count);
        for (std::size_t index = 0; index < listener_count; ++index)
        {
            auto connection =
                sender.observeScoped<IntSender::changed>([&observed](const int& value) noexcept { observed += value; });
            connections.push_back(std::move(connection));
        }
        constexpr std::size_t iterations = 20'000;
        const auto storage_growth_before = lux::object::detail::ObjectDiagnosticsAccess::storageGrowthCount(sender);
        benchmark("object_scoped", listener_count, kProductionConnectionBytes, [&] {
            for (std::size_t index = 0; index < iterations; ++index)
                sender.publish(1);
        });
        LUX_CHECK(lux::object::detail::ObjectDiagnosticsAccess::storageGrowthCount(sender) == storage_growth_before);
        LUX_CHECK(observed == listener_count * iterations * (kWarmupCount + kSampleCount));
    }

    void benchmarkQueued(std::size_t listener_count)
    {
        lux::object::ObjectMessageQueue queue;
        lux::object::ObjectMessageQueue sender_queue;
        IntSender sender{sender_queue.dispatcherRef()};
        class Receiver final : public lux::object::Object<Receiver>
        {
        public:
            explicit Receiver(lux::object::ObjectDispatcherRef dispatcher) : Object(std::move(dispatcher))
            {
            }
            void receive(const int& value) noexcept
            {
                observed += value;
            }
            std::uint64_t observed{0};
        } receiver{queue.dispatcherRef()};
        std::vector<lux::object::Connection> connections;
        connections.reserve(listener_count);
        for (std::size_t index = 0; index < listener_count; ++index)
        {
            auto connection =
                sender.observe<IntSender::changed, &Receiver::receive, lux::object::EDelivery::QUEUED>(receiver);
            LUX_CHECK(connection);
            connections.push_back(std::move(*connection));
        }
        constexpr std::size_t iterations = 2'000;
        benchmark("object_queued", listener_count, kProductionConnectionBytes, [&] {
            for (std::size_t index = 0; index < iterations; ++index)
                sender.publish(1);
            LUX_CHECK(queue.dispatchPending() == listener_count * iterations);
        }
        );
        LUX_CHECK(receiver.observed == listener_count * iterations * (kWarmupCount + kSampleCount));
    }

    template <auto& Signal, auto Method, class Payload>
    void benchmarkQueuedPayload(std::string_view name, std::size_t listener_count)
    {
        using lux::object::test::fixture::QueuedPayloadReceiver;
        using lux::object::test::fixture::QueuedPayloadSender;

        lux::object::ObjectMessageQueue sender_queue;
        lux::object::ObjectMessageQueue receiver_queue;
        QueuedPayloadSender sender{sender_queue.dispatcherRef()};
        QueuedPayloadReceiver receiver{receiver_queue.dispatcherRef()};
        std::vector<lux::object::Connection> connections;
        connections.reserve(listener_count);
        for (std::size_t index = 0; index < listener_count; ++index)
        {
            auto connection = sender.observe<Signal, Method, lux::object::EDelivery::QUEUED>(receiver);
            LUX_CHECK(connection);
            connections.push_back(std::move(*connection));
        }

        constexpr std::size_t iterations = 250;
        const Payload payload{};
        benchmark(name, listener_count, kProductionConnectionBytes, [&] {
            for (std::size_t index = 0; index < iterations; ++index)
                sender.publish(payload);
            LUX_CHECK(receiver_queue.dispatchPending() == listener_count * iterations);
        }
        );
        LUX_CHECK(receiver.observed == listener_count * iterations * (kWarmupCount + kSampleCount));
    }

    struct PostContentionEvent final
    {
        std::array<std::byte, 64> bytes{};
    };

    class PostContentionReceiver final : public lux::object::Object<PostContentionReceiver>
    {
    public:
        explicit PostContentionReceiver(lux::object::ObjectDispatcherRef dispatcher) noexcept
            : Object(std::move(dispatcher))
        {
        }

        void event(lux::object::EventView& view) noexcept override
        {
            if (view.getIf<PostContentionEvent>())
            {
                ++observed;
                view.accept();
            }
        }

        std::uint64_t observed{0};
    };

    void benchmarkPostContention(std::size_t producer_count)
    {
        constexpr std::size_t posts_per_producer = 250;
        lux::object::ObjectMessageQueue queue;
        PostContentionReceiver receiver{queue.dispatcherRef()};
        const auto target = receiver.weakRef();
        std::barrier start{static_cast<std::ptrdiff_t>(producer_count + 1)};
        std::barrier finish{static_cast<std::ptrdiff_t>(producer_count + 1)};
        std::atomic_bool stopping{false};
        std::vector<std::thread> producers;
        producers.reserve(producer_count);
        for (std::size_t producer = 0; producer < producer_count; ++producer)
        {
            producers.emplace_back([&] {
                for (;;)
                {
                    start.arrive_and_wait();
                    if (stopping.load(std::memory_order_acquire))
                        return;
                    for (std::size_t index = 0; index < posts_per_producer; ++index)
                    {
                        LUX_CHECK(
                            lux::object::postEvent(target, PostContentionEvent{}) ==
                            lux::object::EEventPostStatus::POSTED
                        );
                    }
                    finish.arrive_and_wait();
                }
            }
            );
        }

        benchmark("dispatcher_post_contention", producer_count, 0, [&] {
            start.arrive_and_wait();
            finish.arrive_and_wait();
            LUX_CHECK(queue.dispatchPending() == producer_count * posts_per_producer);
        }
        );
        stopping.store(true, std::memory_order_release);
        start.arrive_and_wait();
        for (auto& producer : producers)
            producer.join();
        LUX_CHECK(receiver.observed == producer_count * posts_per_producer * (kWarmupCount + kSampleCount));
    }

    void benchmarkChurn()
    {
        IntSender sender;
        constexpr std::size_t iterations = 5'000;
        benchmark("object_churn", iterations, kProductionConnectionBytes, [&] {
            for (std::size_t index = 0; index < iterations; ++index)
            {
                auto connection = sender.observeScoped<IntSender::changed>([](const int&) noexcept {});
                static_cast<void>(connection);
            }
        });
    }

    void benchmarkReentrancy()
    {
        IntSender sender;
        std::uint64_t observed = 0;
        auto nested = sender.observeScoped<IntSender::changed>([&](const int& value) noexcept {
            observed += static_cast<std::uint64_t>(value);
            if (value == 1)
                sender.publish(2);
        });
        constexpr std::size_t iterations = 10'000;
        const auto storage_growth_before = lux::object::detail::ObjectDiagnosticsAccess::storageGrowthCount(sender);
        benchmark("object_reentrant", 1, kProductionConnectionBytes, [&] {
            for (std::size_t index = 0; index < iterations; ++index)
                sender.publish(1);
        }
        );
        LUX_CHECK(lux::object::detail::ObjectDiagnosticsAccess::storageGrowthCount(sender) == storage_growth_before);
        LUX_CHECK(observed == 3 * iterations * (kWarmupCount + kSampleCount));
    }

    void benchmarkDynamicReflection()
    {
        lux::meta::ReflectionRegistry::initRegistry();
        auto& registry = lux::meta::ReflectionRegistry::instance();
        const auto* sender_class = registry.findClass("lux::object::test::fixture::IntSender");
        const auto* receiver_class = registry.findClass("lux::object::test::fixture::IntReceiver");
        LUX_CHECK(sender_class && receiver_class);
        const auto signal = lux::object::reflection::findSignal(registry, *sender_class, "changed");
        LUX_CHECK(signal);
        const lux::meta::RefMethod* method = nullptr;
        for (const auto& candidate : receiver_class->methods)
        {
            if (candidate.invokable.name == "receive")
                method = std::addressof(candidate);
        }
        LUX_CHECK(method);

        IntSender sender;
        IntReceiver receiver;
        auto connection =
            lux::object::reflection::observe(sender, signal, receiver, *method, lux::object::EDelivery::DIRECT);
        LUX_CHECK(connection);
        constexpr std::size_t iterations = 20'000;
        const auto storage_growth_before = lux::object::detail::ObjectDiagnosticsAccess::storageGrowthCount(sender);
        benchmark("object_dynamic_reflected", 1, kProductionConnectionBytes, [&] {
            for (std::size_t index = 0; index < iterations; ++index)
                sender.publish(1);
        });
        LUX_CHECK(lux::object::detail::ObjectDiagnosticsAccess::storageGrowthCount(sender) == storage_growth_before);
        LUX_CHECK(receiver.observed == iterations * (kWarmupCount + kSampleCount));
        connection->disconnect();
        lux::meta::ReflectionRegistry::destroyRegistry();
    }

    template <class Candidate> void benchmarkLayout(std::string_view name, std::size_t listener_count)
    {
        using namespace lux::object::test::benchmark;
        std::uint64_t observed = 0;
        std::vector<ListenerControl> controls(listener_count);
        Candidate candidate;
        for (auto& control : controls)
        {
            control.observed = &observed;
            control.invoke = &accumulate;
            candidate.append(control);
        }
        constexpr std::size_t iterations = 20'000;
        constexpr auto bytes_per_connection =
            std::is_same_v<Candidate, CandidateA> ? sizeof(ListenerControl*) : sizeof(DirectListener);
        benchmark(name, listener_count, bytes_per_connection, [&] {
            for (std::size_t index = 0; index < iterations; ++index)
                candidate.notify(1);
        }
        );
        LUX_CHECK(observed == listener_count * iterations * (kWarmupCount + kSampleCount));
    }

    template <class Candidate> void benchmarkLayoutChurn(std::string_view name)
    {
        using namespace lux::object::test::benchmark;
        std::uint64_t observed = 0;
        ListenerControl control{&observed, &accumulate, true};
        Candidate candidate;
        constexpr std::size_t iterations = 5'000;
        benchmark(name, iterations, sizeof(Candidate), [&] {
            for (std::size_t index = 0; index < iterations; ++index)
            {
                candidate.append(control);
                candidate.pop();
            }
        });
    }
} // namespace

void*
operator new(std::size_t size)
{
    allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(size))
        return memory;
    throw std::bad_alloc{};
}

void*
operator new[](std::size_t size)
{
    return ::operator new(size);
}
void*
operator new(std::size_t size, std::align_val_t alignment)
{
    allocations.fetch_add(1, std::memory_order_relaxed);
#if defined(_MSC_VER)
    if (void* memory = _aligned_malloc(size, static_cast<std::size_t>(alignment)))
        return memory;
#else
    const auto value = static_cast<std::size_t>(alignment);
    const auto rounded = (size + value - 1) / value * value;
    if (void* memory = std::aligned_alloc(value, rounded))
        return memory;
#endif
    throw std::bad_alloc{};
}
void*
operator new[](std::size_t size, std::align_val_t alignment)
{
    return ::operator new(size, alignment);
}
void
operator delete(void* memory) noexcept
{
    std::free(memory);
}
void
operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}
void
operator delete[](void* memory) noexcept
{
    std::free(memory);
}
void
operator delete[](void* memory, std::size_t) noexcept
{
    std::free(memory);
}
void
operator delete(void* memory, std::align_val_t) noexcept
{
#if defined(_MSC_VER)
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}
void
operator delete(void* memory, std::size_t, std::align_val_t alignment) noexcept
{
    ::operator delete(memory, alignment);
}
void
operator delete[](void* memory, std::align_val_t alignment) noexcept
{
    ::operator delete(memory, alignment);
}
void
operator delete[](void* memory, std::size_t, std::align_val_t alignment) noexcept
{
    ::operator delete(memory, alignment);
}

int
main()
{
    lux::object::detail::ObjectDiagnosticsAccess::resetMessageStorage();
    const auto before_construction = allocations.load(std::memory_order_relaxed);
    IntSender sender;
    LUX_CHECK(allocations.load(std::memory_order_relaxed) == before_construction);
    sender.publish(1);
    LUX_CHECK(allocations.load(std::memory_order_relaxed) == before_construction);

    std::cout << "meta,warmups," << kWarmupCount << '\n';
    std::cout << "meta,samples," << kSampleCount << '\n';
    std::cout << "meta,connection_handle_bytes," << sizeof(lux::object::Connection) << '\n';
    std::cout << "meta,production_connection_control_bytes," << kProductionConnectionBytes << '\n';
    std::cout << "meta,candidate_b_extra_bytes_per_connection,"
              << lux::object::test::benchmark::kCandidateBExtraBytesPerConnection << '\n';
    std::cout << "record,case,listeners_or_count,sample_or_median,elapsed_or_p95,"
                 "allocations,bytes_per_connection,inline_messages,heap_messages\n";

    benchmarkDispatchBaselines();
    for (const auto count : std::array<std::size_t, 5>{0, 1, 4, 16, 64})
    {
        benchmarkTypedMember(count);
        benchmarkScoped(count);
        benchmarkLayout<lux::object::test::benchmark::CandidateA>("listener_layout_a", count);
        benchmarkLayout<lux::object::test::benchmark::CandidateB>("listener_layout_b", count);
    }
    benchmarkDynamicReflection();
    benchmarkQueued(1);
    benchmarkQueued(4);
    using namespace lux::object::test::fixture;
    for (const auto listeners : std::array<std::size_t, 3>{1, 4, 16})
    {
        benchmarkQueuedPayload<
            QueuedPayloadSender::payload4,
            static_cast<void (QueuedPayloadReceiver::*)(const QueuedPayload4&) noexcept>(
                &QueuedPayloadReceiver::receive),
            QueuedPayload4>("object_queued_payload_4", listeners);
        benchmarkQueuedPayload<
            QueuedPayloadSender::payload64,
            static_cast<void (QueuedPayloadReceiver::*)(const QueuedPayload64&) noexcept>(
                &QueuedPayloadReceiver::receive),
            QueuedPayload64>("object_queued_payload_64", listeners);
        benchmarkQueuedPayload<
            QueuedPayloadSender::payload256,
            static_cast<void (QueuedPayloadReceiver::*)(const QueuedPayload256&) noexcept>(
                &QueuedPayloadReceiver::receive),
            QueuedPayload256>("object_queued_payload_256", listeners);
        benchmarkQueuedPayload<
            QueuedPayloadSender::payload1024,
            static_cast<void (QueuedPayloadReceiver::*)(const QueuedPayload1024&) noexcept>(
                &QueuedPayloadReceiver::receive),
            QueuedPayload1024>("object_queued_payload_1024", listeners);
    }
    benchmarkPostContention(1);
    benchmarkPostContention(4);
    benchmarkChurn();
    benchmarkReentrancy();
    benchmarkLayoutChurn<lux::object::test::benchmark::CandidateA>("listener_layout_a_churn");
    benchmarkLayoutChurn<lux::object::test::benchmark::CandidateB>("listener_layout_b_churn");
}
