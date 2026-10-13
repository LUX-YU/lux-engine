#include "Support.hpp"
#include <algorithm>
#include <barrier>
#include <chrono>
#include <fstream>
#include <new>
#include <numeric>
#include <string>
#include <thread>
#include <malloc.h>

namespace allocation
{
    thread_local bool enabled = false;
    thread_local std::size_t count = 0;
    thread_local std::size_t bytes = 0;

    void record(std::size_t size) noexcept
    {
        if (!enabled) return;
        ++count;
        bytes += size;
    }
}

void* operator new(std::size_t size)
{
    allocation::record(size);
    auto* result = std::malloc((std::max)(size, std::size_t{1}));
    if (!result) std::abort();
    return result;
}

void* operator new[](std::size_t size) { return ::operator new(size); }

void operator delete(void* pointer) noexcept { std::free(pointer); }

void operator delete[](void* pointer) noexcept { std::free(pointer); }

void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }

void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

void* operator new(std::size_t size, std::align_val_t alignment)
{
    allocation::record(size);
    auto* result = _aligned_malloc((std::max)(size, std::size_t{1}), static_cast<std::size_t>(alignment));
    if (!result) std::abort();
    return result;
}

void* operator new[](std::size_t size, std::align_val_t alignment) { return ::operator new(size, alignment); }

void operator delete(void* pointer, std::align_val_t) noexcept { _aligned_free(pointer); }

void operator delete[](void* pointer, std::align_val_t) noexcept { _aligned_free(pointer); }

void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept { _aligned_free(pointer); }

void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept { _aligned_free(pointer); }

namespace connection_test
{
    using namespace lux::render::connection;
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;

    auto valueDescriptor() { return operationDescriptor<Value, Value>("test.increment"); }

    auto bulkDescriptor() { return operationDescriptor<Payload, Value>("test.upload"); }

    auto voidDescriptor() { return operationDescriptor<Value>("test.commit"); }

    std::shared_ptr<const OperationDirectory> directory()
    {
        auto result = std::make_shared<OperationDirectory>();
        for (auto descriptor : {valueDescriptor(), bulkDescriptor(), voidDescriptor()})
        {
            auto id = result->emplace(OperationDefinition{{}, descriptor});
            result->find(id)->id = id;
        }
        return result;
    }

    struct Fixture final
    {
        std::shared_ptr<const OperationDirectory> operations = directory();
        std::unique_ptr<LocalListener> listener;
        std::unique_ptr<ClientConnectionEndpoint> a;
        std::unique_ptr<ClientConnectionEndpoint> b;

        explicit Fixture(ConnectionConfig config = {8, 8, 1024 * 1024 * 8, 256, 4})
        {
            listener = must(LocalListener::listen("test.main", operations, config));
            a = must(ClientConnectionEndpoint::connect("test.main"));
            b = must(ClientConnectionEndpoint::connect("test.main"));
            check(listener->accept(8) == 2, "two accepted");
        }

        std::vector<ServerWork> receive()
        {
            std::vector<ServerWork> work;
            for (auto& endpoint : listener->endpoints())
            {
                while (auto item = endpoint->receive()) work.push_back(std::move(*item));
            }
            return work;
        }
    };

    ReplyMessage terminal(ClientConnectionEndpoint& endpoint)
    {
        auto result = must(endpoint.poll());
        check(result.has_value(), "terminal present");
        return std::move(*result);
    }

    std::uint64_t valueOf(const ReplyMessage& reply)
    {
        check(reply.header.outcome == EOperationOutcome::SUCCESS, "successful reply");
        return must(decodeWire<Value>({DataSchema<Value>::describe(), reply.result.bytes, {}}, {8, 0})).value;
    }

