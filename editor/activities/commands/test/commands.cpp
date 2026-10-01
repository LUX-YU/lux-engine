#include <lux/engine/editor/commands/CommandRegistry.hpp>
#include <cassert>
#include <cstdio>
#include <thread>

namespace cxx = lux::cxx;
using namespace lux::editor;
using namespace lux::editor::commands;
namespace
{
    CommandDescriptor descriptor()
    {
        return {CommandId{"test.command"}, "Command", "Edit", "Ctrl+S"};
    }
    auto entry(int& calls)
    {
        return std::make_shared<CommandEntry>(
            contracts::CodeLease::builtin(),
            descriptor(),
            [](const CommandQuery&) -> CommandResult<CommandState> { return CommandState{true}; },
            [&calls](const CommandInvocation&) -> CommandResult<DispatchReceipt> {
                ++calls;
                return DispatchReceipt{ImmediateCompletion{}};
            }
        );
    }
    void pinnedAndCurrent()
    {
        int old_calls{}, new_calls{};
        CommandRegistry registry;
        CommandDispatcher dispatcher{registry, 2};
        auto first = CommandRegistrySnapshot::create({entry(old_calls)});
        assert(first && registry.publish(*first));
        auto handle = first->find(CommandIdView{"test.command"});
        assert(handle);
        CommandInvocation pinned;
        CommandInvocation current{{}, {}, ERegistryBinding::CURRENT_REGISTRATION};
        assert(dispatcher.enqueue(*handle, pinned));
        assert(dispatcher.enqueue(*handle, current));
        CommandInvocation refused;
        auto full = dispatcher.enqueue(*handle, refused);
        assert(!full && full.error().code == ECommandError::CAPACITY);
        auto second = CommandRegistrySnapshot::create({entry(new_calls)});
        assert(second && registry.publish(*second));
        auto completed = dispatcher.drain();
        assert(completed && completed->size() == 2 && dispatcher.pending() == 0);
        assert(old_calls == 1 && new_calls == 1);
        assert((*completed)[0].result && (*completed)[1].result);

        auto incompatible = descriptor();
        incompatible.input_version = 2;
        auto changed = std::make_shared<CommandEntry>(
            contracts::CodeLease::builtin(),
            incompatible,
            [](const CommandQuery&) -> CommandResult<CommandState> { return CommandState{true}; },
            [](const CommandInvocation&) -> CommandResult<DispatchReceipt> {
                return DispatchReceipt{ImmediateCompletion{}};
            }
        );
        CommandInvocation queued{{}, {}, ERegistryBinding::CURRENT_REGISTRATION};
        assert(dispatcher.enqueue(*handle, queued));
        assert(registry.publish(*CommandRegistrySnapshot::create({changed})));
        auto rejected = dispatcher.drain();
        assert(rejected && rejected->size() == 1 && !rejected->front().result);
        assert(rejected->front().result.error().code == ECommandError::INCOMPATIBLE_REGISTRATION);
    }
    void queryLifetime()
    {
        CommandRegistry registry;
        CommandRegistrySnapshot snapshot;
        std::optional<CommandHandle> external;
        bool released{}, active{}, early{}, nested_busy{}, publication_busy{};
        struct Witness final
        {
            bool& released;
            bool& active;
            bool& early;
            ~Witness()
            {
                released = true;
                early = active;
            }
        };
        auto code = std::make_shared<Witness>(released, active, early);
        auto record = std::make_shared<CommandEntry>(
            contracts::CodeLease::plugin(code),
            descriptor(),
            [&](const CommandQuery& query) -> CommandResult<CommandState> {
                active = true;
                auto nested = registry.query(*external, query);
                nested_busy = !nested && nested.error().code == ECommandError::BUSY;
                auto replacement = registry.publish({});
                publication_busy = !replacement && replacement.error().code == ECommandError::BUSY;
                external.reset();
                snapshot = {};
                assert(!released);
                active = false;
                return CommandState{true};
            },
            [](const CommandInvocation&) -> CommandResult<DispatchReceipt> {
                return DispatchReceipt{ImmediateCompletion{}};
            }
        );
        snapshot = *CommandRegistrySnapshot::create({record});
        external = *snapshot.find(CommandIdView{"test.command"});
        record.reset();
        code.reset();
        CommandInvocation input;
        assert(registry.query(*external, input.query()));
        assert(released && !early && nested_busy && publication_busy);
    }
    void repeatedSnapshotOwnership()
    {
        int calls{};
        auto original = entry(calls);
        auto snapshot = *CommandRegistrySnapshot::create({original});
        for (std::size_t index{}; index < 10000; ++index)
        {
            std::weak_ptr<CommandEntry> previous = snapshot.entries().front();
            auto next = CommandRegistrySnapshot::create({snapshot.entries().front()});
            assert(next && next->entries().front().get() == original.get());
            snapshot = std::move(*next);
            // A shared entry is not a reason to retain every previous receiving-module wrapper.
            assert(previous.expired());
        }
        CommandRegistry registry;
        CommandInvocation input;
        assert(registry.query(*snapshot.find(CommandIdView{"test.command"}), input.query()));
        std::puts("PASS 10000 shared-entry revisions retain one value without an owner chain");
    }
    void busyAndReentry()
    {
        CommandRegistry registry;
        CommandDispatcher dispatcher{registry};
        bool busy{true};
        int calls{};
        auto record = std::make_shared<CommandEntry>(
            contracts::CodeLease::builtin(),
            descriptor(),
            [&](const CommandQuery&) -> CommandResult<CommandState> {
                if (busy)
                    return cxx::unexpected(CommandFailure{ECommandError::BUSY, "real.domain", 29, "reading"});
                return CommandState{true};
            },
            [&](const CommandInvocation&) -> CommandResult<DispatchReceipt> {
                ++calls;
                auto nested = dispatcher.drain();
                assert(!nested && nested.error().code == ECommandError::BUSY);
                return DispatchReceipt{AcceptedOperation{"save", 17}};
            }
        );
        auto snapshot = CommandRegistrySnapshot::create({record});
        assert(snapshot);
        auto handle = snapshot->find(CommandIdView{"test.command"});
        CommandInvocation input;
        assert(dispatcher.enqueue(*handle, input));
        auto first = dispatcher.drain();
        assert(first && first->empty() && dispatcher.pending() == 1 && calls == 0);
        busy = false;
        auto second = dispatcher.drain();
        assert(second && second->size() == 1 && calls == 1 && dispatcher.pending() == 0);
        assert(std::get<AcceptedOperation>(*second->front().result).value == 17);
        std::thread foreign([&] {
            auto result = registry.query(*handle, input.query());
            assert(!result && result.error().code == ECommandError::WRONG_THREAD);
        });
        foreign.join();
    }
}
int main()
{
    pinnedAndCurrent();
    queryLifetime();
    repeatedSnapshotOwnership();
    busyAndReentry();
    std::puts("PASS immutable command entries, pinned/current policy, bounded BUSY FIFO, recursive and foreign calls");
}
