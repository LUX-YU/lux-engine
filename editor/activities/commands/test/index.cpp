#include <lux/engine/editor/commands/CommandRegistry.hpp>
#include <lux/engine/editor/commands/CommandIndex.hpp>
#include <cassert>
#include <cstdio>

using namespace lux::editor;
using namespace lux::editor::commands;

int main()
{
    auto entry = [](std::string_view name) {
        return CommandEntry::create(contracts::CodeLease::builtin(), {CommandIdView{name}, name},
            [](const CommandQuery&) -> CommandResult<CommandState> { return CommandState{true}; },
            [](const CommandInvocation&) -> CommandResult<DispatchReceipt> {
                return DispatchReceipt{ImmediateCompletion{}};
            }
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
}
