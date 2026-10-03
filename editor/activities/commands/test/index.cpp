#include <lux/engine/editor/commands/CommandRegistry.hpp>
#include <lux/engine/editor/commands/CommandIndex.hpp>
#include <lux/engine/editor/commands/CommandIndexTestAccess.hpp>
#include <cassert>
#include <cstdio>

using namespace lux::editor;
using namespace lux::editor::commands;

int main()
{
    auto entry = [](std::string_view name)
    {
        return CommandEntry::create(
            contracts::CodeLease::builtin(),
            {CommandIdView{name}, name},
            [](const CommandQuery&) -> CommandResult<CommandState> { return CommandState{true}; },
            [](const CommandInvocation&) -> CommandResult<DispatchReceipt>
            { return DispatchReceipt{ImmediateCompletion{}}; }
        );
    };
    const auto first = entry("test.literal");
    const auto second = entry("test.dynamic");
    const auto snapshot = CommandRegistrySnapshot::create({first, second});
    assert(snapshot);
    const std::vector<std::shared_ptr<CommandEntry>> candidates{first, second};
    const auto collision = detail::commandIndex(candidates, [](CommandIdView) { return std::uint64_t{17}; });
    assert(!collision && collision.error().code == ECommandError::HASH_COLLISION);
    assert(snapshot->entries().size() == 2 && snapshot->find(first->descriptor().id));
    std::puts("PASS controlled collision uses production candidate-index algorithm; published snapshot unchanged");
    unsigned old_calls{}, changed_calls{};
    auto counted = [](std::string_view name, unsigned& calls)
    {
        return CommandEntry::create(
            contracts::CodeLease::builtin(),
            {CommandIdView{name}, name},
            [](const CommandQuery&) -> CommandResult<CommandState> { return CommandState{true}; },
            [&calls](const CommandInvocation&) -> CommandResult<DispatchReceipt>
            {
                ++calls;
                return DispatchReceipt{ImmediateCompletion{}};
            }
        );
    };
    auto original = CommandRegistrySnapshot::create({counted("old.identity", old_calls)});
    auto replacement = CommandRegistrySnapshot::create({counted("different.identity", changed_calls)});
    assert(original && replacement);
    const auto handle = original->at(0);
    assert(handle);
    auto collided = detail::CommandIndexTestAccess::withSingleHash(*replacement, handle->descriptor().id.hash());
    assert(collided);
    CommandRegistry registry;
    CommandDispatcher dispatcher{registry};
    assert(registry.publish(*original));
    const auto revision = registry.revision();
    CommandInvocation pinned;
    CommandInvocation current{{}, {}, ERegistryBinding::CURRENT_REGISTRATION};
    assert(dispatcher.enqueue(*handle, pinned) && dispatcher.enqueue(*handle, current));
    assert(registry.publish(*collided) && registry.revision() == revision + 1);
    assert(!collided->find(handle->descriptor().id));
    const auto completed = dispatcher.drain();
    assert(completed && completed->size() == 2 && (*completed)[0].result);
    assert(!(*completed)[1].result && (*completed)[1].result.error().code == ECommandError::NOT_FOUND);
    assert(old_calls == 1 && changed_calls == 0 && dispatcher.pending() == 0);
    std::puts("PASS controlled cross-version locator collision: actual PINNED callback runs; CURRENT rejects other name"
    );
}
