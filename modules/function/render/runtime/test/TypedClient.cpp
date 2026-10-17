#include "ServerFixture.hpp"
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/runtime/Commands.hpp>
#include <lux/engine/render/runtime/CommandBinding.hpp>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include "TestHooks.hpp"
#include "Allocation.hpp"
#include <array>
#include <fstream>
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
    using Response = RenderResult<unsigned>;
};
struct Small final
{
    static constexpr std::string_view name = "test.raw.small";
    struct Request final { unsigned sample; };
    struct Value final { unsigned sample; unsigned value; };
    using Response = RenderResult<Value>;
};
struct SmallSample final
{
    std::int64_t submit_begin, submit_end, handler_begin, handler_end, publication_begin, publication_end;
    std::int64_t receive_begin, receive_end;
};
static std::array<SmallSample, 1020> small_samples{};
static std::int64_t now() noexcept
{ return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }
namespace lux::render::runtime_detail::test
{
    void* beforeResponsePublication(std::uint64_t, const ipc::IpcMessage& message) noexcept
    {
        if (message.typeHash() != lux::cxx::type_hash<Small::Response>()) return nullptr;
        const auto& result = message.get<Small::Response>();
        CHECK(result && result->sample < small_samples.size());
        auto& sample = small_samples[result->sample];
        sample.publication_begin = now();
        return &sample;
    }
    void afterResponsePublication(void* value) noexcept
    { if (value) static_cast<SmallSample*>(value)->publication_end = now(); }
}
struct Handler final
{
    static constexpr std::string_view name = "test.raw.handler";
    std::atomic<unsigned>& calls;
    std::atomic<const unsigned*>& observed;
    Handler(std::atomic<unsigned>& c, std::atomic<const unsigned*>& o) noexcept : calls(c), observed(o) {}
    void work(const Work::Request& input, Work::Response& response) noexcept
    {
        ++calls;
        observed = input.source->values.data();
        const auto result = input.source->values.front() + 1;
        switch (input.mode)
        {
        case 0: response = result; break;
        case 1: response = lux::cxx::unexpected(RenderError{kRuntimeState, {17}}); break;
        case 2: response = lux::cxx::unexpected(RenderError{kCommandCancelled}); break;
        default: std::abort();
        }
    }
    void small(const Small::Request& input, Small::Response& response) noexcept
    {
        auto& sample = small_samples[input.sample];
        sample.handler_begin = now();
        response.emplace(Small::Value{input.sample, input.sample + 1});
        sample.handler_end = now();
    }
    using Bindings = std::tuple<CommandMethod<Work, &Handler::work>, CommandMethod<Small, &Handler::small>>;
};

static ipc::IpcMessage receive(ipc::IpcRequester& peer, std::uint64_t expected)
{
    const auto deadline = Clock::now() + 10s;
    ipc::IpcMessage message;
    for (;;)
    {
        std::uint64_t correlation = 0;
        const auto status = peer.tryReceive(correlation, message);
        CHECK(status != ipc::EIpcReceive::CLOSED && Clock::now() < deadline);
        if (status == ipc::EIpcReceive::RECEIVED) { CHECK(correlation == expected); return message; }
        std::this_thread::yield();
    }
}

