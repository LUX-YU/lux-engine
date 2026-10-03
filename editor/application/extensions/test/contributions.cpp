#include <lux/engine/editor/extensions/Contributions.hpp>
#include <lux/engine/editor/configuration/EditorReflection.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/ui/Element.hpp>
#include <cassert>
#include <iostream>

using namespace lux;
using namespace lux::editor;
using namespace lux::editor::extensions;
namespace
{
    template <class T> auto take(T value)
    {
        assert(value);
        return std::move(*value);
    }
    struct Binding final
    {
        unsigned value;
    };
    struct Facts final
    {
        bool code_alive{true};
        unsigned destroyed{}, old_calls{}, new_calls{};
    };
    class Window final : public ui::Pane
    {
    public:
        Window(object::ObjectDispatcherRef dispatcher, ui::PaneId id, Facts& facts)
            : Pane(dispatcher, std::move(id), ui::PaneTypeId{"extension.window"}, "Extension"), facts_(facts)
        {
        }
        ~Window() override
        {
            assert(facts_.code_alive);
            ++facts_.destroyed;
        }

    private:
        Facts& facts_;
    };
} // namespace
int originalCases()
{
    auto messages = take(object::ObjectMessageQueue::create(32));
    commands::CommandRegistry commands;
    ContributionRegistry registry{messages.dispatcherRef(), commands, 2};
    Facts facts;
    auto library = std::shared_ptr<const void>(
        new int{1},
        [&](const void* p)
        {
            assert(facts.destroyed == 2);
            facts.code_alive = false;
            delete static_cast<const int*>(p);
            auto recursive = registry.applyPending();
            assert(!recursive && recursive.error().code == EContributionError::BUSY);
        }
    );
    auto code = contracts::CodeLease::plugin(library);
    auto empty = take(ContributionSnapshot::prepare({}));
    auto later = empty;
    auto pending = empty;
    ContributionDraft draft;
    bool replace{};
    draft.views.push_back(views::ViewFactoryEntry::create(
        code,
        views::ViewFactoryDescriptor{views::ViewTypeIdView{"extension.window"}, "Window", cxx::typeToken<Binding>()},
        [&, code](const views::ViewFactoryInput& input) -> views::ViewFactoryResult<views::DetachedView>
        {
            ++facts.old_calls;
            if (!replace)
            {
                replace = true;
                assert(registry.enqueue(later));
                const auto refused = registry.applyPending();
                assert(!refused && refused.error().code == EContributionError::BUSY);
                assert(registry.revision() == 1);
            }
            return views::DetachedView{code, std::make_unique<Window>(input.dispatcher(), input.paneId(), facts)};
        }
    ));
    auto prepared = take(ContributionSnapshot::prepare(std::move(draft)));
    assert(registry.enqueue(prepared));
    assert(!prepared.valid());
    assert(registry.applyPending());
    std::vector<views::DetachedView> views;
    auto batch = [&](const ContributionSnapshot& snapshot) -> ContributionResult<void>
    {
        for (const auto id : {"one", "two"})
        {
            views::ViewFactoryInput input{
                messages.dispatcherRef(),
                ui::PaneId{id},
                contracts::CodeLease::builtin(),
                cxx::typeToken<Binding>(),
                std::make_shared<const Binding>(Binding{9})
            };
            views.push_back(take(snapshot.views().prepare(views::ViewTypeId{"extension.window"}, input)));
            assert(!views.back().pane()->attachedRoot());
        }
        assert(snapshot.views().entries().size() == 1 && facts.old_calls == 2);
        return {};
    };
    assert(registry.withSnapshot(batch));
    assert(registry.enqueue(pending));
    auto full = empty;
    auto refused = registry.enqueue(full);
    assert(!refused && full.valid());
    library.reset();
    code = contracts::CodeLease::builtin();
    views.clear();
    assert(facts.destroyed == 2 && facts.code_alive);
    // The external pin retires under the publication guard, after all catalogs have changed.
    assert(registry.applyPending());
    assert(!facts.code_alive);
    assert(registry.snapshot().views().entries().empty() && commands.snapshot().entries().empty());
    assert(registry.applyPending());

    // Notification runs only after publication. A nested request is retained for the next outer turn.
    unsigned notifications{};
    object::LuxObject receiver{messages.dispatcherRef()};
    auto notification = take(object::LuxObject::connect(
        &registry,
        &ContributionRegistry::changed,
        &receiver,
        [&](std::uint64_t value) noexcept
        {
            ++notifications;
            assert(value == registry.revision());
            assert(commands.revision() == value);
            assert(!registry.applyPending());
            if (notifications == 1)
            {
                auto next = empty;
                assert(registry.enqueue(next));
            }
        },
        object::EDelivery::DIRECT
    ));
    auto next = empty;
    assert(registry.enqueue(next));
    assert(registry.applyPending());
    assert(notifications == 1);
    assert(registry.applyPending());
    assert(notifications == 2);

    // Duplicate type and incomplete factories reject a complete candidate, leaving the live revision untouched.
    const auto revision = registry.revision();
    ContributionDraft broken;
    broken.sessions.push_back(nullptr);
    assert(!ContributionSnapshot::prepare(std::move(broken)));
    assert(registry.revision() == revision);
    std::cout
        << "PASS immutable batch, bounded publication, cleanup pins, post-publication notification and rejection\n";
    return 0;
}