    void contracts()
    {
        check(!ClientConnectionEndpoint::connect("test.missing"), "missing address");
        auto operations = directory();
        auto listener = must(LocalListener::listen("test.main", operations, {2, 4, 128, 256, 1}));
        check(!LocalListener::listen("test.main", operations), "duplicate listener");
        std::barrier start(3);
        std::unique_ptr<ClientConnectionEndpoint> a, b;
        std::thread first([&] { start.arrive_and_wait(); a = must(ClientConnectionEndpoint::connect("test.main")); });
        std::thread second([&] { start.arrive_and_wait(); b = must(ClientConnectionEndpoint::connect("test.main")); });
        start.arrive_and_wait();
        first.join();
        second.join();
        a->adoptCurrentThread();
        b->adoptCurrentThread();
        check(!a->ready() && !b->ready(), "pending before accept");
        check(listener->accept(1) == 1 && listener->accept(1) == 1, "bounded accept");
        auto op = must(a->bind(valueDescriptor()));
        auto other = must(b->bind(valueDescriptor()));
        check(op.id() == other.id(), "one operation authority");
        auto mismatch = valueDescriptor();
        ++mismatch.version.minor;
        check(!a->bind(mismatch), "exact operation version");
        mismatch = valueDescriptor();
        mismatch.reply->alignment = 4;
        check(!a->bind(mismatch), "reply layout mismatch");
        auto aid = must(a->submit(op, Value{41}));
        auto bid = must(b->submit(other, Value{91}));
        check(aid == bid, "numeric request ids intentionally collide");
        std::vector<ServerWork> deferred;
        for (auto& endpoint : listener->endpoints()) deferred.push_back(std::move(*endpoint->receive()));
        std::reverse(deferred.begin(), deferred.end());
        for (auto& work : deferred) must(work.succeed(Value{must(work.decode<Value>()).value + 1}));
        check(valueOf(terminal(*a)) == 42 && valueOf(terminal(*b)) == 92, "origin survives deferred reordering");
        check(!deferred.front().succeed(Value{0}), "double completion rejected");
        auto next = must(a->submit(op, Value{1}));
        check(next != aid && !a->abandon(aid), "generation invalidates old id");
        must(a->abandon(next));
        check(!a->abandon(next), "duplicate abandon rejected");
        must(a->submit(op, Value{2}));
        Packet held{must(encodeWire(Value{3}, {}, {8, 0})), {}};
        auto* pointer = held.bytes.data();
        check(!a->submit(op, held) && held.bytes.data() == pointer, "request queue rollback retains input");
        for (auto& endpoint : listener->endpoints())
        {
            while (auto work = endpoint->receive()) must(work->succeed(Value{5}));
        }
        check(valueOf(terminal(*a)) == 5 && a->pendingCount() == 0, "abandoned credit consumed not prematurely freed");
        auto early = must(ClientConnectionEndpoint::connect("test.main"));
        early.reset();
        check(listener->accept(8) == 1 && listener->endpoints().size() == 2, "close before accept");
        must(a->submit(op, Value{7}));
        listener->stop();
        check(terminal(*a).header.outcome == EOperationOutcome::OUTCOME_UNKNOWN, "lost outcome not cancelled");
        listener.reset();
        auto replacement = must(LocalListener::listen("test.main", operations));
        auto fresh = must(ClientConnectionEndpoint::connect("test.main"));
        replacement->accept(1);
        check(!fresh->submit(op, Value{9}), "old server binding rejected after address reuse");
        check(!a->submit(op, Value{9}), "old connection remains closed");
        auto missing = must(ClientConnectionEndpoint::connect("test.main"));
        replacement->stop();
        check(!missing->ready(), "unaccepted connection closed on stop");
        std::puts("contracts: concurrent connect, origin, generation, schema, rollback, address reuse passed");
    }

    struct Counted final
    {
        explicit Counted(std::atomic<int>& counter, std::size_t size) : destroyed(counter), bytes(size) {}
        ~Counted() { destroyed.fetch_add(1); }
        std::atomic<int>& destroyed;
        std::vector<std::byte> bytes;
    };

