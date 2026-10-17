#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/runtime/Commands.hpp>
#include <lux/engine/render/protocol/AsyncCommand.hpp>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include "TestHooks.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace lux::render;
namespace ipc = lux::platform::ipc;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "raw runtime line %d: %s\n", __LINE__, #x); std::abort(); } } while(false)
template <class T, class E> T take(lux::cxx::expected<T, E> value) { CHECK(value); return std::move(*value); }

struct Source final
{
    std::atomic<unsigned>& destroyed;
    std::vector<unsigned> values;
    Source(std::atomic<unsigned>& count, unsigned value) : destroyed(count), values(16384, value) {}
    ~Source() { ++destroyed; }
};
struct Work final
{
    static constexpr std::string_view name = "test.raw.work";
    struct Request final { std::unique_ptr<Source> source; unsigned mode = 0; };
    using Response = unsigned;
};
struct Handler final
{
    static constexpr std::string_view name = "test.raw.handler";
    std::atomic<unsigned>& calls;
    std::atomic<const unsigned*>& observed;
    Handler(std::atomic<unsigned>& c, std::atomic<const unsigned*>& o) noexcept : calls(c), observed(o) {}
    void work(const Work::Request& input, Reply<Work> reply) noexcept
    {
        ++calls;
        observed = input.source->values.data();
        const auto result = input.source->values.front() + 1;
        switch (input.mode)
        {
        case 0: CHECK(reply.complete(unsigned(result))); break;
        case 1: CHECK(reply.fail(RenderError{kRuntimeState, {17}})); break;
        case 2: CHECK(reply.cancel()); break;
        case 3:
        {
            auto stable = take(reply.defer());
            CHECK(stable.request().source->values.front() + 1 == result);
            CHECK(stable.completeWhen(stdexec::just(unsigned(result))));
            break;
        }
        case 4: CHECK(reply.completeWhen(stdexec::just_error(RenderError{kRuntimeState, {23}}))); break;
        case 5: CHECK(reply.completeWhen(stdexec::just_stopped())); break;
        default: std::abort();
        }
    }
    using Bindings = std::tuple<CommandMethod<Work, &Handler::work>>;
};

static ipc::IpcMessage receive(ipc::IpcRequester& peer)
{
    const auto deadline = Clock::now() + 10s;
    ipc::IpcMessage message;
    for (;;)
    {
        const auto status = peer.tryReceive(message);
        CHECK(status != ipc::EIpcReceive::CLOSED && Clock::now() < deadline);
        if (status == ipc::EIpcReceive::RECEIVED) return message;
        std::this_thread::yield();
    }
}

int main()
{
    std::atomic<unsigned> calls{0}, destroyed{0};
    std::atomic<const unsigned*> observed{nullptr};
    auto server = take(RenderServer::create({"runtime.raw", true}));
    (void)take(server->emplaceHandler<Handler>("main", {}, calls, observed));
    const auto command = take(server->commandId<Work>());
    auto a = take(ipc::IpcCore::instance().createRequester("runtime.raw", {256, 1}));
    auto b = take(ipc::IpcCore::instance().createRequester("runtime.raw", {256, 1}));
    const auto send = [&](ipc::IpcRequester& peer, RequestId id, unsigned value, unsigned mode = 0) {
        ipc::IpcMessage message;
        message.emplace<Work::Request>(Work::Request{std::make_unique<Source>(destroyed, value), mode});
        message.setMetadata(RequestHeader{id, command});
        CHECK(peer.trySend(message));
        CHECK(message.empty());
    };
    const auto wait_destroyed = [&](unsigned expected) {
        const auto deadline = Clock::now() + 10s;
        while (destroyed < expected) { CHECK(Clock::now() < deadline); std::this_thread::yield(); }
        CHECK(destroyed == expected);
    };
    send(a, {1}, 10);
    send(a, {2}, 20);
    // Actual reply growth allocation failure after side effects. Existing segment slots still work.
    ipc::testing::failSegmentAllocations(100000);
    bool ran = false;
    std::thread owner([&] { ran = bool(server->run()); });
    auto deadline = Clock::now() + 10s;
    while (calls.load() != 2) { CHECK(Clock::now() < deadline); std::this_thread::yield(); }
    send(b, {1}, 100); // Same caller-chosen number, separate source Link.
    auto other = receive(b);
    CHECK(other.metadata<ReplyHeader>().request == RequestId{1} && other.get<unsigned>() == 101);
    wait_destroyed(2); // A's first and B completed; A's unpublished second source still owned.
    auto first = receive(a);
    CHECK(first.metadata<ReplyHeader>().request == RequestId{1} && first.get<unsigned>() == 11);
    auto second = receive(a);
    CHECK(second.metadata<ReplyHeader>().request == RequestId{2} && second.get<unsigned>() == 21);
    wait_destroyed(3);
    CHECK(calls == 3 && destroyed == 3); // Delivery retried without repeating the handler.
    ipc::testing::failSegmentAllocations(0);
    for (unsigned mode = 1; mode <= 5; ++mode)
    {
        send(a, {100 + mode}, 30, mode);
        auto result = receive(a);
        const auto header = result.metadata<ReplyHeader>();
        CHECK(header.request == RequestId{100 + mode});
        if (mode == 3) CHECK(header.outcome == ECommandOutcome::SUCCESS && result.get<unsigned>() == 31);
        else if (mode == 2 || mode == 5)
            CHECK(header.outcome == ECommandOutcome::CANCELLED && !result.get<RenderError>().type);
        else CHECK(header.outcome == ECommandOutcome::FAILED && result.get<RenderError>().type == kRuntimeState);
    }
    // A valid but stale CommandId is rejected before typed payload access.
    ipc::IpcMessage stale;
    stale.emplace<Ack>();
    stale.setMetadata(RequestHeader{RequestId{999}, CommandId{command.index, command.gen + 1}});
    CHECK(a.trySend(stale));
    auto rejected = receive(a);
    CHECK(rejected.metadata<ReplyHeader>().outcome == ECommandOutcome::FAILED);
    CHECK(rejected.get<RenderError>().type == kRuntimeIdentity && calls == 8);
    server->stop(); owner.join();
    CHECK(ran && server->stats().validation_errors == 0 && destroyed == 8);
    server.reset();
    CHECK(a.tryReceive(stale) == ipc::EIpcReceive::CLOSED);
    CHECK(first.get<unsigned>() == 11 && other.get<unsigned>() == 101);
    std::puts("Raw Commands: isolated caller IDs, real reply-growth OOM retry once, sync/async terminal, late owned results");
}