namespace
{
    using namespace commands;
    std::shared_ptr<CommandEntry> command(std::string_view id)
    {
        return CommandEntry::create(
            contracts::CodeLease::builtin(),
            CommandDescriptor{CommandIdView{id}, std::string{id}},
            [](const CommandQuery&) -> CommandResult<CommandState> { return CommandState{true}; },
            [](const CommandInvocation&) -> CommandResult<DispatchReceipt>
            { return DispatchReceipt{ImmediateCompletion{}}; }
        );
    }
    ContributionSnapshot catalog(std::string_view id)
    {
        ContributionDraft draft;
        draft.commands.push_back(command(id));
        return take(ContributionSnapshot::prepare(std::move(draft)));
    }
    struct PublicationAttempt final
    {
        CommandRegistry& commands;
        CommandRegistrySnapshot candidate;
        unsigned calls{}, blocked{};
        void run()
        {
            ++calls;
            auto result = commands.publish(candidate);
            blocked += !result && result.error().code == ECommandError::BUSY;
        }
    };
    PublicationAttempt* reflection_attempt{};
    void
    registerAttempt(meta::ReflectionRegistry& reflection, std::vector<std::pair<std::string_view, meta::RefType*>>&)
    {
        reflection_attempt->run();
        auto value = std::make_unique<meta::RefClass>();
        value->name = "R11DraftOnly";
        value->full_name = "R11DraftOnly";
        value->hash = cxx::type_hash<Binding>();
        value->type = meta::ref_type_of_v<Binding>;
        value->type.ptr = value.get();
        reflection.registerClass(std::move(value));
        assert(reflection.findClass("R11DraftOnly"));
        assert(!meta::ReflectionRegistry::instance().findClass("R11DraftOnly"));
    }
    void r11Batch(std::string_view mode)
    {
        auto messages = take(object::ObjectMessageQueue::create(32));
        CommandRegistry commands;
        ContributionRegistry registry{messages.dispatcherRef(), commands, 4};
        PublicationAttempt attempt{commands, take(CommandRegistrySnapshot::create({command("C")}))};
        ContributionDraft first;
        first.commands.push_back(command("A"));
        if (mode == "factory")
        {
            first.views.push_back(views::ViewFactoryEntry::create(
                contracts::CodeLease::builtin(),
                views::ViewFactoryDescriptor{views::ViewTypeIdView{"test.batch"}, "Batch", cxx::typeToken<Binding>()},
                [&](const views::ViewFactoryInput& input) -> views::ViewFactoryResult<views::DetachedView>
                {
                    auto pinned_command = take(commands.snapshot().find(CommandIdView{"A"}));
                    attempt.run();
                    auto recursive = registry.applyPending();
                    assert(!recursive && recursive.error().code == EContributionError::BUSY);
                    // An independent registry and pinned query remain usable; only participating publication is held.
                    CommandRegistry independent;
                    assert(independent.publish(attempt.candidate));
                    CommandInvocation invocation;
                    assert(commands.query(pinned_command, invocation.query()));
                    return views::DetachedView{
                        contracts::CodeLease::builtin(),
                        std::make_unique<ui::Pane>(
                            input.dispatcher(),
                            input.paneId(),
                            ui::PaneTypeId{"test.batch"},
                            "Batch"
                        )
                    };
                }
            ));
        }
        if (mode == "cleanup")
        {
            first.code.push_back(contracts::CodeLease::plugin(std::shared_ptr<const void>(
                new int{1},
                [&](const void* p)
                {
                    attempt.run();
                    assert(registry.snapshot().commands().find(CommandIdView{"B"}));
                    auto next = catalog("D");
                    assert(registry.enqueue(next));
                    delete static_cast<const int*>(p);
                }
            )));
        }
        auto initial = take(ContributionSnapshot::prepare(std::move(first)));
        assert(registry.enqueue(initial) && registry.applyPending());
        assert(commands.revision() == 1 && registry.revision() == 1);
        bool facts_correct{};
        if (mode == "factory")
        {
            auto next = catalog("B");
            assert(registry.enqueue(next));
            auto batch = [&](const ContributionSnapshot& snapshot) -> ContributionResult<void>
            {
                views::ViewFactoryInput input{
                    messages.dispatcherRef(),
                    ui::PaneId{"one"},
                    contracts::CodeLease::builtin(),
                    cxx::typeToken<Binding>(),
                    std::make_shared<const Binding>(Binding{9})
                };
                auto view = take(snapshot.views().prepare(views::ViewTypeId{"test.batch"}, input));
                assert(!view.pane()->attachedRoot());
                facts_correct = commands.revision() == 1 && registry.revision() == 1 &&
                                bool(commands.snapshot().find(CommandIdView{"A"}));
                return {};
            };
            assert(registry.withSnapshot(batch));
        }
        else if (mode == "reflection")
        {
            // The observer owns the environment across candidate rejection, when its pin is released.
            auto environment = acquireEditorReflection();
            ContributionDraft rejected;
            rejected.commands.push_back(command("B"));
            rejected.reflection.push_back({contracts::CodeLease::builtin(), registerAttempt});
            rejected.configurations.push_back(lux::editor::scene::ConfigurationEditor{
                contracts::CodeLease::builtin(),
                ConfigurationDescriptor{
                    "r11.invalid",
                    1,
                    serialization::makePortableValueCodec<int>(),
                    +[](meta::ReflectionRegistry&) noexcept -> const meta::RefClass* { return nullptr; }
                },
                +[](ui::Element&, ui::ElementId, ConfigurationValue&) noexcept
                -> lux::editor::scene::ConfigurationEditor::CreateResult { return std::unique_ptr<ui::Element>{}; }
            });
            auto candidate = take(ContributionSnapshot::prepare(std::move(rejected)));
            const auto reflection_count = meta::ReflectionRegistry::instance().classes().size();
            reflection_attempt = &attempt;
            assert(registry.enqueue(candidate));
            const auto result = registry.applyPending();
            reflection_attempt = nullptr;
            assert(!result && result.error().domain == "configuration.reflection");
            facts_correct = commands.revision() == 1 && registry.revision() == 1 &&
                            bool(commands.snapshot().find(CommandIdView{"A"})) &&
                            bool(registry.snapshot().commands().find(CommandIdView{"A"})) &&
                            meta::ReflectionRegistry::instance().classes().size() == reflection_count &&
                            !meta::ReflectionRegistry::instance().findClass("R11DraftOnly");
        }
        else
        {
            object::LuxObject receiver{messages.dispatcherRef()};
            unsigned notifications{};
            auto connection = take(object::LuxObject::connect(
                &registry,
                &ContributionRegistry::changed,
                &receiver,
                [&](std::uint64_t revision) noexcept
                {
                    ++notifications;
                    if (mode == "notify")
                    {
                        attempt.run();
                        if (notifications == 1)
                        {
                            auto next = catalog("D");
                            assert(registry.enqueue(next));
                        }
                    }
                    facts_correct = commands.revision() == revision &&
                                    commands.snapshot().entries()[0] == registry.snapshot().commands().entries()[0];
                },
                object::EDelivery::DIRECT
            ));
            auto next = catalog("B");
            assert(registry.enqueue(next) && registry.applyPending());
            std::cout << "R11 first publication " << mode << " direct_calls=" << attempt.calls
                      << " blocked=" << attempt.blocked << " consistent=" << facts_correct << std::endl;
            assert(attempt.blocked == attempt.calls && facts_correct && notifications == 1);
            assert(registry.applyPending());
            assert(notifications == 2 && commands.snapshot().find(CommandIdView{"D"}));
        }
        std::cout << "R11 batch " << mode << " direct_calls=" << attempt.calls << " blocked=" << attempt.blocked
                  << " consistent=" << facts_correct << std::endl;
        assert(attempt.calls && attempt.calls == attempt.blocked && facts_correct);
        assert(commands.canPublish() && commands.publish(attempt.candidate));
        assert(commands.snapshot().find(CommandIdView{"C"}));
    }
} // namespace
int main(int argc, char** argv)
{
    if (argc == 2)
    {
        r11Batch(argv[1]);
        return 0;
    }
    return originalCases();
}