    void ownership()
    {
        Fixture fixture({2, 2, 2 * 1024 * 1024, 256, 1});
        auto op = must(fixture.a->bind(bulkDescriptor()));
        auto value_op = must(fixture.b->bind(valueDescriptor()));
        std::atomic<int> destroyed{0};
        auto source = std::make_shared<Counted>(destroyed, 1024 * 1024);
        source->bytes[0] = std::byte{123};
        const auto* address = source->bytes.data();
        auto owned = OwnedWireBytes::fromOwner(source, std::span<const std::byte>(source->bytes));
        std::array attachments{owned};
        auto id = must(fixture.a->submit(op, Payload{{0, 0, owned.size()}}, attachments));
        check(
            !fixture.a->submit(op, Payload{{0, 0, owned.size()}}, attachments),
            "byte budget independent of ring space"
        );
        must(fixture.b->submit(value_op, Value{12}));
        auto work = fixture.receive();
        std::optional<ServerWork> bulk;
        for (auto& item : work)
        {
            if (item.header().operation == op.id()) bulk.emplace(std::move(item));
            else must(item.succeed(Value{13}));
        }
        check(valueOf(terminal(*fixture.b)) == 13, "B unaffected by A byte budget");
        source.reset();
        owned = {};
        attachments = {};
        auto decoded = must(bulk->decode<Payload>());
        auto resolved = must(resolveExternal(bulk->payload().attachmentView(), decoded.external, 2 * 1024 * 1024));
        check(
            resolved.data() == address && resolved.data()[0] == std::byte{123}, "bulk pointer identity; no data copy"
        );
        resolved = {};
        must(fixture.a->abandon(id));
        check(destroyed == 0 && fixture.a->pendingCount() == 1, "abandon retains active input and credit");
        must(bulk->succeed(Value{77}));
        check(destroyed == 0, "reply retains retired input until client consumption");
        check(!must(fixture.a->poll()) && destroyed == 1, "abandoned reply drains and owner destroyed exactly once");
        auto void_op = must(fixture.b->bind(voidDescriptor()));
        must(fixture.b->submit(void_op, Value{1}));
        auto void_work = fixture.receive();
        must(void_work.front().succeed());
        check(terminal(*fixture.b).header.outcome == EOperationOutcome::SUCCESS, "void terminal");
        must(fixture.b->submit(value_op, Value{1}));
        { auto dropped = fixture.receive(); }
        check(terminal(*fixture.b).header.outcome == EOperationOutcome::CANCELLED, "dropped work terminal");
        must(fixture.b->submit(value_op, Value{1}));
        auto wrong = fixture.receive();
        must(wrong.front().succeed(Payload{}));
        check(terminal(*fixture.b).header.outcome == EOperationOutcome::FAILED, "wrong reply schema fails terminal");
        auto closing_source = std::make_shared<Counted>(destroyed, 512);
        std::array closing_bytes{
            OwnedWireBytes::fromOwner(closing_source, std::span<const std::byte>(closing_source->bytes))
        };
        must(fixture.b->submit(must(fixture.b->bind(bulkDescriptor())), Payload{{0, 0, 512}}, closing_bytes));
        auto delayed = fixture.receive();
        closing_bytes = {};
        closing_source.reset();
        fixture.b->close();
        check(
            terminal(*fixture.b).header.outcome == EOperationOutcome::OUTCOME_UNKNOWN,
            "close cannot promise cancellation"
        );
        check(destroyed == 1 && fixture.listener->collectDisconnected() == 0, "active deferred owner prevents reclaim");
        must(delayed.front().succeed(Value{1}));
        check(!must(fixture.b->poll()), "late known reply cannot duplicate unknown terminal");
        check(destroyed == 2, "disconnected input destroyed once after its last use");
        check(fixture.listener->collectDisconnected() == 1, "cold collection removes only disconnected B");
        must(fixture.a->submit(must(fixture.a->bind(valueDescriptor())), Value{1}));
        auto held = fixture.receive();
        fixture.listener->stop();
        check(!held.front().succeed(Value{2}), "deferred work pins backing after listener stop");
        check(
            terminal(*fixture.a).header.outcome == EOperationOutcome::OUTCOME_UNKNOWN, "late client survives listener"
        );
        std::puts("ownership: 1 MiB identity, exact destruction, budgets, abandon, close and late work passed");
    }

