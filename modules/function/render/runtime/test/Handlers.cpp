#include "Discovery.hpp"
#include <lux/engine/render/runtime/RenderServer.hpp>
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

template <class C> static lux::platform::ipc::IpcMessage call(runtime_test::Peer& client,
    typename C::Request input = {})
{
    const auto binding = take(runtime_test::find<C>(client));
    return runtime_test::wait(client, take(runtime_test::send<C>(client, binding, std::move(input))));
}

int main()
{
    Counts counts, rejected, late_counts;
    auto server = take(RenderServer::create({"runtime.handlers", true}));
    const auto directory_id = take(server->commandId<ListCommands>());
    check(!directory_id.isNull());
    check(!server->commandId<First>());
    auto first = take(server->emplaceHandler<Handler>("A", {}, counts, 100));
    auto duplicate = take(server->emplaceHandler<Handler>("A", {}, counts, 999));
    auto second = take(server->emplaceHandler<Handler>("B", {}, counts, 200));
    check(first == duplicate && first != second && counts.created == 2 && counts.destroyed == 0);
    const auto first_command_id = take(server->commandId<First>());
    check(server->commandId<Mismatch>().error().type == kProtocolContract);
    auto invalid = server->emplaceHandler<InvalidHandler>("bad", {}, rejected);
    check(!invalid && invalid.error().type == kProtocolContract);
    check(rejected.created == 1 && rejected.destroyed == 1);
    (void)take(server->emplaceHandler<Installer>("installer", {}, *server, late_counts));

    RenderResult<void> run;
    std::thread thread([&] { run = server->run(); });
    auto a = take(runtime_test::connect("runtime.handlers", directory_id));
    check(!runtime_test::find<First>(*a)); // The application has not queried the catalog yet.
    check(bool(runtime_test::refresh(*a)));
    const auto controls = take(runtime_test::find<ListCommands>(*a));
    check(controls == directory_id);
    auto protected_control = call<SelectCommandBinding>(*a, {controls, {}});
    check(protected_control.metadata<ReplyHeader>().outcome == ECommandOutcome::FAILED &&
        protected_control.get<RenderError>().type == kRuntimeUnsupported);
    check(!call<ListCommands>(*a).get<ListCommands::Response>().commands.empty());
    const auto bound = take(runtime_test::find<First>(*a));
    check(bound == first_command_id);
    const auto bindings = call<ListCommandBindings>(*a, bound);
    check(bindings.get<CommandBindings>().selected == first && bindings.get<CommandBindings>().handlers.size() == 2);
    check(call<First>(*a, 7).get<First::Response>() == 107);
    check(call<Second>(*a, 8).get<Second::Response>() == 108);
    check(call<Third>(*a, 9).get<Third::Response>() == 109);
    check(call<Fourth>(*a, 10).get<Fourth::Response>() == 110);
    check(!runtime_test::find<NewBeforeFailure>(*a)); // Failed installation published neither its first command nor supports.

    auto select = take(runtime_test::find<SelectCommandBinding>(*a));
    CommandSelection selection{bound, second};
    unsigned input = 11;
    const auto select_id = take(runtime_test::send<SelectCommandBinding>(*a, select, selection));
    const auto first_id = take(runtime_test::send<First>(*a, bound, input));
    check(runtime_test::wait(*a, select_id).metadata<ReplyHeader>().outcome == ECommandOutcome::SUCCESS);
    check(runtime_test::wait(*a, first_id).get<First::Response>() == 211);

    check(call<SelectCommandBinding>(*a, {bound, {}}).metadata<ReplyHeader>().outcome == ECommandOutcome::SUCCESS);
    auto unbound = call<First>(*a, 0);
    check(unbound.metadata<ReplyHeader>().outcome == ECommandOutcome::FAILED &&
        unbound.get<RenderError>().type == kRuntimeUnsupported);
    const auto late = call<InstallLate>(*a).get<InstallLate::Response>();
    check(late_counts.created == 1 && call<InstallLate>(*a).get<InstallLate::Response>() == late && late_counts.created == 1);
    check(!runtime_test::find<Late>(*a)); // Existing Client discovery changes only after explicit adoption.
    unbound = call<First>(*a, 0);
    check(unbound.metadata<ReplyHeader>().outcome == ECommandOutcome::FAILED &&
        unbound.get<RenderError>().type == kRuntimeUnsupported); // A new supporter does not steal an unbound command.
    check(bool(runtime_test::refresh(*a)) && call<Late>(*a, 9).get<Late::Response>() == 309);
    check(call<ListCommandBindings>(*a, bound).get<ListCommandBindings::Response>().handlers.size() == 3);
    check(call<SelectCommandBinding>(*a, {bound, second}).metadata<ReplyHeader>().outcome == ECommandOutcome::SUCCESS);

    // A later Client explicitly discovers the live table, not a startup-only snapshot.
    auto b = take(runtime_test::connect("runtime.handlers", directory_id));
    check(!runtime_test::find<Late>(*b));
    check(bool(runtime_test::refresh(*b)));
    check(call<Late>(*b, 10).get<Late::Response>() == 310);
    check(call<First>(*b, 12).get<First::Response>() == 212); // Other Client coordinates after the selection reply.
    auto bad = call<SelectCommandBinding>(*a, {bound, HandlerId{first.index, first.gen + 1}});
    check(bad.metadata<ReplyHeader>().outcome == ECommandOutcome::FAILED && bad.get<RenderError>().type == kRuntimeIdentity);
    check(call<First>(*a, 13).get<First::Response>() == 213);
    check(call<Second>(*a, 14).get<Second::Response>() == 114); // Selection is per command, not per object.
    const auto commands = call<ListCommands>(*a);
    unsigned selected_count = 0;
    for (const auto& command : commands.get<CommandList>().commands)
    {
        check(command.name != NewBeforeFailure::name);
        if (command.id == bound) { check(command.bound); ++selected_count; }
    }
    check(selected_count == 1 && counts.destroyed == 0);
    server->stop();
    thread.join();
    check(bool(run) && server->stats().validation_errors == 0 && counts.destroyed == 2);
    check(late_counts.destroyed == 1);
    server.reset();
    check(counts.destroyed == 2 && bindings.get<CommandBindings>().handlers[0].instance == "A");
    std::puts("Handler ownership: exact destruction, rollback, FIFO selection, unbind, dynamic install/refresh and late catalog discovery PASS");
}
