#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/runtime/RenderClient.hpp>
#include <lux/engine/render/runtime/Commands.hpp>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <thread>

using namespace lux::render;
namespace cxx = lux::cxx;
using namespace std::chrono_literals;

static void check(bool value, std::source_location at = std::source_location::current())
{
    if (!value)
    {
        std::fprintf(stderr, "handler ownership failure at %u\n", at.line());
        std::abort();
    }
}

template <class T> static T take(RenderResult<T> result)
{
    check(bool(result));
    return std::move(*result);
}

struct First
{
    static constexpr std::string_view name = "test.owner.first";
    using Request = unsigned;
    using Response = unsigned;
};
struct Second final : First { static constexpr std::string_view name = "test.owner.second"; };
struct Third final : First { static constexpr std::string_view name = "test.owner.third"; };
struct Fourth final : First { static constexpr std::string_view name = "test.owner.fourth"; };
struct NewBeforeFailure final : First { static constexpr std::string_view name = "test.owner.rollback"; };
struct Mismatch final
{
    static constexpr auto name = First::name;
    using Request = std::uint64_t;
    using Response = unsigned;
};

struct Counts final { std::atomic<unsigned> created{0}, destroyed{0}, calls{0}; };

class Handler final
{
public:
    static constexpr std::string_view name = "test.owner.handler";
    Handler(Counts& counts, unsigned offset) noexcept : counts_(counts), offset_(offset) { ++counts_.created; }
    ~Handler() noexcept { ++counts_.destroyed; }

    void first(const unsigned& input, Reply<First> reply) noexcept { answer(input, std::move(reply)); }
    void second(const unsigned& input, Reply<Second> reply) noexcept { answer(input, std::move(reply)); }
    void third(const unsigned& input, Reply<Third> reply) noexcept { answer(input, std::move(reply)); }
    void fourth(const unsigned& input, Reply<Fourth> reply) noexcept { answer(input, std::move(reply)); }

    using Bindings = std::tuple<CommandMethod<First, &Handler::first>, CommandMethod<Second, &Handler::second>,
        CommandMethod<Third, &Handler::third>, CommandMethod<Fourth, &Handler::fourth>>;

private:
    template <RenderCommand C> void answer(unsigned value, Reply<C> reply) noexcept
    {
        ++counts_.calls;
        check(bool(reply.complete(value + offset_)));
    }
    Counts& counts_;
    unsigned offset_;
};

class InvalidHandler final
{
public:
    static constexpr std::string_view name = "test.owner.invalid";
    explicit InvalidHandler(Counts& counts) noexcept : counts_(counts) { ++counts_.created; }
    ~InvalidHandler() noexcept { ++counts_.destroyed; }
    void added(const unsigned&, Reply<NewBeforeFailure>) noexcept { std::abort(); }
    void mismatch(const std::uint64_t&, Reply<Mismatch>) noexcept { std::abort(); }
    using Bindings = std::tuple<CommandMethod<NewBeforeFailure, &InvalidHandler::added>,
        CommandMethod<Mismatch, &InvalidHandler::mismatch>>;
private:
    Counts& counts_;
};

static RenderCompletion receive(RenderClient& client)
{
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    for (;;)
    {
        RenderCompletion output;
        const auto status = take(client.tryReceive(output));
        if (status == ERenderReceive::RECEIVED) return output;
        check(status != ERenderReceive::CLOSED && std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}

template <class C> static TypedRenderCompletion<C> call(RenderClient& client, typename C::Request input = {})
{
    auto binding = take(client.bind<C>());
    (void)take(client.submit(binding, input, 16384));
    return take(receive(client).take(binding));
}

int main()
{
    Counts counts, rejected;
    auto server = take(RenderServer::create({"runtime.handlers", true}));
    auto first = take(server->emplaceHandler<Handler>("A", {}, counts, 100));
    auto duplicate = take(server->emplaceHandler<Handler>("A", {}, counts, 999));
    auto second = take(server->emplaceHandler<Handler>("B", {}, counts, 200));
    check(first == duplicate && first != second && counts.created == 2 && counts.destroyed == 0);
    auto invalid = server->emplaceHandler<InvalidHandler>("bad", {}, rejected);
    check(!invalid && invalid.error().type == kProtocolContract);
    check(rejected.created == 1 && rejected.destroyed == 1);

    RenderResult<void> run;
    std::thread thread([&] { run = server->run(); });
    auto a = take(RenderClient::create("runtime.handlers"));
    auto b = take(RenderClient::create("runtime.handlers"));
    (void)take(a->hello());
    (void)take(b->hello());
    check(receive(*a).header.outcome == ECommandOutcome::SUCCESS);
    check(receive(*b).header.outcome == ECommandOutcome::SUCCESS);
    const auto bound = take(a->bind<First>());
    const auto bindings = call<ListCommandBindings>(*a, bound.command());
    check(bindings.response().selected == first && bindings.response().handlers.size() == 2);
    check(call<First>(*a, 7).response() == 107);
    check(call<Second>(*a, 8).response() == 108);
    check(call<Third>(*a, 9).response() == 109);
    check(call<Fourth>(*a, 10).response() == 110);
    check(!a->bind<NewBeforeFailure>()); // Failed installation published neither its first command nor supports.

    auto select = take(a->bind<SelectCommandBinding>());
    CommandSelection selection{bound.command(), second};
    unsigned input = 11;
    (void)take(a->submit(select, selection));
    (void)take(a->submit(bound, input)); // FIFO selection takes effect at dispatch.
    check(take(receive(*a).take(select)).header().outcome == ECommandOutcome::SUCCESS);
    check(take(receive(*a).take(bound)).response() == 211);
    check(call<First>(*b, 12).response() == 212); // Other Client coordinates after the selection reply.
    auto bad = call<SelectCommandBinding>(*a, {bound.command(), HandlerId{}});
    check(bad.header().outcome == ECommandOutcome::FAILED && bad.header().error.type == kRuntimeIdentity);
    check(call<First>(*a, 13).response() == 213);
    check(call<Second>(*a, 14).response() == 114); // Selection is per command, not per object.
    const auto commands = call<ListCommands>(*a);
    unsigned selected_count = 0;
    for (const auto& command : commands.response().commands)
    {
        check(command.name != NewBeforeFailure::name);
        if (command.id == bound.command()) { check(command.bound); ++selected_count; }
    }
    check(selected_count == 1 && counts.destroyed == 0);
    server->stop();
    thread.join();
    check(bool(run) && server->stats().validation_errors == 0 && counts.destroyed == 2);
    server.reset();
    check(counts.destroyed == 2 && bindings.response().handlers[0].instance == "A");
    std::puts("Handler ownership: four members, two objects, idempotency, rollback, FIFO selection, exact destruction PASS");
}