int main()
{
    std::atomic<unsigned> calls{0}, destroyed{0};
    std::atomic<const unsigned*> observed{nullptr};
    auto server = take(createTestServer({"runtime.raw", true}));
    (void)take(server->emplaceHandler<Handler>("main", calls, observed));
    const auto command = take(server->commandId<Work>());
    const auto small_command = take(server->commandId<Small>());
    auto a = take(ipc::IpcCore::instance().createRequester("runtime.raw", {256, 1}));
    auto b = take(ipc::IpcCore::instance().createRequester("runtime.raw", {256, 1}));
    const auto send = [&](ipc::IpcRequester& peer, RequestId id, unsigned value, unsigned mode = 0) {
        ipc::IpcMessage message;
        message.emplace<Work::Request>(Work::Request{std::make_unique<Source>(destroyed, value), mode});
        CHECK(peer.trySend(commandTag(command), id.value, message));
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
    std::thread owner([&] { render_allocation::participate = true; ran = bool(server->run()); });
    auto deadline = Clock::now() + 10s;
    while (calls.load() != 2) { CHECK(Clock::now() < deadline); std::this_thread::yield(); }
    send(b, {1}, 100); // Same caller-chosen number, separate source Link.
    auto other = receive(b, 1);
    CHECK(other.get<Work::Response>() == 101);
    wait_destroyed(3); // Outbox retains only responses; all three synchronous inputs are already released.
    auto first = receive(a, 1);
    CHECK(first.get<Work::Response>() == 11);
    auto second = receive(a, 2);
    CHECK(second.get<Work::Response>() == 21);
    wait_destroyed(3);
    CHECK(calls == 3 && destroyed == 3); // Delivery retried without repeating the handler.
    ipc::testing::failSegmentAllocations(0);
    for (unsigned mode = 1; mode <= 2; ++mode)
    {
        send(a, {100 + mode}, 30, mode);
        auto result = receive(a, 100 + mode);
        const auto& response = result.get<Work::Response>();
        CHECK(!response);
        CHECK(response.error().type == (mode == 2 ? kCommandCancelled : kRuntimeState));
    }
    // A valid but stale CommandId is rejected before typed payload access.
    ipc::IpcMessage stale;
    stale.emplace<Ack>();
    CHECK(a.trySend(commandTag(CommandId{command.index, command.gen + 1}), 999, stale));
    auto rejected = receive(a, 999);
    CHECK(rejected.get<RenderError>().type == kProtocolContract && calls == 5);
    render_allocation::participate = true;
    for (unsigned i = 0; i < small_samples.size(); ++i)
    {
        if (i == 20)
        {
            render_allocation::calls = 0;
            render_allocation::window = true;
            ipc::testing::startAllocations();
        }
        auto& sample = small_samples[i];
        sample.submit_begin = now();
        ipc::IpcMessage input;
        input.emplace<Small::Request>(Small::Request{i});
        CHECK(a.trySend(commandTag(small_command), 2000 + i, input));
        sample.submit_end = now();
        const auto deadline = Clock::now() + 5s;
        ipc::IpcMessage output;
        for (;;)
        {
            std::uint64_t correlation = 0;
            sample.receive_begin = now();
            const auto status = a.tryReceive(correlation, output);
            if (status == ipc::EIpcReceive::RECEIVED)
            {
                CHECK(correlation == 2000 + i);
                const auto& result = output.get<Small::Response>();
                CHECK(result && result->value == i + 1);
                sample.receive_end = now();
                break;
            }
            CHECK(status != ipc::EIpcReceive::CLOSED && Clock::now() < deadline);
            std::this_thread::yield(); // Client scheduling is measured separately from the successful receive call.
        }
    }
    render_allocation::window = false;
    const auto dll_allocations = ipc::testing::stopAllocations();
    CHECK(render_allocation::calls == 0 && dll_allocations == 0);
    server->stop(); owner.join();
    std::ofstream csv("runtime-small-command.csv");
    csv << "sample,submit_begin,submit_end,handler_begin,handler_end,publication_begin,publication_end,receive_begin,receive_end\n";
    for (unsigned i = 20; i < small_samples.size(); ++i)
    {
        const auto& x = small_samples[i];
        CHECK(x.submit_begin <= x.handler_begin && x.handler_begin <= x.handler_end &&
            x.handler_end <= x.publication_begin && x.publication_begin <= x.publication_end &&
            x.publication_begin <= x.receive_end);
        csv << i << ',' << x.submit_begin << ',' << x.submit_end << ',' << x.handler_begin << ',' << x.handler_end
            << ',' << x.publication_begin << ',' << x.publication_end << ',' << x.receive_begin << ',' << x.receive_end << '\n';
    }
    std::printf("1000 complete small Commands: EXE new=%zu IPC DLL allocations=%zu; publish bounds include producer tail\n",
        render_allocation::calls.load(), dll_allocations);
    CHECK(ran && server->stats().validation_errors == 0 && destroyed == 5);
    server.reset();
    std::uint64_t correlation = 0;
    CHECK(a.tryReceive(correlation, stale) == ipc::EIpcReceive::CLOSED);
    CHECK(first.get<Work::Response>() == 11 && other.get<Work::Response>() == 101);
    std::puts("Raw Commands: isolated caller IDs, real reply-growth OOM retry once, sync/async terminal, late owned results");
}
