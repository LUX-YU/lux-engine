#include "Support.hpp"

#include <atomic>
#include <barrier>
#include <thread>

using namespace transport_test;

namespace
{
    void spsc()
    {
        HandlerState context;
        context.seen.reserve(2000);
        RenderRouteTable table(1);
        auto descriptor = operationDescriptor<Value, Value>("test.spsc");
        must(table.registerOperation(descriptor, &context, handler));
        auto bound = must(table.bind<Value, Value>(descriptor));
        auto receiver = must(RenderTransport::create(std::move(table)));
        auto sender = receiver->sender();
        auto packet = sender.makeBatch();
        auto requests = sender.makeRequests();
        const auto bytes = encode(1);
        std::thread wrong([&]
        {
            auto other = sender.makeBatch();
            auto output = sender.makeRequests();
            must(other.append(bound, bytes));
            auto result = sender.submit(other, output, EDelivery::COMMAND);
            check(!result && result.error().type == kTransportThread, "SPSC wrong producer");
        });
        wrong.join();
        std::thread consumer([&]
        {
            for (unsigned received = 0; received < 2000;)
            {
                const auto epoch = sender.wakeEpoch();
                if (must(receiver->poll(EDelivery::COMMAND))) ++received;
                else
                {
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                    static_cast<void>(sender.waitUntil(epoch, deadline));
                }
            }
        });
        for (unsigned i = 0; i < 2000; ++i)
        {
            must(packet.append(bound, encode(i + 100)));
            must(sender.submit(packet, requests, EDelivery::COMMAND));
            for (;;)
            {
                auto epoch = sender.wakeEpoch();
                auto result = must(requests.requests()[0].poll());
                if (result) { check(result->outcome == EOperationOutcome::SUCCESS, "SPSC terminal"); break; }
                static_cast<void>(sender.waitUntil(epoch, std::chrono::steady_clock::now() + std::chrono::seconds(2)));
            }
            requests.clear();
        }
        consumer.join();
        for (unsigned i = 0; i < 2000; ++i) check(context.seen[i] == i + 100, "SPSC FIFO");
        check(!receiver->poll(EDelivery::COMMAND), "SPSC wrong consumer");
    }

    void mpmc()
    {
        constexpr unsigned producers = 4, consumers = 3, per_producer = 400, total = producers * per_producer;
        struct Context final
        {
            std::array<std::atomic<unsigned>, total> seen{};
            std::atomic<unsigned> processed{};
        } context;
        auto callback = [](void* opaque, ReplyPromise promise) noexcept
        {
            auto& value = *static_cast<Context*>(opaque);
            auto input = must(decodeWire<Value>(must(promise.input()), {8, 0}));
            check(input.value < total, "MPMC range");
            check(value.seen[input.value].fetch_add(1) == 0, "MPMC exact once");
            must(promise.success());
            value.processed.fetch_add(1);
        };
        RenderRouteTable table(1);
        auto descriptor = operationDescriptor<Value>("test.mpmc");
        must(table.registerOperation(descriptor, &context, callback));
        auto bound = must(table.bind<Value>(descriptor));
        auto receiver = must(RenderTransport::create(std::move(table)));
        auto sender = receiver->sender();
        std::barrier start(producers + consumers);
        std::vector<std::thread> workers;
        for (unsigned n = 0; n < producers; ++n)
        {
            workers.emplace_back([&, n]
            {
                auto packet = sender.makeBatch();
                auto requests = sender.makeRequests();
                start.arrive_and_wait();
                for (unsigned i = 0; i < per_producer; ++i)
                {
                    must(packet.append(bound, encode(n * per_producer + i)));
                    for (;;)
                    {
                        auto submitted = sender.submit(packet, requests, EDelivery::BULK);
                        if (submitted) break;
                        check(submitted.error().type == kTransportBusy || submitted.error().type == kTransportCapacity,
                            "bounded MPMC contention");
                        std::this_thread::yield();
                    }
                    while (!must(requests.requests()[0].poll())) std::this_thread::yield();
                    requests.clear();
                }
            });
        }
        for (unsigned n = 0; n < consumers; ++n)
        {
            workers.emplace_back([&]
            {
                start.arrive_and_wait();
                while (context.processed.load() != total)
                {
                    auto result = receiver->poll(EDelivery::BULK);
                    check(result || result.error().type == kTransportBusy, "MPMC consumer contention");
                    if (!result || !*result) std::this_thread::yield();
                }
            });
        }
        for (auto& worker : workers) worker.join();
        for (auto& seen : context.seen) check(seen.load() == 1, "MPMC no loss");
        check(sender.bulkBytes() == 0, "MPMC budgets returned");
    }

