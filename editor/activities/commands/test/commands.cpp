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
        return {CommandIdView{"test.command"}, "Command", "Edit", "Ctrl+S"};
    }
    auto entry(int& calls)
    {
        return CommandEntry::create(
            contracts::CodeLease::builtin(),
            descriptor(),
            [](const CommandQuery&) -> CommandResult<CommandState> { return CommandState{true}; },
            [&calls](const CommandInvocation&) -> CommandResult<DispatchReceipt> {
                ++calls;
                return DispatchReceipt{ImmediateCompletion{}};
            }
        );
    }
    inline constexpr CommandDescriptor literal{CommandIdView{"test.literal"}, "Static label", "Edit", "Ctrl+L"};
    static_assert(literal.id.hash() == cxx::Fnv1a64::hash("test.literal"));
    static_assert(!std::is_constructible_v<CommandEntry, contracts::CodeLease, CommandDescriptor,
                                         CommandEntry::Query, CommandEntry::Execute>);
    void descriptorStorageAndIndex()
    {
        auto query = [](const CommandQuery&) -> CommandResult<CommandState> { return CommandState{true}; };
        auto execute = [](const CommandInvocation&) -> CommandResult<DispatchReceipt> {
            return DispatchReceipt{ImmediateCompletion{}};
        };
        auto fixed = CommandEntry::bind<literal>(contracts::CodeLease::builtin(), query, execute);
        assert(&fixed->descriptor() == &literal && fixed->descriptor().label.data() == literal.label.data());
        std::shared_ptr<CommandEntry> dynamic;
        {
            std::string name{"test.dynamic"}, label{"Label"}, group{"A/B"}, key{"Alt+M"}, argument{"payload"};
            dynamic = CommandEntry::create(contracts::CodeLease::builtin(),
                {CommandIdView{name}, label, group, key, ECommandScope::APPLICATION, 3, {91, argument}}, query, execute);
            name.assign(2000, 'n'); label.assign(2000, 'l'); group.clear(); key.clear(); argument.clear();
        }
        const auto& owned = dynamic->descriptor();
        assert(owned.id.name() == "test.dynamic" && owned.label == "Label" && owned.group == "A/B");
        assert(owned.shortcut == "Alt+M" && owned.argument_type.name() == "payload" && owned.input_version == 3);
        const auto snapshot = CommandRegistrySnapshot::create({fixed, dynamic});
        assert(snapshot && snapshot->entries()[0].get() == fixed.get());
        assert(!snapshot->at(2) && !CommandRegistrySnapshot{}.at(0));
        assert(&snapshot->at(0)->descriptor() == &literal);
        assert(owned.label.data()[owned.label.size()] == '\0');
        assert(owned.shortcut.data()[owned.shortcut.size()] == '\0');
        assert(snapshot->find(literal.id) && !snapshot->find(CommandIdView{"test.missing"}));
        auto handle = snapshot->find(literal.id);
        assert(handle && snapshot->resolve(*handle));
        assert(&snapshot->resolve(*handle)->descriptor() == &literal);
        const auto duplicate = CommandRegistrySnapshot::create({fixed, fixed});
        assert(!duplicate && duplicate.error().code == ECommandError::DUPLICATE);
        const auto invalid = CommandIdView::fromVerified("forged", 12);
        assert(!invalid.isValid() && !snapshot->find(invalid));
        std::puts("PASS static descriptor identity, one frozen dynamic backing, duplicate and public identity validation");
        std::printf("sizeof Descriptor=%zu Entry=%zu Handle=%zu\n", sizeof(CommandDescriptor), sizeof(CommandEntry), sizeof(CommandHandle));
    }
    void compoundScope()
    {
        static_assert(!std::is_copy_constructible_v<CommandRegistry::Batch>);
        static_assert(!std::is_copy_assignable_v<CommandRegistry::Batch>);
        static_assert(std::is_nothrow_move_constructible_v<CommandRegistry::Batch>);
        static_assert(!std::is_move_assignable_v<CommandRegistry::Batch>);
        int calls{};
        CommandRegistry registry;
        CommandDispatcher dispatcher{registry};
        auto candidate = CommandRegistrySnapshot::create({entry(calls)});
        assert(candidate && registry.publish(*candidate));
        auto handle = candidate->find(CommandIdView{"test.command"});
        assert(handle);
        CommandInvocation input;
        {
            auto acquired = registry.readBatch();
            assert(acquired);
            auto scope = std::move(*acquired);
            assert(!registry.readBatch() && !registry.preparePublication({}) && !registry.publish({}));
            assert(registry.query(*handle, input.query()) && registry.execute(*handle, input));
            assert(dispatcher.enqueue(*handle, input));
            const auto blocked = dispatcher.drain();
            assert(!blocked && blocked.error().code == ECommandError::BUSY && dispatcher.pending() == 1);
        }
        assert(registry.canPublish() && dispatcher.drain() && calls == 2);
        unsigned cleaned{};
        {
            auto code = contracts::CodeLease::plugin(std::shared_ptr<const void>(new int{1}, [&](const void* p) {
                ++cleaned;
                auto publish = registry.publish({});
                assert(!publish && publish.error().code == ECommandError::BUSY);
                delete static_cast<const int*>(p);
            }));
            auto prepared = CommandRegistrySnapshot::create({CommandEntry::create(
                std::move(code),
                descriptor(),
                [](const CommandQuery&) -> CommandResult<CommandState> { return CommandState{true}; },
                [](const CommandInvocation&) -> CommandResult<DispatchReceipt> {
                    return DispatchReceipt{ImmediateCompletion{}};
                }
            )});
            assert(prepared);
            auto batch = registry.preparePublication(std::move(*prepared));
            assert(batch && cleaned == 0);
        } // Abandoned candidate cleanup must stay protected, with no revision change.
        assert(cleaned == 1 && registry.revision() == 1 && registry.canPublish());
        {
            auto batch = registry.preparePublication({});
            assert(batch);
            auto old = batch->commit();
            assert(registry.revision() == 2 && old.entries().size() == 1 && !registry.publish({}));
        }
        assert(registry.canPublish());
        std::thread foreign([&] {
            const auto rejected = registry.readBatch();
            assert(!rejected && rejected.error().code == ECommandError::WRONG_THREAD);
        });
        foreign.join();
    }
    void shortcutAdmission()
    {
        using namespace lux::ui;
        assert(parseShortcut("")->key == EKey::NONE);
        assert((*parseShortcut("Ctrl+Shift+Alt+Z") == Shortcut{EKey::Z, true, true, true}));
        assert(parseShortcut("Delete")->key == EKey::DELETE_KEY);
        assert(parseShortcut("Enter")->key == EKey::ENTER);
        assert(parseShortcut("Escape")->key == EKey::ESCAPE);
        assert(parseShortcut("Ctrl+Ctrl+A").error() == EShortcutError::DUPLICATE_MODIFIER);
        assert(parseShortcut("Shift+Ctrl+A").error() == EShortcutError::MODIFIER_ORDER);
        assert(parseShortcut("Super+A").error() == EShortcutError::UNKNOWN_MODIFIER);
        assert(parseShortcut("Ctrl+").error() == EShortcutError::MISSING_KEY);
        assert(parseShortcut("Ctrl+a").error() == EShortcutError::UNKNOWN_KEY);
        auto make = [](std::string_view id, std::string_view shortcut) {
            return CommandEntry::create(
                contracts::CodeLease::builtin(), {CommandIdView{id}, "Shortcut", "Edit", shortcut},
                [](const CommandQuery&) -> CommandResult<CommandState> { return CommandState{true}; },
                [](const CommandInvocation&) -> CommandResult<DispatchReceipt> {
                    return DispatchReceipt{ImmediateCompletion{}};
                }
            );
        };
        CommandRegistry registry;
        auto original = CommandRegistrySnapshot::create({make("first", "Ctrl+A"), make("unbound", "")});
        assert(original && registry.publish(*original));
        const auto version = registry.revision();
        auto conflict = CommandRegistrySnapshot::create({make("first", "Ctrl+A"), make("second", "Ctrl+A")});
        assert(!conflict && conflict.error().code == ECommandError::SHORTCUT_CONFLICT);
        auto malformed = CommandRegistrySnapshot::create({make("bad", "Shift+Ctrl+A")});
        assert(!malformed && malformed.error().code == ECommandError::INVALID_ARGUMENT);
        assert(registry.revision() == version && original->find(CommandIdView{"first"}));
        auto valid = CommandRegistrySnapshot::create({make("first", "Ctrl+A"), make("second", "Ctrl+Shift+A")});
        assert(valid && registry.publish(*valid));
        assert(CommandRegistrySnapshot::create({make("empty1", ""), make("empty2", "")}));
        std::puts("PASS one shortcut parser, precise syntax failures, cold conflict refusal, unchanged published set");
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
        auto changed = CommandEntry::create(
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
        auto record = CommandEntry::create(
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
        auto record = CommandEntry::create(
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
                return DispatchReceipt{AcceptedOperation{OperationKindId{"save"}, 17}};
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
    descriptorStorageAndIndex();
    shortcutAdmission();
    compoundScope();
    pinnedAndCurrent();
    queryLifetime();
    repeatedSnapshotOwnership();
    busyAndReentry();
    std::puts("PASS immutable command entries, pinned/current policy, bounded BUSY FIFO, recursive and foreign calls");
}