    void races()
    {
        // Hold terminal credit after dequeue, independently of request-ring space.
        {
            Fixture fixture({4, 2, 128, 256, 1});
            auto op = must(fixture.a->bind(valueDescriptor()));
            auto first = must(fixture.a->submit(op, Value{1}));
            must(fixture.a->submit(op, Value{2}));
            auto work = fixture.receive();
            for (auto& item : work) must(item.succeed(Value{1}));
            must(fixture.a->abandon(first));
            check(!fixture.a->submit(op, Value{3}), "abandon does not return reserved terminal credit");
            (void)terminal(*fixture.a);
            check(fixture.a->pendingCount() == 0, "consumption returns credit");
            Packet truncated{must(encodeWire(Value{1}, {}, {8, 0})).subspan(0, 7), {}};
            check(!fixture.a->submit(op, truncated) && truncated.bytes.size() == 7, "invalid wire retains input");
            Packet excessive{must(encodeWire(Value{1}, {}, {8, 0})),
                std::make_shared<const std::vector<OwnedWireBytes>>(2)
            };
            check(!fixture.a->submit(op, excessive), "attachment count admission");
        }
        // Latches deliberately put client close/unknown consumption before deferred completion.
        // The server is the sole reply producer in all interleavings.
        for (int iteration = 0; iteration < 32; ++iteration)
        {
            std::barrier phases(2);
            auto operations = directory();
            std::thread server([&]
            {
                auto listener = must(LocalListener::listen("test.race", operations, {2, 2, 1024, 256, 1}));
                phases.arrive_and_wait();
                phases.arrive_and_wait();
                listener->accept(1);
                phases.arrive_and_wait();
                phases.arrive_and_wait();
                auto work = listener->endpoints().front()->receive();
                check(work.has_value(), "published work survives handoff");
                phases.arrive_and_wait();
                phases.arrive_and_wait();
                if (iteration % 3 == 2)
                {
                    listener->stop();
                    check(!work->succeed(Value{10}), "stop wins over unpublished terminal");
                }
                else must(work->succeed(Value{10}));
                listener->stop();
                phases.arrive_and_wait();
            });
            phases.arrive_and_wait();
            auto client = must(ClientConnectionEndpoint::connect("test.race"));
            phases.arrive_and_wait();
            phases.arrive_and_wait();
            auto op = must(client->bind(valueDescriptor()));
            must(client->submit(op, Value{9}));
            phases.arrive_and_wait();
            phases.arrive_and_wait();
            if (iteration % 3 == 0)
            {
                client->close();
                check(terminal(*client).header.outcome == EOperationOutcome::OUTCOME_UNKNOWN, "close before terminal");
            }
            phases.arrive_and_wait();
            phases.arrive_and_wait();
            if (iteration % 3 == 0) check(!must(client->poll()), "late completion does not produce a second terminal");
            else if (iteration % 3 == 1) check(valueOf(terminal(*client)) == 10, "published terminal survives stop");
            else
            {
                check(terminal(*client).header.outcome == EOperationOutcome::OUTCOME_UNKNOWN, "stop before completion");
            }
            server.join();
        }
        std::puts("races: 32 barrier-controlled close/complete/consume interleavings passed");
    }

    void distribution(std::string_view label, std::vector<double> samples)
    {
        check(!samples.empty(), "latency samples present");
        std::sort(samples.begin(), samples.end());
        std::printf("%.*s count=%zu p50=%.3f p95=%.3f p99=%.3f max=%.3f us\n",
            static_cast<int>(label.size()), label.data(), samples.size(), samples[samples.size() / 2],
            samples[samples.size() * 95 / 100], samples[samples.size() * 99 / 100], samples.back());
    }

