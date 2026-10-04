#include <lux/engine/editor/scene/SceneEditorCatalog.hpp>
#include <array>
#include <cassert>
#include <iostream>
#include <lux/engine/editor/configuration/EditorReflection.hpp>
#include <lux/engine/editor/extensions/Contributions.hpp>
#include <lux/engine/ui/Element.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Root.hpp>

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
    lux::editor::desktop::EditorContext editor_context{messages.dispatcherRef()};
    auto& commands = editor_context.commands();
    ContributionRegistry registry{messages.dispatcherRef(), editor_context, 2};
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
    auto code = lux::object::CodeLease::plugin(library);
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
                lux::object::CodeLease::builtin(),
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
    code = lux::object::CodeLease::builtin();
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
            lux::object::CodeLease::builtin(),
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
        desktop::EditorContext* context{};
        void run()
        {
            ++calls;
            auto result = commands.publish(candidate);
            blocked += !result && result.error().code == ECommandError::BUSY;
            if (context)
            {
                auto services = context->services().publish({});
                assert(!services && services.error().code == services::EServiceError::BUSY);
                auto empty = take(desktop::UiCatalog::prepare({}));
                auto ui = context->ui().publish(std::move(empty));
                assert(!ui && ui.error().code == desktop::EUiError::BUSY);
            }
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
        desktop::EditorContext editor_context{messages.dispatcherRef()};
        auto& commands = editor_context.commands();
        ContributionRegistry registry{messages.dispatcherRef(), editor_context, 4};
        PublicationAttempt attempt{commands, take(CommandRegistrySnapshot::create({command("C")}))};
        attempt.context = &editor_context;
        ContributionDraft first;
        first.commands.push_back(command("A"));
        if (mode == "factory")
        {
            first.views.push_back(views::ViewFactoryEntry::create(
                lux::object::CodeLease::builtin(),
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
                        lux::object::CodeLease::builtin(),
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
            first.code.push_back(lux::object::CodeLease::plugin(std::shared_ptr<const void>(
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
                    lux::object::CodeLease::builtin(),
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
            rejected.reflection.push_back({lux::object::CodeLease::builtin(), registerAttempt, &lux::editor::scene::validateSceneEditors});
            lux::editor::scene::SceneEditorCatalog::Definition definition;
            definition.configurations.push_back(lux::editor::scene::ConfigurationEditor{
                lux::object::CodeLease::builtin(),
                ConfigurationDescriptor{
                    "r11.invalid",
                    1,
                    serialization::makePortableValueCodec<int>(),
                    +[](meta::ReflectionRegistry&) noexcept -> const meta::RefClass* { return nullptr; }
                },
                +[](ui::Element&, ui::ElementId, ConfigurationValue&) noexcept
                    -> lux::editor::scene::ConfigurationEditor::CreateResult { return std::unique_ptr<ui::Element>{}; }
            });
            rejected.services.push_back(lux::editor::scene::declareSceneEditors(
                lux::object::CodeLease::builtin(), services::ServiceNameView{"test.r11.editors"}, std::move(definition)
            ));
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
namespace
{
    constexpr std::array binding_contracts{
        services::ServiceContract::forType<Binding, Binding>(services::ServiceNameView{"contribution.binding"})
    };
    constexpr auto binding_factory = [](services::ServiceResolver&, const services::ServiceConfiguration&) noexcept
        -> services::ServiceResult<std::unique_ptr<Binding>> { return std::make_unique<Binding>(9); };
    constexpr auto binding_service = services::ServiceDescriptor::forType<Binding, binding_factory>(
        services::ServiceNameView{"contribution.binding.default"},
        binding_contracts
    );
    constexpr std::array binding_dependencies{
        services::ServiceDependency{services::ServiceNameView{"contribution.binding"}, 1, cxx::typeToken<Binding>()},
        services::ServiceDependency{
            services::ServiceNameView{"contribution.facts"},
            1,
            cxx::typeToken<Facts>(),
            services::EDependencyKind::BORROWED
        }
    };
    constexpr desktop::UiDescriptor binding_ui{
        views::ViewTypeIdView{"extension.window"},
        "Window",
        binding_dependencies,
        1,
        nullptr,
        [](services::ServiceResolver& resolver,
           const desktop::UiCreateInfo& input) -> desktop::UiResult<std::unique_ptr<ui::Pane>>
        {
            auto binding = resolver.get<Binding>(0);
            auto facts = resolver.require<Facts>(1);
            assert(binding && (*binding)->value == 9 && facts);
            ++facts->get().old_calls;
            return std::make_unique<Window>(input.dispatcher, input.instance, facts->get());
        }
    };
    void commandReadsCatalog()
    {
        auto messages = take(object::ObjectMessageQueue::create(32));
        desktop::EditorContext context{messages.dispatcherRef()};
        auto& commands = context.commands();
        ContributionRegistry registry{messages.dispatcherRef(), context};
        commands::CommandDispatcher dispatcher{commands};
        unsigned invoked{};
        ContributionDraft draft;
        draft.commands.push_back(commands::CommandEntry::create(
            object::CodeLease::builtin(),
            {commands::CommandIdView{"test.read.catalog"}, "Read catalog"},
            [](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState>
            { return commands::CommandState{true}; },
            [&](const commands::CommandInvocation& input) -> commands::CommandResult<commands::DispatchReceipt>
            {
                auto use = [&](const ContributionSnapshot& snapshot) -> ContributionResult<void>
                {
                    auto pinned = take(snapshot.commands().at(0));
                    assert(!commands.publish({}));
                    assert(!registry.applyPending());
                    auto recursive = commands.execute(pinned, input);
                    assert(!recursive && recursive.error().code == commands::ECommandError::BUSY);
                    auto nested_read = commands.readBatch();
                    assert(nested_read && !commands.publish({}));
                    ++invoked;
                    return {};
                };
                auto result = registry.withSnapshot(use);
                if (!result)
                {
                    std::cerr << "command catalog rejected domain=" << result.error().domain
                              << " code=" << static_cast<unsigned>(result.error().code) << std::endl;
                }
                assert(result);
                // A read's return cannot release the enclosing command/dispatcher protection.
                assert(!commands.publish({}) && !dispatcher.drain());
                return commands::DispatchReceipt{commands::ImmediateCompletion{}};
            }
        ));
        auto catalog = take(ContributionSnapshot::prepare(std::move(draft)));
        assert(registry.enqueue(catalog) && registry.applyPending());
        commands::CommandInvocation input;
        auto handle = take(commands.snapshot().at(0));
        assert(commands.execute(handle, input));
        assert(dispatcher.enqueue(handle, input));
        auto completed = dispatcher.drain();
        assert(completed && completed->size() == 1 && completed->front().result && invoked == 2);
        assert(commands.canPublish());
        std::cout << "Command and queued dispatch may read the fixed catalog; nested execution/publication remain BUSY "
                     "PASS\n";
    }
    void neutralCatalog()
    {
        auto messages = take(object::ObjectMessageQueue::create(32));
        desktop::EditorContext context{messages.dispatcherRef()};
        ContributionRegistry registry{messages.dispatcherRef(), context};
        auto scope = take(context.services().createScope());
        Facts facts;
        assert(scope.provide(services::ServiceNameView{"contribution.facts"}, facts));
        ContributionDraft draft;
        draft.services.push_back(services::ServiceEntry::bind<binding_service>(object::CodeLease::builtin()));
        draft.ui.push_back(desktop::UiEntry::bind<binding_ui>(object::CodeLease::builtin()));
        draft.commands.push_back(command("binding.command"));
        auto candidate = take(ContributionSnapshot::prepare(std::move(draft)));
        assert(registry.enqueue(candidate) && registry.applyPending());
        assert(facts.old_calls == 0);
        auto use = [&](const ContributionSnapshot& current) -> ContributionResult<void>
        {
            assert(current.ui().entries()[0] == context.ui().snapshot().entries()[0]);
            auto handle = take(current.ui().at(0));
            for (auto name : {"left", "right"})
            {
                auto pane = context.ui().create(handle, scope, {messages.dispatcherRef(), ui::PaneId{name}, {}, {}});
                assert(pane && !(*pane)->attachedRoot());
            }
            auto root = take(ui::Root::create(messages.dispatcherRef()));
            std::vector<desktop::UiMountRequest> requests{
                {handle, {messages.dispatcherRef(), ui::PaneId{"mounted-left"}, {}, {}}, true},
                {handle, {messages.dispatcherRef(), ui::PaneId{"mounted-right"}, {}, {}}, true}
            };
            auto mounted = context.ui().mount(*root, scope, std::move(requests));
            assert(mounted && root->panes().size() == 2);
            auto services = context.services().publish({});
            assert(!services && services.error().code == services::EServiceError::BUSY);
            auto ui = context.ui().publish(take(desktop::UiCatalog::prepare({})));
            assert(!ui && ui.error().code == desktop::EUiError::BUSY);
            return {};
        };
        assert(registry.withSnapshot(use));
        assert(facts.old_calls == 4 && facts.destroyed == 4);
        ContributionDraft invalid;
        auto invalid_service = binding_service;
        invalid_service.destroy = nullptr;
        invalid.services.push_back(services::ServiceEntry::create(object::CodeLease::builtin(), invalid_service));
        auto rejected = ContributionSnapshot::prepare(std::move(invalid));
        assert(!rejected && rejected.error().domain == "services");
        assert(registry.revision() == 1 && context.ui().revision() == 1 && context.commands().revision() == 1);
        assert(scope.release() && scope.drained());
        std::cout << "Actual contribution owner atomically publishes service/UI/command catalogs; "
                     "fixed read scope permits lazy factories and rejects directory changes PASS\n";
    }
} // namespace
void domainCatalog()
{
    auto messages = take(object::ObjectMessageQueue::create(16));
    desktop::EditorContext context{messages.dispatcherRef()};
    ContributionRegistry contributions{messages.dispatcherRef(), context};
    auto scope = take(context.services().createScope());
    scene::SceneEditorCatalog::Definition definition;
    definition.components.push_back({
        cxx::typeToken<Binding>(), "Binding",
        +[](ui::Element& parent, ui::ElementId id, scene::InspectorFields&) -> scene::InspectorComponent::CreateResult {
            return std::make_unique<ui::Label>(parent, std::move(id), "Binding");
        }
    });
    auto first = scene::declareSceneEditors(
        object::CodeLease::builtin(), services::ServiceNameView{"test.scene.editor.first"}, definition
    );
    auto backing = take(first->definition<scene::SceneEditorCatalog::Definition>());
    ContributionDraft draft;
    draft.services.push_back(first);
    draft.reflection.push_back({object::CodeLease::builtin(), {}, &scene::validateSceneEditors});
    auto candidate = take(ContributionSnapshot::prepare(std::move(draft)));
    assert(contributions.enqueue(candidate) && contributions.applyPending());
    auto catalog = take(context.services().get<scene::SceneEditorCatalog>(scope));
    auto again = take(context.services().get<scene::SceneEditorCatalog>(scope));
    assert(catalog == again && &catalog->definition() == backing.get());
    assert(catalog->definition().components[0].type == cxx::typeToken<Binding>());
    for (bool collision : {false, true})
    {
        auto conflicting = definition;
        if (collision)
            conflicting.components[0].type = {cxx::typeToken<Binding>().hash(), "DifferentBinding"};
        ContributionDraft duplicate;
        duplicate.services.push_back(first);
        duplicate.services.push_back(scene::declareSceneEditors(
            object::CodeLease::builtin(), services::ServiceNameView{"test.scene.editor.second"}, std::move(conflicting)
        ));
        duplicate.reflection.push_back({object::CodeLease::builtin(), {}, &scene::validateSceneEditors});
        auto rejected = take(ContributionSnapshot::prepare(std::move(duplicate)));
        assert(contributions.enqueue(rejected));
        auto applied = contributions.applyPending();
        assert(!applied && applied.error().domain == "component");
        const auto expected = collision ? services::EServiceError::HASH_COLLISION : services::EServiceError::DUPLICATE;
        assert(applied.error().domain_code == static_cast<std::uint64_t>(expected));
        assert(contributions.revision() == 1 && context.commands().revision() == 1 && context.ui().revision() == 1);
        assert(take(context.services().get<scene::SceneEditorCatalog>(scope)) == catalog);
        assert(contributions.snapshot().services().size() == 1);
    }
    auto empty = take(ContributionSnapshot::prepare({}));
    assert(contributions.enqueue(empty) && contributions.applyPending());
    assert(!context.services().resolve<scene::SceneEditorCatalog>());
    assert(catalog->definition().components[0].label == "Binding");
    catalog.reset();
    again.reset();
    assert(scope.release() && scope.drained());
}
int main(int argc, char** argv)
{
    if (argc == 2)
    {
        r11Batch(argv[1]);
        return 0;
    }
    neutralCatalog();
    commandReadsCatalog();
    domainCatalog();
    return originalCases();
}