    void races()
    {
        // Completion, consumption and abandon race on the same cell; exactly one
        // client action wins, while the producer still owns its input-use lease.
        for (unsigned round = 0; round < 100; ++round)
        {
            HandlerState context;
            context.defer = true;
            RenderRouteTable table(1);
            auto descriptor = operationDescriptor<Value, Value>("test.consume-race");
            must(table.registerOperation(descriptor, &context, handler));
            auto bound = must(table.bind<Value, Value>(descriptor));
            TransportCapacity limits;
            limits.replies = 1;
            auto receiver = must(RenderTransport::create(std::move(table), limits));
            auto sender = receiver->sender();
            auto packet = sender.makeBatch();
            auto requests = sender.makeRequests();
            must(packet.append(bound, encode(1)));
            must(sender.submit(packet, requests, EDelivery::COMMAND));
            must(receiver->poll(EDelivery::COMMAND));
            auto ticket = requests.requests()[0].ticket();
            auto inbox = sender.inbox();
            std::barrier start(3);
            std::atomic<unsigned> winners{};
            std::thread writer([&, promise = std::move(*context.deferred)]() mutable
            {
                start.arrive_and_wait();
                must(promise.success(DataSchema<Value>::describe(), context.answer));
            });
            context.deferred.reset();
            std::thread consumer([&]
            {
                start.arrive_and_wait();
                for (;;)
                {
                    auto result = inbox.poll(ticket);
                    if (!result) break;
                    if (*result) { ++winners; break; }
                    std::this_thread::yield();
                }
            });
            start.arrive_and_wait();
            if (inbox.abandon(ticket)) ++winners;
            writer.join();
            consumer.join();
            check(winners == 1, "one consume/abandon winner");
            requests.clear();
            must(packet.append(bound, encode(2)));
            must(sender.submit(packet, requests, EDelivery::COMMAND));
            check(!inbox.poll(ticket), "racing old ticket cannot contaminate reused slot");
            must(receiver->poll(EDelivery::COMMAND));
            context.deferred.reset();
            check(terminal(requests.requests()[0]).outcome == EOperationOutcome::CANCELLED, "new generation settled");
        }
        for (unsigned round = 0; round < 100; ++round)
        {
            HandlerState context;
            context.defer = true;
            RenderRouteTable table(1);
            auto descriptor = operationDescriptor<Value, Value>("test.race");
            must(table.registerOperation(descriptor, &context, handler));
            auto bound = must(table.bind<Value, Value>(descriptor));
            auto receiver = must(RenderTransport::create(std::move(table)));
            auto sender = receiver->sender();
            auto packet = sender.makeBatch();
            auto requests = sender.makeRequests();
            must(packet.append(bound, encode(1)));
            must(sender.submit(packet, requests, EDelivery::COMMAND));
            must(receiver->poll(EDelivery::COMMAND));
            auto stale = requests.requests()[0].ticket();
            std::barrier start(3);
            std::thread writer([&, promise = std::move(*context.deferred)]() mutable
            {
                start.arrive_and_wait();
                auto result = promise.success(DataSchema<Value>::describe(), context.answer);
                check(result || result.error().type == kTransportStale, "one terminal winner");
            });
            context.deferred.reset();
            std::thread stopper([&] { start.arrive_and_wait(); receiver->requestStop(); });
            start.arrive_and_wait();
            if (round % 2) must(requests.requests()[0].abandon());
            writer.join();
            stopper.join();
            if (round % 2 == 0)
            {
                auto reply = terminal(requests.requests()[0]);
                check(reply.outcome == EOperationOutcome::SUCCESS || reply.outcome == EOperationOutcome::CANCELLED,
                    "race yields real terminal");
            }
            check(!sender.inbox().poll(stale), "one observable consumption");
            check(receiver->drained(), "race drains");
        }
        // Admission racing close: successful publication must still be settled.
        for (unsigned round = 0; round < 100; ++round)
        {
            RenderRouteTable table(1);
            auto descriptor = operationDescriptor<Value>("test.admission");
            must(table.registerOperation(descriptor, nullptr, voidHandler));
            auto bound = must(table.bind<Value>(descriptor));
            auto receiver = must(RenderTransport::create(std::move(table)));
            auto sender = receiver->sender();
            std::barrier start(2);
            std::thread submitter([&]
            {
                auto packet = sender.makeBatch();
                auto requests = sender.makeRequests();
                must(packet.append(bound, encode(1)));
                start.arrive_and_wait();
                auto result = sender.submit(packet, requests, EDelivery::BULK);
                if (!result)
                {
                    check(result.error().type == kTransportStopped, "admission close rejection");
                    check(packet.size() == 1, "close preserves unsubmitted bytes");
                    return;
                }
                while (!must(requests.requests()[0].poll())) std::this_thread::yield();
            });
            start.arrive_and_wait();
            receiver->requestStop();
            submitter.join();
            while (must(receiver->poll(EDelivery::BULK))) {}
            check(receiver->drained() && sender.bulkBytes() == 0, "admission race safely retired");
        }
    }