    void progress()
    {
        auto operations = directory();
        std::atomic<bool> listening{false}, finish{false}, upload_stop{false};
        std::atomic<std::size_t> ticks{0}, uploads{0}, maximum_bytes{0}, maximum_depth{0};
        std::atomic<std::uint64_t> server_token{0};
        std::vector<double> tick_samples;
        tick_samples.reserve(50000);
        std::vector<std::array<std::size_t, 4>> queue_samples;
        queue_samples.reserve(50000);
        double maximum_tick = 0;
        std::thread server([&]
        {
            auto listener = must(LocalListener::listen("test.progress", operations, {8, 8, 4096, 256, 1}));
            listening.store(true, std::memory_order_release);
            auto previous = Clock::now();
            std::size_t cursor = 0;
            while (!finish.load(std::memory_order_acquire))
            {
                listener->accept(2);
                const auto endpoints = listener->endpoints();
                std::size_t total = 8;
                for (std::size_t visited = 0; visited < endpoints.size() && total; ++visited)
                {
                    auto& endpoint = endpoints[(cursor + visited) % endpoints.size()];
                    for (std::size_t local = 0; local < 2 && total; ++local)
                    {
                        auto work = endpoint->receive();
                        if (!work) break;
                        --total;
                        if (work->header().operation == operations->values()[1].id)
                        {
                            auto payload = must(work->decode<Payload>());
                            auto bytes = must(resolveExternal(
                                work->payload().attachmentView(), payload.external, 4096
                            ));
                            check(bytes.size() == 32 && bytes.data()[0] == std::byte{77}, "owned upload contents");
                            server_token.store(777, std::memory_order_release);
                            must(work->succeed(Value{777}));
                        }
                        else
                        {
                            auto value = must(work->decode<Value>()).value;
                            must(work->succeed(Value{value == 999 ? server_token.load() : value + 1}));
                        }
                    }
                }
                if (!endpoints.empty()) cursor = (cursor + 1) % endpoints.size();
                const auto now = Clock::now();
                const auto interval = std::chrono::duration<double, std::micro>(now - previous).count();
                maximum_tick = (std::max)(maximum_tick, interval);
                if (ticks.load() % 64 == 0 && tick_samples.size() < tick_samples.capacity())
                    tick_samples.push_back(interval);
                previous = now;
                ticks.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
            }
        });
        const auto deadline = Clock::now() + 15s;
        while (!listening.load(std::memory_order_acquire))
        {
            check(Clock::now() < deadline, "listen deadline");
            std::this_thread::yield();
        }
        auto a = must(ClientConnectionEndpoint::connect("test.progress"));
        std::thread uploader([&]
        {
            auto b = must(ClientConnectionEndpoint::connect("test.progress"));
            while (!b->ready())
            {
                check(Clock::now() < deadline, "upload ready deadline");
                std::this_thread::yield();
            }
            auto op = must(b->bind(bulkDescriptor()));
            auto owner = std::make_shared<std::array<std::byte, 32>>();
            owner->front() = std::byte{77};
            std::array bytes{OwnedWireBytes::fromOwner(owner, std::span<const std::byte>(*owner))};
            std::size_t sample_counter = 0;
            while (!upload_stop.load(std::memory_order_acquire))
            {
                for (int i = 0; i < 16; ++i) (void)b->submit(op, Payload{{0, 0, 32}}, bytes);
                maximum_depth.store((std::max)(maximum_depth.load(), b->requestDepth()));
                maximum_bytes.store((std::max)(maximum_bytes.load(), b->inFlightBytes()));
                if (++sample_counter % 128 == 0 && queue_samples.size() < queue_samples.capacity())
                    queue_samples.push_back({ticks.load(), b->requestDepth(), b->replyDepth(), b->inFlightBytes()});
                while (auto reply = must(b->poll()))
                {
                    check(valueOf(*reply) == 777, "B receives its own terminal");
                    uploads.fetch_add(1, std::memory_order_relaxed);
                }
                check(Clock::now() < deadline, "uploader deadline");
                std::this_thread::yield();
            }
        });
        while (!a->ready())
        {
            check(Clock::now() < deadline, "main ready deadline");
            std::this_thread::yield();
        }
        auto op = must(a->bind(valueDescriptor()));
        for (std::uint64_t i = 0; i < 8; ++i) must(a->submit(op, Value{i}));
        check(!a->submit(op, Value{9}), "A exhausts only its own credit");
        const auto start_ticks = ticks.load();
        const auto start_uploads = uploads.load();
        std::this_thread::sleep_for(1200ms);
        std::printf("paused progress: ticks=%zu uploads=%zu\n", ticks.load() - start_ticks,
            uploads.load() - start_uploads);
        const bool has_independent_progress = ticks.load() > start_ticks + 100 && uploads.load() > start_uploads + 100;
        check(has_independent_progress, "A pause cannot stall B or ticks");
        for (std::uint64_t i = 0; i < 8; ++i) check(valueOf(terminal(*a)) == i + 1, "A FIFO replies");
        for (std::uint64_t i = 0; i < 200; ++i)
        {
            must(a->submit(op, Value{i}));
            for (;;)
            {
                if (auto reply = must(a->poll()))
                {
                    check(valueOf(*reply) == i + 1, "A progress under B flood");
                    break;
                }
                check(Clock::now() < deadline, "A fairness deadline");
                std::this_thread::yield();
            }
        }
        upload_stop.store(true, std::memory_order_release);
        uploader.join();
        must(a->submit(op, Value{999}));
        for (;;)
        {
            if (auto reply = must(a->poll()))
            {
                check(valueOf(*reply) == 777, "server resource survives B disconnect");
                break;
            }
            check(Clock::now() < deadline, "global token deadline");
            std::this_thread::yield();
        }
        a->close();
        finish.store(true, std::memory_order_release);
        server.join();
        std::ofstream raw("g3r-ticks.csv");
        raw << "tick,interval_us\n";
        for (std::size_t i = 0; i < tick_samples.size(); ++i) raw << i << ',' << tick_samples[i] << '\n';
        distribution("tick", tick_samples);
        std::ofstream queues("g3r-queues.csv");
        queues << "tick,request_depth,reply_depth,inflight_bytes\n";
        for (const auto& sample : queue_samples)
            queues << sample[0] << ',' << sample[1] << ',' << sample[2] << ',' << sample[3] << '\n';
        std::printf(
            "tick_sample_stride=64 tick_all_max_us=%.3f queue_samples=%zu\n", maximum_tick, queue_samples.size()
        );
        std::printf("pause_ms=1200 uploads=%zu ticks=%zu max_request_depth=%zu max_inflight_bytes=%zu\n",
            uploads.load(), ticks.load(), maximum_depth.load(), maximum_bytes.load());
    }

