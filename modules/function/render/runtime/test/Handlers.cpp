#include "Discovery.hpp"
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
struct Late final : First { static constexpr std::string_view name = "test.owner.late"; };
struct InstallLate final
{
    static constexpr std::string_view name = "test.owner.install";
    using Request = Ack;
    using Response = HandlerId;
};
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

class LateHandler final
{
public:
    static constexpr std::string_view name = "test.owner.late-handler";
    explicit LateHandler(Counts& counts) noexcept : counts_(counts) { ++counts_.created; }
    ~LateHandler() noexcept { ++counts_.destroyed; }
    void first(const unsigned& input, Reply<First> reply) noexcept { check(bool(reply.complete(input + 300))); }
    void late(const unsigned& input, Reply<Late> reply) noexcept { check(bool(reply.complete(input + 300))); }
    using Bindings = std::tuple<CommandMethod<First, &LateHandler::first>, CommandMethod<Late, &LateHandler::late>>;
private:
    Counts& counts_;
};

class Installer final
{
public:
    static constexpr std::string_view name = "test.owner.installer";
    Installer(RenderServer& server, Counts& counts) noexcept : server_(server), counts_(counts) {}
    void install(const Ack&, Reply<InstallLate> reply) noexcept
    {
        // Actual Owner-thread dispatch is the directory mutation safe point, not a concurrent caller.
        auto installed = server_.emplaceHandler<LateHandler>("late", {}, counts_);
        if (!installed) check(bool(reply.fail(installed.error())));
        else check(bool(reply.complete(*installed)));
    }
    using Bindings = std::tuple<CommandMethod<InstallLate, &Installer::install>>;
private:
    RenderServer& server_;
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
    Counts counts, rejected, late_counts;
    auto server = take(RenderServer::create({"runtime.handlers", true}));
    auto first = take(server->emplaceHandler<Handler>("A", {}, counts, 100));
    auto duplicate = take(server->emplaceHandler<Handler>("A", {}, counts, 999));
    auto second = take(server->emplaceHandler<Handler>("B", {}, counts, 200));
    check(first == duplicate && first != second && counts.created == 2 && counts.destroyed == 0);
    auto invalid = server->emplaceHandler<InvalidHandler>("bad", {}, rejected);
    check(!invalid && invalid.error().type == kProtocolContract);
    check(rejected.created == 1 && rejected.destroyed == 1);
    (void)take(server->emplaceHandler<Installer>("installer", {}, *server, late_counts));

    RenderResult<void> run;
    std::thread thread([&] { run = server->run(); });
    auto a = take(RenderClient::create("runtime.handlers"));
    (void)take(a->hello());
    check(receive(*a).header.outcome == ECommandOutcome::SUCCESS);
    check(!a->bind<First>()); // Hello exposes controls, never the business directory.
    check(bool(runtime_test::refresh(*a)));
    const auto controls = take(a->bind<ListCommands>());
    auto protected_control = call<SelectCommandBinding>(*a, {controls.command(), {}});
    check(protected_control.header().outcome == ECommandOutcome::FAILED &&
        protected_control.header().error.type == kRuntimeUnsupported);
    check(!call<ListCommands>(*a).response().commands.empty());
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

    check(call<SelectCommandBinding>(*a, {bound.command(), {}}).header().outcome == ECommandOutcome::SUCCESS);
    auto unbound = call<First>(*a, 0);
    check(unbound.header().outcome == ECommandOutcome::FAILED &&
        unbound.header().error.type == kRuntimeUnsupported);
    const auto late = call<InstallLate>(*a).response();
    check(late_counts.created == 1 && call<InstallLate>(*a).response() == late && late_counts.created == 1);
    check(!a->bind<Late>()); // Existing Client discovery changes only after explicit adoption.
    unbound = call<First>(*a, 0);
    check(unbound.header().outcome == ECommandOutcome::FAILED &&
        unbound.header().error.type == kRuntimeUnsupported); // A new supporter does not steal an unbound command.
    const auto list_binding = take(a->bind<ListCommands>());
    ListCommands::Request empty;
    (void)take(a->submit(list_binding, empty, 16384));
    auto directory = receive(*a);
    directory.session[0] ^= 1;
    auto rejected_directory = a->refreshCommands(directory);
    check(!rejected_directory && rejected_directory.error().type == kRuntimeIdentity && !a->bind<Late>());
    directory.session[0] ^= 1;
    check(bool(a->refreshCommands(directory)) && call<Late>(*a, 9).response() == 309);
    check(call<ListCommandBindings>(*a, bound.command()).response().handlers.size() == 3);
    check(call<SelectCommandBinding>(*a, {bound.command(), second}).header().outcome == ECommandOutcome::SUCCESS);

    // A later Client explicitly discovers the live table, not a startup snapshot in Hello.
    auto b = take(RenderClient::create("runtime.handlers"));
    (void)take(b->hello());
    check(receive(*b).header.outcome == ECommandOutcome::SUCCESS);
    check(!b->bind<Late>());
    check(bool(runtime_test::refresh(*b)));
    check(call<Late>(*b, 10).response() == 310);
    check(call<First>(*b, 12).response() == 212); // Other Client coordinates after the selection reply.
    auto bad = call<SelectCommandBinding>(*a, {bound.command(), HandlerId{first.index, first.gen + 1}});
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
    check(late_counts.destroyed == 1);
    server.reset();
    check(counts.destroyed == 2 && bindings.response().handlers[0].instance == "A");
    std::puts("Handler ownership: exact destruction, rollback, FIFO selection, unbind, dynamic install/refresh and late Hello PASS");
}
