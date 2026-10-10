#include "Support.hpp"

#include <barrier>
#include <latch>
#include <thread>

using namespace transport_test;

void testThreads()
{
    Fixture fixture;
    auto& transport = *fixture.transport;
    auto candidate = transport.makeProgramPacket();
    auto control = transport.makeControlPacket();
    std::thread wrong([&] {
        check(fails(candidate.write(fixture.value, Value{}), kTransportWrongThread), "PROGRAM owner-thread writer");
        check(fails(transport.submit(candidate), kTransportWrongThread), "PROGRAM owner-thread submit");
        check(fails(control.request(fixture.ping, Ping{}), kTransportWrongThread), "CONTROL owner-thread writer");
        check(fails(transport.submit(control), kTransportWrongThread), "CONTROL owner-thread submit");
    });
    wrong.join();
    constexpr std::uint64_t count = 50000;
    std::thread consumer([&] {
        for (std::uint64_t next = 0; next < count;)
        {
            const auto epoch = transport.wake().snapshot();
            const auto consumed = must(transport.pollProgram([&](RenderPacketView& view) noexcept {
                check(must(view.read(fixture.value, 0)).value == next, "SPSC FIFO");
                ++next;
            }));
            if (!consumed) transport.wake().wait(epoch);
        }
    });
    for (std::uint64_t next = 0; next < count; ++next)
    {
        must(candidate.write(fixture.value, Value{next}));
        while (true)
        {
            auto result = transport.submit(candidate);
            if (result) break;
            check(fails(result, kTransportCapacity), "only normal PROGRAM backpressure");
            std::this_thread::yield();
        }
    }
    consumer.join();
    check(fails(transport.pollProgram([](RenderPacketView&) noexcept {}), kTransportWrongThread), "single consumer");

    Fixture requests;
    auto inbox = requests.transport->replyInbox();
    auto request = requests.transport->makeControlPacket();
    constexpr std::uint64_t request_count = 5000;
    std::thread responder([&] {
        for (std::uint64_t next = 0; next < request_count;)
        {
            auto consumed = must(requests.transport->pollControl([&](RenderPacketView& view) noexcept {
                check(must(view.read(requests.ping, 0)).value == next, "CONTROL FIFO");
                must(view.complete(0, Ack{next++}));
            }));
            if (!consumed) std::this_thread::yield();
        }
    });
    for (std::uint64_t next = 0; next < request_count; ++next)
    {
        auto ticket = must(request.request(requests.ping, Ping{next}));
        must(requests.transport->submit(request));
        while (true)
        {
            auto result = must(inbox.poll(ticket));
            if (result) { check(result->value == next, "cross-thread reply"); break; }
            std::this_thread::yield();
        }
    }
    responder.join();
}

void testUploads()
{
    constexpr unsigned producers = 4, consumers = 3, per_producer = 2000, total = producers * per_producer;
    Fixture fixture;
    auto& transport = *fixture.transport;
    std::vector<std::atomic<unsigned>> seen(total);
    std::atomic<unsigned> processed{};
    std::barrier start{producers + consumers};
    std::vector<std::thread> workers;
    for (unsigned producer = 0; producer < producers; ++producer)
    {
        workers.emplace_back([&, producer] {
            auto packet = transport.makeUploadPacket();
            start.arrive_and_wait();
            for (unsigned index = 0; index < per_producer; ++index)
            {
                must(packet.write(fixture.upload, Upload{producer * per_producer + index, {}}));
                while (true)
                {
                    auto result = transport.submit(packet);
                    if (result) break;
                    check(fails(result, kTransportCapacity) || fails(result, kTransportBusy), "MPMC pressure");
                    std::this_thread::yield();
                }
            }
        });
    }
    for (unsigned consumer = 0; consumer < consumers; ++consumer)
    {
        workers.emplace_back([&] {
            auto scratch = transport.makeUploadPacket();
            start.arrive_and_wait();
            while (processed.load() != total)
            {
                auto result = transport.pollUpload(scratch, [&](RenderPacketView& view) noexcept {
                    const auto value = must(view.read(fixture.upload, 0)).value;
                    check(value < total && seen[value].fetch_add(1) == 0, "MPMC exact once");
                    processed.fetch_add(1);
                });
                check(result || fails(result, kTransportBusy), "MPMC consumer contention");
                if (!result || !*result) std::this_thread::yield();
            }
        });
    }
    for (auto& worker : workers) worker.join();
    check(transport.uploadBytes() == 0, "MPMC byte accounting drained");
    for (auto& count : seen) check(count.load() == 1, "MPMC no loss");

    auto capacity = RenderTransportCapacity{};
    capacity.upload_bytes = sizeof(Upload) + 64;
    Fixture bounded{capacity};
    auto packet = bounded.transport->makeUploadPacket();
    const std::array<std::byte, 64> bytes{};
    auto reference = must(packet.attach(PinnedRenderBytes::copy(bytes)));
    must(packet.write(bounded.upload, Upload{1, reference}));
    must(bounded.transport->submit(packet));
    std::latch entered{1}, release{1};
    std::thread blocked([&] {
        auto scratch = bounded.transport->makeUploadPacket();
        must(bounded.transport->pollUpload(scratch, [&](RenderPacketView&) noexcept {
            entered.count_down();
            release.wait();
        }));
    });
    entered.wait();
    must(packet.write(bounded.upload, Upload{}));
    check(fails(bounded.transport->submit(packet), kTransportByteBudget), "budget includes active dispatch");
    check(packet.size() == 1, "budget rejection retains upload");
    release.count_down();
    blocked.join();
    must(bounded.transport->submit(packet));
    auto scratch = bounded.transport->makeUploadPacket();
    must(bounded.transport->pollUpload(scratch, [](RenderPacketView&) noexcept {}));
    check(bounded.transport->uploadBytes() == 0, "retired byte credit is reusable");
}