    void cost()
    {
        allocation::enabled = true;
        Fixture fixture;
        auto op = must(fixture.a->bind(valueDescriptor()));
        allocation::enabled = false;
        std::printf("cold assembly allocations=%zu bytes=%zu\n", allocation::count, allocation::bytes);
        auto roundtrip = [&]
        {
            const auto id = must(fixture.a->submit(op, Value{41}));
            for (const auto& endpoint : fixture.listener->endpoints())
            {
                if (auto work = endpoint->receive()) must(work->succeed(Value{must(work->decode<Value>()).value + 1}));
            }
            auto reply = terminal(*fixture.a);
            check(reply.header.request == id && valueOf(reply) == 42, "full codec roundtrip");
        };
        allocation::count = allocation::bytes = 0;
        allocation::enabled = true;
        auto cold_start = Clock::now();
        roundtrip();
        const auto cold_time = std::chrono::duration<double, std::micro>(Clock::now() - cold_start).count();
        allocation::enabled = false;
        std::printf("cold first codec operation allocations=%zu bytes=%zu latency_us=%.3f\n",
            allocation::count, allocation::bytes, cold_time);
        for (int i = 0; i < 100; ++i) roundtrip();
        std::vector<double> samples;
        samples.reserve(10000);
        allocation::count = allocation::bytes = 0;
        allocation::enabled = true;
        for (int i = 0; i < 10000; ++i)
        {
            auto start = Clock::now();
            roundtrip();
            samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
        }
        allocation::enabled = false;
        std::printf(
            "warm full codec allocations=%zu bytes=%zu iterations=10000\n", allocation::count, allocation::bytes
        );
        check(allocation::count == 0, "warm complete small operation does not allocate");
        std::ofstream raw("g3r-roundtrip.csv");
        raw << "iteration,roundtrip_us\n";
        for (std::size_t i = 0; i < samples.size(); ++i) raw << i << ',' << samples[i] << '\n';
        distribution("full codec roundtrip", samples);
    }
}

int main(int argc, char** argv)
{
    using namespace connection_test;
    check(argc == 2, "test selection required");
    const std::string_view test = argv[1];
    if (test == "contracts") contracts();
    else if (test == "ownership") ownership();
    else if (test == "races") races();
    else if (test == "progress") progress();
    else if (test == "cost") cost();
    else check(false, "unknown test");
}