    void orderingAndWait()
    {
        HandlerState context;
        RenderRouteTable table(1);
        auto descriptor = operationDescriptor<Value, Value>("test.ordering");
        must(table.registerOperation(descriptor, &context, handler));
        auto bound = must(table.bind<Value, Value>(descriptor));
        auto receiver = must(RenderTransport::create(std::move(table)));
        auto sender = receiver->sender();
        auto packet = sender.makeBatch();
        auto bulk = sender.makeRequests();
        auto command = sender.makeRequests();
        must(packet.append(bound, encode(1)));
        must(sender.submit(packet, bulk, EDelivery::BULK));
        must(packet.append(bound, encode(2)));
        must(sender.submit(packet, command, EDelivery::PRIORITY_COMMAND));
        must(receiver->poll(EDelivery::PRIORITY_COMMAND));
        check(!must(bulk.requests()[0].poll()), "no cross-lane completion guarantee");
        must(receiver->poll(EDelivery::BULK));
        check(context.seen == std::vector<std::uint64_t>{2, 1}, "intentional cross-lane reverse order");
        static_cast<void>(terminal(bulk.requests()[0]));
        static_cast<void>(terminal(command.requests()[0]));
        bulk.clear(); command.clear();
        // A causal cross-lane dependency is established by observing success,
        // then submitting its dependent operation, never by lane priority.
        must(packet.append(bound, encode(3)));
        must(sender.submit(packet, bulk, EDelivery::BULK));
        must(receiver->poll(EDelivery::BULK));
        check(terminal(bulk.requests()[0]).outcome == EOperationOutcome::SUCCESS, "dependency committed");
        must(packet.append(bound, encode(4)));
        must(sender.submit(packet, command, EDelivery::PRIORITY_COMMAND));
        must(receiver->poll(EDelivery::PRIORITY_COMMAND));
        check(terminal(command.requests()[0]).outcome == EOperationOutcome::SUCCESS, "dependent operation succeeds");
        check(context.seen[2] == 3 && context.seen[3] == 4, "explicit cross-lane causal order");
        bulk.clear();
        command.clear();
        for (unsigned round = 0; round < 100; ++round)
        {
            auto priority = sender.makeRequests();
            for (auto lane : {EDelivery::COMMAND, EDelivery::PRIORITY_COMMAND, EDelivery::BULK})
            {
                must(packet.append(bound, encode(100 + round)));
                auto& out = lane == EDelivery::COMMAND ? command : lane == EDelivery::BULK ? bulk : priority;
                must(sender.submit(packet, out, lane));
            }
            for (auto lane : {EDelivery::PRIORITY_COMMAND, EDelivery::COMMAND, EDelivery::BULK})
            {
                must(receiver->poll(lane));
            }
            check(terminal(bulk.requests()[0]).outcome == EOperationOutcome::SUCCESS, "bounded fair bulk progress");
            static_cast<void>(terminal(command.requests()[0]));
            static_cast<void>(terminal(priority.requests()[0]));
            command.clear(); bulk.clear();
        }
        auto epoch = sender.wakeEpoch();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
        check(!sender.waitUntil(epoch, deadline), "deadline exits");
        std::stop_source stop;
        std::barrier start(2);
        std::thread waiter([&]
        {
            start.arrive_and_wait();
            const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            static_cast<void>(sender.waitUntil(epoch, limit, stop.get_token()));
            check(stop.stop_requested(), "caller stop exits wait");
        });
        start.arrive_and_wait(); stop.request_stop(); waiter.join();
        std::thread stopped_waiter([&]
        {
            static_cast<void>(sender.waitUntil(epoch, std::chrono::steady_clock::now() + std::chrono::seconds(10)));
            check(sender.stopping(), "endpoint stop exits wait");
        });
        receiver->requestStop(); stopped_waiter.join();
    }
}

void concurrencyTests(std::string_view test)
{
    if (test == "spsc") spsc();
    else if (test == "mpmc") mpmc();
    else if (test == "races") races();
    else if (test == "ordering") orderingAndWait();
    else check(false, "unknown test");
}
