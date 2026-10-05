#include <cassert>
#include <cstdio>
#include <functional>
#include <lux/engine/editor/commands/CommandRegistry.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/services/ServiceRegistry.hpp>
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
            lux::object::CodeLease::builtin(),
            descriptor(),
            [](const CommandQuery&) -> CommandResult<CommandState> { return CommandState{true}; },
            [&calls](const CommandInvocation&) -> CommandResult<DispatchReceipt>
            {
                ++calls;
                return DispatchReceipt{ImmediateCompletion{}};
            }
        );
    }
    inline constexpr CommandDescriptor literal{CommandIdView{"test.literal"}, "Static label", "Edit", "Ctrl+L"};
    static_assert(literal.id.hash() == cxx::Fnv1a64::hash("test.literal"));
    static_assert(!std::is_constructible_v<
                  CommandEntry,
                  lux::object::CodeLease,
                  CommandDescriptor,
                  CommandEntry::Query,
                  CommandEntry::Execute>);
    void typedViewTargets()
    {
        struct Target final { unsigned value; };
        int queries{}, executions{};
        auto metadata = descriptor();
        metadata.scope = ECommandScope::VIEW;
        metadata.target_type = cxx::typeToken<Target>();
        auto create = [&](const CommandDescriptor& value)
        {
            return CommandEntry::create(
                lux::object::CodeLease::builtin(), value,
                [&](const CommandQuery& query) -> CommandResult<CommandState>
                {
                    ++queries;
                    assert(query.view<Target>() && query.view<Target>()->value == 73);
                    return CommandState{true};
                },
                [&](const CommandInvocation& input) -> CommandResult<DispatchReceipt>
                {
                    ++executions;
                    assert(input.view<Target>() && input.view<Target>()->value == 73);
                    return DispatchReceipt{};
                }
            );
        };
        auto original = CommandRegistrySnapshot::create({create(metadata)});
        assert(original);
        CommandRegistry registry;
        assert(registry.publish(*original));
        auto handle = original->at(0);
        assert(handle);
        auto input = CommandInvocation::forView(Target{73}, lux::object::CodeLease::builtin());
        auto copied = input;
        assert(copied.view<Target>() == input.view<Target>());
        assert(registry.execute(*handle, copied) && executions == 1);
        const auto observed = queries;
        const auto wrong = registry.execute(
            *handle, CommandInvocation::forView(73u, lux::object::CodeLease::builtin())
        );
        const auto missing = registry.execute(*handle, CommandInvocation{CommandArguments{}});
        assert(!wrong && wrong.error().code == ECommandError::INVALID_ARGUMENT);
        assert(!missing && missing.error().code == ECommandError::INVALID_ARGUMENT);
        assert(queries == observed && executions == 1);
        metadata.target_type = cxx::typeToken<unsigned>();
        auto changed = CommandRegistrySnapshot::create({create(metadata)});
        assert(changed && changed->resolve(*handle).error().code == ECommandError::INCOMPATIBLE_REGISTRATION);
        assert(registry.publish(*changed));
        assert(registry.execute(*handle, copied) && executions == 2); // Original pinned declaration.
        metadata.target_type = {};
        assert(!CommandRegistrySnapshot::create({create(metadata)}));
        metadata.scope = ECommandScope::APPLICATION;
        metadata.target_type = cxx::typeToken<Target>();
        assert(!CommandRegistrySnapshot::create({create(metadata)}));
        std::puts(
            "PASS immutable provider target: exact type, copied value, pinned/current registration and no UI dependency"
        );
    }
    void descriptorStorageAndIndex()
    {
        auto query = [](const CommandQuery&) -> CommandResult<CommandState> { return CommandState{true}; };
        auto execute = [](const CommandInvocation&) -> CommandResult<DispatchReceipt>
        { return DispatchReceipt{ImmediateCompletion{}}; };
        auto fixed = CommandEntry::bind<literal>(lux::object::CodeLease::builtin(), query, execute);
        assert(&fixed->descriptor() == &literal && fixed->descriptor().label.data() == literal.label.data());
        std::shared_ptr<CommandEntry> dynamic;
        {
            std::string name{"test.dynamic"}, label{"Label"}, group{"A/B"}, key{"Alt+M"}, argument{"payload"};
            dynamic = CommandEntry::create(
                lux::object::CodeLease::builtin(),
                {CommandIdView{name}, label, group, key, ECommandScope::APPLICATION, 3, {91, argument}},
                query,
                execute
            );
            name.assign(2000, 'n');
            label.assign(2000, 'l');
            group.clear();
            key.clear();
            argument.clear();
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
        std::puts(
            "PASS static descriptor identity, one frozen dynamic backing, duplicate and public identity validation"
        );
        std::printf(
            "sizeof Descriptor=%zu Entry=%zu Handle=%zu\n",
            sizeof(CommandDescriptor),
            sizeof(CommandEntry),
            sizeof(CommandHandle)
        );
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
            {
                auto nested = registry.readBatch();
                assert(nested && !registry.preparePublication({}) && !registry.publish({}));
            }
            assert(!registry.preparePublication({}) && !registry.publish({}));
            assert(registry.query(*handle, input.query()) && registry.execute(*handle, input));
            assert(dispatcher.enqueue(*handle, input));
            const auto blocked = dispatcher.drain();
            assert(!blocked && blocked.error().code == ECommandError::BUSY && dispatcher.pending() == 1);
        }
        assert(registry.canPublish() && dispatcher.drain() && calls == 2);
        unsigned cleaned{};
        {
            auto code = lux::object::CodeLease::plugin(std::shared_ptr<const void>(
                new int{1},
                [&](const void* p)
                {
                    ++cleaned;
                    auto publish = registry.publish({});
                    assert(!publish && publish.error().code == ECommandError::BUSY);
                    delete static_cast<const int*>(p);
                }
            ));
            auto prepared = CommandRegistrySnapshot::create({CommandEntry::create(
                std::move(code),
                descriptor(),
                [](const CommandQuery&) -> CommandResult<CommandState> { return CommandState{true}; },
                [](const CommandInvocation&) -> CommandResult<DispatchReceipt>
                { return DispatchReceipt{ImmediateCompletion{}}; }
            )});
            assert(prepared);
            auto batch = registry.preparePublication(std::move(*prepared));
            assert(batch && cleaned == 0);
        } // Abandoned candidate cleanup must stay protected, with no revision change.
        assert(cleaned == 1 && registry.revision() == 1 && registry.canPublish());
        {
            auto batch = registry.preparePublication({});
            assert(batch);
            assert(!registry.readBatch());
            auto old = batch->commit();
            assert(registry.revision() == 2 && old.entries().size() == 1 && !registry.publish({}));
        }
        assert(registry.canPublish());
        {
            std::optional<CommandRegistry::Batch> outer;
            outer.emplace(std::move(*registry.readBatch()));
            auto inner = registry.readBatch();
            assert(inner);
            outer.reset();
            assert(!registry.publish({}));
        }
        assert(registry.canPublish());
        std::thread foreign(
            [&]
            {
                const auto rejected = registry.readBatch();
                assert(!rejected && rejected.error().code == ECommandError::WRONG_THREAD);
            }
        );
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
        auto make = [](std::string_view id, std::string_view shortcut)
        {
            return CommandEntry::create(
                lux::object::CodeLease::builtin(),
                {CommandIdView{id}, "Shortcut", "Edit", shortcut},
                [](const CommandQuery&) -> CommandResult<CommandState> { return CommandState{true}; },
                [](const CommandInvocation&) -> CommandResult<DispatchReceipt>
                { return DispatchReceipt{ImmediateCompletion{}}; }
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
            lux::object::CodeLease::builtin(),
            incompatible,
            [](const CommandQuery&) -> CommandResult<CommandState> { return CommandState{true}; },
            [](const CommandInvocation&) -> CommandResult<DispatchReceipt>
            { return DispatchReceipt{ImmediateCompletion{}}; }
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
            lux::object::CodeLease::plugin(code),
            descriptor(),
            [&](const CommandQuery& query) -> CommandResult<CommandState>
            {
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
            [](const CommandInvocation&) -> CommandResult<DispatchReceipt>
            { return DispatchReceipt{ImmediateCompletion{}}; }
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
            lux::object::CodeLease::builtin(),
            descriptor(),
            [&](const CommandQuery&) -> CommandResult<CommandState>
            {
                if (busy)
                {
                    return cxx::unexpected(CommandFailure{ECommandError::BUSY, "real.domain", 29, "reading"});
                }
                return CommandState{true};
            },
            [&](const CommandInvocation&) -> CommandResult<DispatchReceipt>
            {
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
        std::thread foreign(
            [&]
            {
                auto result = registry.query(*handle, input.query());
                assert(!result && result.error().code == ECommandError::WRONG_THREAD);
            }
        );
        foreign.join();
    }
    struct BindingTrace final
    {
        unsigned created{}, destroyed{}, factories{}, executed{};
        bool malformed{};
        std::function<void()> constructing, querying, destroying;
    };
    struct Receiver final
    {
        BindingTrace& trace;
        explicit Receiver(BindingTrace& trace) : trace(trace)
        {
            ++trace.created;
        }
        ~Receiver()
        {
            ++trace.destroyed;
            if (trace.destroying)
            {
                trace.destroying();
            }
        }
        static lux::services::ServiceResult<std::unique_ptr<Receiver>>
        create(lux::services::ServiceResolver& resolver, const lux::services::ServiceConfiguration&) noexcept
        {
            auto trace = resolver.require<BindingTrace>(0);
            if (!trace)
            {
                return cxx::unexpected(std::move(trace.error()));
            }
            return std::make_unique<Receiver>(trace->get());
        }
    };
    constexpr lux::services::ServiceDependency trace_dependency[]{
        {lux::services::ServiceNameView{"test.command.trace"},
         1,
         cxx::typeToken<BindingTrace>(),
         lux::services::EDependencyKind::BORROWED}
    };
    constexpr lux::services::ServiceContract receiver_contract[]{
        lux::services::ServiceContract::forType<Receiver, Receiver>(
            lux::services::ServiceNameView{"test.command.receiver"}
        )
    };
    constexpr auto receiver_descriptor = lux::services::ServiceDescriptor::forType<Receiver, &Receiver::create>(
        lux::services::ServiceNameView{"test.command.receiver.default"},
        receiver_contract,
        trace_dependency
    );
    constexpr lux::services::ServiceDependency command_dependencies[]{
        {lux::services::ServiceNameView{"test.command.receiver"}, 1, cxx::typeToken<Receiver>()}
    };
    CommandResult<std::unique_ptr<CommandBinding>>
    createBinding(lux::services::ServiceResolver& resolver, const lux::object::CodeLease&) noexcept
    {
        auto receiver = resolver.get<Receiver>(0);
        if (!receiver)
        {
            const auto& error = receiver.error();
            return cxx::unexpected(CommandFailure{
                ECommandError::DOMAIN_FAILURE,
                "binding.dependency",
                static_cast<std::uint64_t>(error.code),
                error.detail
            });
        }
        assert(resolver.get<Receiver>(1).error().code == lux::services::EServiceError::UNDECLARED_DEPENDENCY);
        ++(*receiver)->trace.factories;
        if ((*receiver)->trace.constructing)
        {
            (*receiver)->trace.constructing();
        }
        if ((*receiver)->trace.malformed)
        {
            return std::make_unique<CommandBinding>(
                [value = *receiver](const CommandQuery&) -> CommandResult<CommandState> { return CommandState{true}; },
                CommandBinding::Execute{}
            );
        }
        return std::make_unique<CommandBinding>(
            [value = *receiver](const CommandQuery&) -> CommandResult<CommandState>
            {
                if (value->trace.querying)
                {
                    value->trace.querying();
                }
                return CommandState{true};
            },
            [value = *receiver](const CommandInvocation&) -> CommandResult<DispatchReceipt>
            {
                ++value->trace.executed;
                return DispatchReceipt{ImmediateCompletion{}};
            }
        );
    }
    constexpr CommandDescriptor deferred_command{
        .id = CommandIdView{"test.command.deferred"},
        .label = "Deferred",
        .dependencies = command_dependencies,
        .create = &createBinding
    };
    void lazyBindings()
    {
        auto messages = lux::object::ObjectMessageQueue::create(16);
        assert(messages);
        lux::services::ServiceRegistry services{messages->dispatcherRef()};
        assert(services.publish({lux::services::ServiceEntry::bind<receiver_descriptor>(lux::object::CodeLease::builtin(
        ))}));
        auto first_scope = services.createScope(), second_scope = services.createScope();
        assert(first_scope && second_scope);
        BindingTrace first, second;
        assert(first_scope->provide(trace_dependency[0].contract, first));
        assert(second_scope->provide(trace_dependency[0].contract, second));
        auto entry = CommandEntry::bind<deferred_command>(lux::object::CodeLease::builtin());
        auto catalog = CommandRegistrySnapshot::create({entry});
        assert(catalog && &entry->descriptor() == &deferred_command);
        auto handle = *catalog->at(0);
        CommandInvocation input;
        CommandRegistry direct;
        assert(direct.query(handle, input.query()).error().code == ECommandError::DOMAIN_FAILURE);
        {
            CommandRegistry a{services, *first_scope}, b{services, *second_scope};
            assert(a.publish(*catalog) && b.publish(*catalog));
            assert(first.created == 0 && second.created == 0);
            auto busy = [&](lux::services::ServiceResolver&) -> lux::services::ServiceResult<void>
            {
                assert(a.query(handle, input.query()).error().code == ECommandError::BUSY);
                assert(first.factories == 0 && first.created == 0);
                return {};
            };
            assert(services.withDependencies(*first_scope, {}, busy));
            for (unsigned i{}; i < 100; ++i)
            {
                assert(a.query(handle, input.query()) && a.execute(handle, input));
            }
            assert(first.created == 1 && first.factories == 1 && first.executed == 100);
            assert(b.execute(handle, input) && second.created == 1 && second.factories == 1 && second.executed == 1);
            assert(first_scope->beginClose());
            assert(a.execute(handle, input).error().code == ECommandError::CLOSED);
            assert(first.executed == 100 && first.destroyed == 0);
            assert(first_scope->cancelClose() && a.execute(handle, input));
            first.querying = [&] { assert(first_scope->beginClose()); };
            assert(a.execute(handle, input).error().code == ECommandError::CLOSED);
            assert(first.executed == 101);
            first.querying = {};
            assert(first_scope->cancelClose());
            // Original PINNED handles retain their receiver; publication does not rebase their dependencies.
            assert(a.publish({}) && a.execute(handle, input));
            first.destroying = [&] { assert(a.publish({}).error().code == ECommandError::BUSY); };
            second.destroying = [&] { assert(b.publish({}).error().code == ECommandError::BUSY); };
        }
        assert(first.created == first.destroyed && second.created == second.destroyed);
        first.destroying = {};
        first.malformed = true;
        first.constructing = [&]
        { assert(first_scope->beginClose().error().code == lux::services::EServiceError::BUSY); };
        {
            CommandRegistry refused{services, *first_scope};
            first.destroying = [&]
            {
                assert(refused.publish({}).error().code == ECommandError::BUSY);
                assert(services.publish({}).error().code == lux::services::EServiceError::BUSY);
            };
            assert(refused.execute(handle, input).error().code == ECommandError::INVALID_ARGUMENT);
            assert(first.created == first.destroyed && first.factories == 2);
            first.constructing = {};
            first.malformed = false;
            assert(first_scope->isOpen() && refused.execute(handle, input));
            assert(first.factories == 3);
            first.destroying = {};
        }
        assert(first_scope->release() && second_scope->release() && services.drained());
        std::puts("PASS lazy command bindings: zero registration construction, one binding per scope, pinned target, "
                  "BUSY retry and guarded refusal");
    }
    void releaseScopedBindings()
    {
        auto messages = lux::object::ObjectMessageQueue::create(16);
        assert(messages);
        lux::services::ServiceRegistry services{messages->dispatcherRef()};
        assert(services.publish({lux::services::ServiceEntry::bind<receiver_descriptor>(lux::object::CodeLease::builtin(
        ))}));
        auto scope = services.createScope();
        assert(scope);
        BindingTrace trace;
        assert(scope->provide(trace_dependency[0].contract, trace));
        CommandRegistry commands{services, *scope};
        auto catalog =
            CommandRegistrySnapshot::create({CommandEntry::bind<deferred_command>(lux::object::CodeLease::builtin())});
        assert(catalog && commands.publish(*catalog));
        const auto handle = *catalog->at(0);
        assert(commands.execute(handle, CommandInvocation{}));
        assert(commands.releaseBindings().error().code == ECommandError::BUSY);
        assert(trace.created == 1 && trace.destroyed == 0);
        assert(commands.publish({}) && trace.destroyed == 0); // Pinned handle retains its receiver.
        bool rejected_thread{};
        std::jthread worker(
            [&] { rejected_thread = commands.releaseBindings().error().code == ECommandError::WRONG_THREAD; }
        );
        worker.join();
        assert(rejected_thread && trace.destroyed == 0);
        assert(scope->release());
        assert(!services.drained()); // Only the cached binding now retains the actual receiver.
        trace.destroying = [&]
        {
            assert(commands.releaseBindings().error().code == ECommandError::BUSY);
            assert(commands.publish({}).error().code == ECommandError::BUSY);
            assert(commands.execute(handle, CommandInvocation{}).error().code == ECommandError::BUSY);
        };
        {
            auto reading = commands.readBatch();
            assert(reading);
            assert(commands.releaseBindings().error().code == ECommandError::BUSY);
        }
        assert(commands.releaseBindings());
        assert(trace.destroyed == 1 && services.drained());
        assert(commands.releaseBindings());
        assert(commands.execute(handle, CommandInvocation{}).error().code == ECommandError::CLOSED);
        assert(trace.created == 1 && trace.destroyed == 1 && trace.executed == 1);
        std::puts(
            "PASS closed-scope receiver release: pinned identity, wrong-thread, read/cleanup reentry, exact destruction"
        );
    }
    void dynamicBindings()
    {
        auto messages = lux::object::ObjectMessageQueue::create(16);
        assert(messages);
        lux::services::ServiceRegistry services{messages->dispatcherRef()};
        assert(services.publish({lux::services::ServiceEntry::bind<receiver_descriptor>(lux::object::CodeLease::builtin(
        ))}));
        auto scope = services.createScope();
        assert(scope);
        BindingTrace trace;
        assert(scope->provide(trace_dependency[0].contract, trace));
        const auto make = [](std::string id)
        {
            std::string contract{command_dependencies[0].contract.name()};
            std::string type{command_dependencies[0].type.name()};
            auto dependency = command_dependencies[0];
            dependency.contract = lux::services::ServiceNameView{contract};
            dependency.type = {command_dependencies[0].type.hash(), type};
            auto descriptor = deferred_command;
            descriptor.id = CommandIdView{id};
            descriptor.dependencies = {&dependency, 1};
            auto entry = CommandEntry::create(lux::object::CodeLease::builtin(), descriptor);
            std::fill(contract.begin(), contract.end(), '?');
            std::fill(type.begin(), type.end(), '?');
            assert(entry->descriptor().dependencies[0].contract.name() == command_dependencies[0].contract.name());
            assert(entry->descriptor().dependencies[0].type.name() == command_dependencies[0].type.name());
            return entry;
        };
        {
            CommandRegistry commands{services, *scope, 1};
            auto next = CommandRegistrySnapshot::create({make("next")});
            assert(next);
            auto next_handle = *next->at(0);
            {
                auto first = CommandRegistrySnapshot::create({make("first")});
                assert(first);
                assert(commands.execute(*first->at(0), CommandInvocation{}));
                assert(commands.execute(next_handle, CommandInvocation{}).error().code == ECommandError::CAPACITY);
                assert(trace.factories == 1 && trace.created == 1 && trace.executed == 1);
            }
            trace.destroying = [&]
            {
                assert(commands.publish({}).error().code == ECommandError::BUSY);
                assert(services.publish({}).error().code == lux::services::EServiceError::BUSY);
            };
            assert(commands.execute(next_handle, CommandInvocation{}));
            assert(trace.factories == 2 && trace.created == 2 && trace.destroyed == 1 && trace.executed == 2);
            trace.destroying = {};
            auto malformed = deferred_command;
            malformed.create = nullptr;
            auto invalid =
                CommandRegistrySnapshot::create({CommandEntry::create(lux::object::CodeLease::builtin(), malformed)});
            assert(!invalid && invalid.error().code == ECommandError::INVALID_ARGUMENT);
            invalid = CommandRegistrySnapshot::create({CommandEntry::create(
                lux::object::CodeLease::builtin(),
                deferred_command,
                [](const CommandQuery&) -> CommandResult<CommandState> { return CommandState{true}; },
                {}
            )});
            assert(!invalid && invalid.error().code == ECommandError::INVALID_ARGUMENT);
        }
        assert(trace.created == trace.destroyed && scope->release() && services.drained());
        std::puts("PASS dynamic dependency backing, bounded lazy binding reclamation and incompatible binding refusal");
    }
} // namespace
int main()
{
    typedViewTargets();
    descriptorStorageAndIndex();
    shortcutAdmission();
    compoundScope();
    pinnedAndCurrent();
    queryLifetime();
    repeatedSnapshotOwnership();
    busyAndReentry();
    lazyBindings();
    dynamicBindings();
    releaseScopedBindings();
    std::puts("PASS immutable command entries, pinned/current policy, bounded BUSY FIFO, recursive and foreign calls");
}