void testStop()
{
    for (unsigned round = 0; round < 100; ++round)
    {
        Fixture racing;
        auto inbox = racing.transport->replyInbox();
        auto packet = racing.transport->makeControlPacket();
        auto ticket = must(packet.request(racing.ping, Ping{}));
        must(racing.transport->submit(packet));
        std::optional<RenderReplyPromise<Ack>> promise;
        must(racing.transport->pollControl([&](RenderPacketView& view) noexcept {
            promise.emplace(must(view.deferReply<Ack>(0)));
        }));
        std::barrier start{2};
        std::thread completer([&] {
            start.arrive_and_wait();
            auto result = promise->complete(Ack{123});
            check(result || fails(result, kTransportReply), "completion loses only to stop");
        });
        start.arrive_and_wait();
        racing.transport->requestStop();
        completer.join();
        auto result = inbox.poll(ticket);
        const bool has_value = result && result->has_value() && (**result).value == 123;
        check(has_value || fails(result, kTransportStopping), "exactly one terminal reply wins");
        check(fails(inbox.poll(ticket), kTransportReply), "terminal reply consumed once");
    }
    for (unsigned round = 0; round < 50; ++round)
    {
        Fixture fixture;
        auto& transport = *fixture.transport;
        auto inbox = transport.replyInbox();
        auto control = transport.makeControlPacket();
        auto ticket = must(control.request(fixture.ping, Ping{}));
        must(transport.submit(control));
        std::latch ready{1};
        std::thread worker([&] {
            auto packet = transport.makeUploadPacket();
            ready.count_down();
            for (unsigned index = 0; index < 200; ++index)
            {
                must(packet.write(fixture.upload, Upload{index, {}}));
                auto result = transport.submit(packet);
                if (!result)
                {
                    const bool is_expected = fails(result, kTransportStopping) ||
                        fails(result, kTransportCapacity) || fails(result, kTransportBusy);
                    check(is_expected, "stop/admission race outcome");
                    break;
                }
            }
        });
        ready.wait();
        transport.requestStop();
        worker.join();
        check(fails(inbox.poll(ticket), kTransportStopping), "stop settles accepted request");
        check(fails(transport.submit(control), kTransportStopping), "no new control after stop");
        auto scratch = transport.makeUploadPacket();
        auto forbidden = [](RenderPacketView&) noexcept { check(false, "stopped work must not execute"); };
        must(transport.pollControl(forbidden));
        while (must(transport.pollUpload(scratch, forbidden))) {}
        check(transport.drained() && transport.uploadBytes() == 0, "stopped queues drained");
    }
    Fixture fixture;
    std::latch waiting{1};
    std::thread waiter([&] {
        const auto epoch = fixture.transport->wake().snapshot();
        waiting.count_down();
        fixture.transport->wake().wait(epoch);
    });
    waiting.wait();
    fixture.transport->requestStop();
    waiter.join();
}
