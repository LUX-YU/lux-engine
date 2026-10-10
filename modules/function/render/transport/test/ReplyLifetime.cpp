#include "Support.hpp"

#include <barrier>
#include <thread>

using namespace transport_test;

namespace
{
    std::atomic<bool> hold_writer{}, writer_entered{}, release_writer{};

    RenderTransportCapacity singleReply()
    {
        RenderTransportCapacity capacity;
        capacity.replies = 1;
        return capacity;
    }

    void lifecycle()
    {
        Fixture fixture{singleReply()}, foreign;
        auto& transport = *fixture.transport;
        auto inbox = transport.replyInbox();
        auto packet = transport.makeControlPacket();
        for (unsigned index = 0; index < 10000; ++index)
        {
            auto ticket = must(packet.request(fixture.ping, Ping{}));
            must(packet.reset());
            must(inbox.abandon(ticket));
            check(fails(inbox.abandon(ticket), kTransportReply), "duplicate abandon is stale");
        }
        auto ticket = must(packet.request(fixture.ping, Ping{}));
        check(fails(foreign.transport->replyInbox().abandon(ticket), kTransportWrongOwner), "foreign abandon");
        must(transport.submit(packet));
        must(transport.pollControl([](RenderPacketView& view) noexcept { must(view.complete(0, Ack{7})); }));
        check(fails(packet.request(fixture.ping, Ping{}), kTransportCapacity), "unread completion owns capacity");
        must(inbox.abandon(ticket));
        check(fails(inbox.poll(ticket), kTransportReply), "abandoned completed result cannot be consumed");

        auto deferred = must(packet.request(fixture.ping, Ping{}));
        must(transport.submit(packet));
        std::optional<RenderReplyPromise<Ack>> promise;
        must(transport.pollControl([&](RenderPacketView& view) noexcept {
            promise.emplace(must(view.deferReply<Ack>(0)));
        }));
        must(inbox.abandon(deferred));
        auto replacement = must(packet.request(fixture.ping, Ping{}));
        check(fails(promise->complete(Ack{99}), kTransportReply), "late completion rejects recycled generation");
        promise.reset();
        check(!must(inbox.poll(replacement)), "old promise destruction cannot cancel replacement");
        must(transport.submit(packet));
        must(transport.pollControl([](RenderPacketView& view) noexcept {
            must(view.complete(0, Ack{8}));
            check(fails(view.complete(0, Ack{9}), kTransportReply), "duplicate completion rejected");
        }));
        check(must(inbox.poll(replacement))->value == 8, "replacement unpolluted");
        check(fails(inbox.poll(replacement), kTransportReply), "duplicate consumption rejected");

        auto cancelled = must(packet.request(fixture.ping, Ping{}));
        must(transport.submit(packet));
        must(transport.pollControl([&](RenderPacketView& view) noexcept {
            auto abandoned_promise = must(view.deferReply<Ack>(0));
        }));
        must(inbox.abandon(cancelled));
        auto stopped = must(packet.request(fixture.ping, Ping{}));
        must(transport.submit(packet));
        must(transport.pollControl([&](RenderPacketView& view) noexcept {
            promise.emplace(must(view.deferReply<Ack>(0)));
        }));
        fixture.transport.reset();
        must(inbox.abandon(stopped));
        check(fails(promise->complete(Ack{}), kTransportReply), "promise safe after transport and receiver end");
    }

    void writing(bool cancellation)
    {
        Fixture fixture{singleReply()};
        auto inbox = fixture.transport->replyInbox();
        auto packet = fixture.transport->makeControlPacket();
        auto ticket = must(packet.request(fixture.ping, Ping{}));
        must(fixture.transport->submit(packet));
        std::optional<RenderReplyPromise<Ack>> promise;
        must(fixture.transport->pollControl([&](RenderPacketView& view) noexcept {
            promise.emplace(must(view.deferReply<Ack>(0)));
        }));
        writer_entered = false;
        release_writer = false;
        hold_writer = true;
        std::thread writer([&] {
            if (cancellation) promise.reset();
            else must(promise->complete(Ack{42}));
        });
        writer_entered.wait(false);
        must(inbox.abandon(ticket));
        check(fails(inbox.abandon(ticket), kTransportReply), "writing abandon transfers responsibility once");
        check(fails(packet.request(fixture.ping, Ping{}), kTransportCapacity), "WRITING storage not reused early");
        hold_writer = false;
        release_writer = true;
        release_writer.notify_all();
        writer.join();
        auto next = must(packet.request(fixture.ping, Ping{}));
        if (promise) check(fails(promise->complete(Ack{}), kTransportReply), "writer cannot complete next generation");
        must(inbox.abandon(next));
        must(packet.reset());
    }

    void races()
    {
        for (unsigned round = 0; round < 1000; ++round)
        {
            Fixture fixture{singleReply()};
            auto inbox = fixture.transport->replyInbox();
            auto packet = fixture.transport->makeControlPacket();
            auto ticket = must(packet.request(fixture.ping, Ping{}));
            must(fixture.transport->submit(packet));
            std::optional<RenderReplyPromise<Ack>> promise;
            must(fixture.transport->pollControl([&](RenderPacketView& view) noexcept {
                promise.emplace(must(view.deferReply<Ack>(0)));
            }));
            std::barrier start{3};
            std::thread backend([&] {
                start.arrive_and_wait();
                if (round % 2) promise.reset();
                else
                {
                    auto result = promise->complete(Ack{round});
                    check(result || fails(result, kTransportReply), "completion vs abandon/stop");
                }
            });
            std::thread stop([&] {
                start.arrive_and_wait();
                if (round % 3 == 0) fixture.transport->requestStop();
            });
            start.arrive_and_wait();
            must(inbox.abandon(ticket));
            backend.join();
            stop.join();
            check(fails(inbox.poll(ticket), kTransportReply), "no result after reception abandoned");
            if (!fixture.transport->stopping())
            {
                auto next = must(packet.request(fixture.ping, Ping{}));
                if (promise) check(fails(promise->complete(Ack{}), kTransportReply), "late generation after race");
                must(inbox.abandon(next));
            }
        }
    }
}

namespace lux::render::detail
{
    void replyWritingTestHook() noexcept
    {
        if (hold_writer.load())
        {
            writer_entered = true;
            writer_entered.notify_all();
            release_writer.wait(false);
        }
    }
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    const std::string_view test{argv[1]};
    if (test == "lifecycle") lifecycle();
    else if (test == "writing") writing(false);
    else if (test == "cancelling") writing(true);
    else if (test == "races") races();
    else return 3;
    return 0;
}
