#include <lux/engine/editor/commands/CommandRegistry.hpp>
#include <cassert>
#include <cstdio>

using namespace lux::editor::commands;
using lux::editor::contracts::CodeLease;
namespace
{
    constexpr CommandDescriptor declaration{CommandIdView{"ec3.command"}, "Command", "Tests", "Ctrl+T"};
    CommandDescriptor mutable_declaration{CommandIdView{"ec3.mutable"}, "Mutable"};
    auto query(const CommandQuery&) -> CommandResult<CommandState> { return CommandState{true}; }
    auto execute(const CommandInvocation&) -> CommandResult<DispatchReceipt>
    {
        return DispatchReceipt{ImmediateCompletion{}};
    }
}
int main()
{
#if EC3_INVALID_DECLARATION == 1
    const CommandDescriptor temporary{CommandIdView{"ec3.local"}, "Local"};
    auto rejected = CommandEntry::bind<temporary>(CodeLease::builtin(), query, execute);
#elif EC3_INVALID_DECLARATION == 2
    auto rejected = CommandEntry::bind<mutable_declaration>(CodeLease::builtin(), query, execute);
#elif EC3_INVALID_DECLARATION == 3
    auto rejected = std::make_shared<CommandEntry>(CodeLease::builtin(), declaration, query, execute);
#else
    auto fixed = CommandEntry::bind<declaration>(CodeLease::builtin(), query, execute);
    assert(&fixed->descriptor() == &declaration);
    assert(fixed->descriptor().label.data() == declaration.label.data());
    std::shared_ptr<CommandEntry> dynamic;
    {
        std::string name{"ec3.dynamic"}, label{"Small"}, group{"Group"}, shortcut{"Ctrl+D"}, argument{"argument"};
        const CommandDescriptor input{
            CommandIdView{name}, label, group, shortcut, ECommandScope::APPLICATION, 2, {7, argument}
        };
        dynamic = CommandEntry::create(CodeLease::builtin(), input, query, execute);
        name.assign(4096, 'x');
        label.clear();
        group.clear();
        shortcut.clear();
        argument.clear();
    }
    const auto& description = dynamic->descriptor();
    assert(description.id.name() == "ec3.dynamic" && description.label == "Small");
    assert(description.group == "Group" && description.shortcut == "Ctrl+D");
    assert(description.argument_type.name() == "argument" && description.input_version == 2);
    auto snapshot = CommandRegistrySnapshot::create({fixed, dynamic});
    assert(snapshot);
    auto handle = snapshot->find(declaration.id);
    assert(handle);
    CommandRegistry registry;
    assert(registry.publish(*snapshot));
    CommandInvocation invocation;
    assert(registry.execute(*handle, invocation));
    auto invalid = CommandEntry::create(CodeLease::plugin({}), declaration, query, execute);
    assert(!CommandRegistrySnapshot::create({invalid}));
    const auto duplicate = CommandRegistrySnapshot::create({fixed, fixed});
    assert(!duplicate && duplicate.error().code == ECommandError::DUPLICATE);
    assert(registry.snapshot().find(declaration.id));
    const auto copy = *handle;
    assert(registry.publish({}));
    fixed.reset();
    snapshot = CommandRegistrySnapshot{};
    assert(registry.execute(copy, invocation));
    assert(!registry.snapshot().resolve(copy));
    std::printf("PASS installed descriptor lifetime: Descriptor=%zu Entry=%zu Handle=%zu\n",
                sizeof(CommandDescriptor), sizeof(CommandEntry), sizeof(CommandHandle));
#endif
}
