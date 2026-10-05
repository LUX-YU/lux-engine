#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <lux/engine/editor/desktop/ReviewView.hpp>
#include <lux/engine/editor/desktop/ViewCommands.hpp>
#include <lux/engine/editor/desktop/ViewHost.hpp>
#include <lux/engine/ui/Element.hpp>
#include <lux/engine/ui/Root.hpp>
#include <source_location>

using namespace lux;
using namespace lux::editor;
namespace
{
    template <class T> auto take(T value, std::source_location location = std::source_location::current())
    {
        if (!value)
        {
            std::fprintf(stderr, "Failed result at %s:%u\n", location.file_name(), location.line());
        }
        assert(value);
        return std::move(*value);
    }
    struct Facts final
    {
        unsigned pane{}, element{}, code{}, draws{};
        bool alive{true};
    };
    struct Code final
    {
        Facts& facts;
        explicit Code(Facts& value) : facts(value) {}
        ~Code()
        {
            assert(facts.pane == 1 && facts.element == 1);
            facts.alive = false;
            ++facts.code;
        }
    };
    class Content final : public ui::Element
    {
    public:
        Content(ui::Pane& pane, Facts& facts) : Element(pane, ui::ElementId{"body"}), facts_(facts) {}
        ~Content() override
        {
            assert(facts_.alive);
            ++facts_.element;
        }
        views::ViewRequests* requests{};
        views::ViewId target;

    private:
        void draw() noexcept override
        {
            ++facts_.draws;
            if (requests)
            {
                assert(requests->close(target));
                assert(!facts_.pane);
                requests = nullptr;
            }
        }
        Facts& facts_;
    };
    class Window final : public ui::Pane
    {
    public:
        Window(object::ObjectDispatcherRef dispatcher, const char* id, Facts& facts)
            : Pane(dispatcher, ui::PaneId{id}, ui::PaneTypeId{"p10.test"}, id), body(*this, facts), facts_(facts)
        {
            assert(setContent(body));
        }
        ~Window() override
        {
            assert(facts_.alive);
            ++facts_.pane;
        }
        Content body;
        unsigned close_attempts{};
        std::optional<views::ViewPreparationFailure> close_error;
        views::ViewCloseResult prepareClose()
        {
            ++close_attempts;
            if (close_error)
            {
                return cxx::unexpected(*close_error);
            }
            return {};
        }

    private:
        Facts& facts_;
    };
    views::DetachedView candidate(object::ObjectDispatcherRef dispatcher, const char* id, Facts& facts)
    {
        auto code = std::make_shared<Code>(facts);
        return {lux::object::CodeLease::plugin(code), std::make_unique<Window>(dispatcher, id, facts)};
    }
    void completeViewConnections(object::ObjectDispatcherRef dispatcher)
    {
        struct Sender final : object::LuxObject
        {
            explicit Sender(object::ObjectDispatcherRef dispatcher) : LuxObject(dispatcher) {}
            object::TSignal<> changed{*this};
            void send() noexcept
            {
                assert(emit(changed).complete());
            }
        } sender(dispatcher);
        Facts first_facts, second_facts;
        unsigned received{};
        auto first = candidate(dispatcher, "connected-first", first_facts);
        first.addConnection(take(object::LuxObject::connect(
            &sender,
            &Sender::changed,
            [&]() noexcept
            {
                assert(first_facts.alive && !first_facts.pane);
                ++received;
            }
        )));
        auto transferred = std::move(first);
        sender.send();
        assert(received == 1);
        auto second = candidate(dispatcher, "connected-second", second_facts);
        transferred = std::move(second);
        assert(first_facts.code == 1);
        sender.send();
        assert(received == 1); // Move replacement released the old callback before the old node/code.
    }
    void contentAssociations(object::ObjectDispatcherRef dispatcher)
    {
        auto root = take(ui::Root::create(dispatcher));
        desktop::ViewHost host(*root);
        struct Comparison final : ui::Pane
        {
            Comparison(object::ObjectDispatcherRef dispatcher, desktop::ViewHost& host)
                : Pane(dispatcher, ui::PaneId{"compare"}, ui::PaneTypeId{"comparison"}, "Comparison"), host_(host)
            {
            }
            views::ViewContent content_;
            views::ViewId id_;
            desktop::ViewHost& host_;
            bool busy_{};
            unsigned changes_{};
        };
        auto pane = std::make_unique<Comparison>(dispatcher, host);
        auto* comparison = pane.get();
        views::DetachedView view{
            lux::object::CodeLease::builtin(),
            std::move(pane),
            nullptr,
            nullptr,
            +[](const ui::Pane& pane) noexcept
            {
                const auto& comparison = static_cast<const Comparison&>(pane);
                // Capturing plugin metadata is also a callback boundary: owner destruction is forbidden.
                const auto nested = comparison.host_.drain();
                assert(!nested && nested.error() == views::EViewError::BUSY);
                return comparison.content_;
            },
            +[](ui::Pane& pane, const views::ViewContent& content) -> views::ViewCloseResult
            {
                auto& comparison = static_cast<Comparison&>(pane);
                const auto nested = comparison.host_.rebindContent(comparison.id_, {});
                assert(!nested && nested.error().retryable);
                if (comparison.busy_)
                {
                    return cxx::unexpected(views::ViewPreparationFailure{"comparison", 7, "Reading", true});
                }
                comparison.content_ = content;
                ++comparison.changes_;
                return {};
            }
        };
        const auto id = take(host.adopt(view, views::ViewRestoreKey{"comparison"})).id;
        comparison->id_ = id;
        assert(take(host.describe(id)).content.sessions.empty());
        const sessions::SessionId a{9, 0, 1}, b{9, 1, 3};
        const views::ViewContent pair{{a, b}, b};
        assert(host.rebindContent(id, pair) && take(host.describe(id)).content == pair);
        comparison->busy_ = true;
        const auto failed = host.rebindContent(id, {{a}, a});
        assert(!failed && failed.error().code == 7 && failed.error().retryable);
        assert(take(host.describe(id)).content == pair && comparison->changes_ == 1);
        comparison->busy_ = false;
        assert(!host.rebindContent(id, {{a, a}, a}));
        assert(!host.rebindContent(id, {{a}, b}));
        assert(comparison->changes_ == 1 && take(host.describe(id)).content == pair);
        assert(host.rebindContent(id, {{a}, a}));
        assert(host.rebindContent(id, {}));
        assert(take(host.describe(id)).content.sessions.empty());
        assert(host.close(id));
        take(host.drain());
        const auto stale = host.rebindContent(id, pair);
        assert(!stale && !stale.error().retryable);
        std::puts("PASS EC1 real Host zero/one/multiple content identities, primary, callback gate and stale view");
    }
    void ownership(object::ObjectDispatcherRef dispatcher)
    {
        auto root = take(ui::Root::create(dispatcher, {.docking = false}));
        Facts first, second, third;
        desktop::ViewHost host(*root, {1, 2});
        auto a = candidate(dispatcher, "first", first);
        auto* a_window = static_cast<Window*>(a.pane());
        assert(root->panes().empty());
        const auto id = take(host.adopt(a, views::ViewRestoreKey{"first"})).id;
        assert(!a.pane() && take(host.describe(id)).visible);
        auto b = candidate(dispatcher, "second", second);
        const auto full = host.adopt(b, views::ViewRestoreKey{"second"});
        assert(!full && full.error() == views::EViewError::CAPACITY && b.pane() && !b.pane()->attachedRoot());
        assert(host.focus(id) && host.show(id));
        auto overflow = host.close(id);
        assert(!overflow && overflow.error() == views::EViewError::CAPACITY);
        assert(take(host.drain()).completed == 2);
        a_window->body.requests = &host;
        a_window->body.target = id;
        ui::DrawData draw;
        assert(root->update({{640, 480}, .016F}, &draw));
        assert(first.draws && first.pane == 0 && host.describe(id));
        assert(take(host.drain()).completed == 1);
        assert(first.pane == 1 && first.element == 1 && first.code == 1);
        assert(!host.describe(id));
        assert(std::ranges::all_of(root->panes(), [](auto* pane) { return pane == nullptr; }));
        const auto next = take(host.adopt(b, views::ViewRestoreKey{"second"})).id;
        assert(next.slot == id.slot && next.generation != id.generation);
        assert(!host.close(id));
        assert(host.describe(next));
        // Native close intent survives a full external request queue.
        assert(host.show(next) && host.focus(next));
        root->panes().front()->requestClose();
        take(host.drain());
        assert(second.pane == 0 && take(host.closeIntents()) == std::vector{next});
        assert(host.dismissCloseIntent(next));
        assert(take(host.closeIntents()).empty() && host.describe(next));
        root->panes().front()->requestClose();
        assert(take(host.closeIntents()) == std::vector{next});
        assert(host.close(next)); // Product policy explicitly approved closing this view.
        take(host.drain());
        assert(second.pane == 1 && second.code == 1);
        auto c = candidate(dispatcher, "third", third);
        take(host.adopt(c, views::ViewRestoreKey{"third"}));
        // Destructor detaches before destroying the remaining owned unit.
    }
    void reentrant(object::ObjectDispatcherRef dispatcher)
    {
        auto root = take(ui::Root::create(dispatcher));
        Facts first, second;
        desktop::ViewHost host(*root, {2, 8});
        auto a = candidate(dispatcher, "notice-first", first);
        auto b = candidate(dispatcher, "notice-second", second);
        views::ViewId first_id;
        bool saw_busy{};
        unsigned notifications{};
        auto notice = take(object::LuxObject::connect(
            root.get(),
            &ui::Root::attachmentChanged,
            [&](const ui::AttachmentChanged& changed) noexcept
            {
                ++notifications;
                const auto all = host.describeAll();
                const auto nested = host.drain();
                assert(!all && all.error() == views::EViewError::BUSY);
                assert(!nested && nested.error() == views::EViewError::BUSY);
                saw_busy = true;
                if (changed.mounted && changed.pane == ui::PaneId{"notice-second"})
                {
                    assert(host.close(first_id));
                }
            }
        ));
        first_id = take(host.adopt(a, views::ViewRestoreKey{"notice-first"})).id;
        const auto second_id = take(host.adopt(b, views::ViewRestoreKey{"notice-second"})).id;
        assert(saw_busy && notifications == 2 && first.pane == 0);
        auto after_close = take(object::LuxObject::connect(
            root.get(),
            &ui::Root::objectRemoved,
            [&](object::LuxObject*) noexcept
            {
                assert(host.close(second_id));
                assert(second.pane == 0);
            }
        ));
        const auto first_batch = take(host.drain());
        assert(first_batch.completed == 1 && first_batch.pending == 2);
        assert(first.pane == 1 && second.pane == 0);
        after_close.disconnect();
        const auto second_batch = take(host.drain());
        assert(second_batch.completed == 1 && second_batch.stale == 1);
        assert(second.pane == 1 && notifications == 4);
    }
    void failedPrepare(object::ObjectDispatcherRef dispatcher)
    {
        auto root = take(ui::Root::create(dispatcher, {.attachment_capacity = 1}));
        Facts facts;
        desktop::ViewHost host(*root);
        auto view = candidate(dispatcher, "too-large", facts);
        const auto revision = root->windowRevision();
        const auto failed = host.adopt(view, views::ViewRestoreKey{"too-large"});
        assert(!failed && failed.error() == views::EViewError::CAPACITY);
        assert(root->windowRevision() == revision && root->panes().empty());
        assert(view.pane() && !facts.pane && take(host.describeAll()).empty());
    }
    void closeFailures(object::ObjectDispatcherRef dispatcher)
    {
        auto root = take(ui::Root::create(dispatcher));
        Facts facts;
        desktop::ViewHost host(*root);
        auto code = std::make_shared<Code>(facts);
        auto pane = std::make_unique<Window>(dispatcher, "close-errors", facts);
        auto& window = *pane;
        views::DetachedView view{
            lux::object::CodeLease::plugin(code),
            std::move(pane),
            +[](ui::Pane& target) { return static_cast<Window&>(target).prepareClose(); }
        };
        code.reset();
        const auto id = take(host.adopt(view, views::ViewRestoreKey{"close-errors"})).id;
        window.close_error = views::ViewPreparationFailure{"test.permission", 37, "Explicit refusal", false};
        assert(host.close(id));
        assert(take(host.drain()).pending == 0 && window.close_attempts == 1);
        const auto saved = take(host.closeFailure(id));
        assert(saved && saved->domain == "test.permission" && saved->code == 37 && !saved->retryable);
        for (int i{}; i != 10; ++i)
        {
            take(host.drain());
        }
        assert(window.close_attempts == 1 && facts.alive && !facts.pane);
        window.close_error = views::ViewPreparationFailure{"session", 9, "Temporarily reading", true};
        assert(host.close(id));
        assert(take(host.drain()).pending == 1 && window.close_attempts == 2);
        assert(take(host.drain()).pending == 1 && window.close_attempts == 3);
        window.close_error.reset();
        assert(take(host.drain()).completed == 1);
        assert(facts.pane == 1 && facts.element == 1 && facts.code == 1);
        assert(!host.closeFailure(id));
    }
    void reuseCapacity(object::ObjectDispatcherRef dispatcher)
    {
        auto root = take(ui::Root::create(dispatcher, {.docking = false}));
        desktop::ViewHost host(*root, {1, 4});
        views::ViewId previous;
        for (unsigned turn{}; turn < 64; ++turn)
        {
            Facts facts;
            auto next = candidate(dispatcher, "reused", facts);
            const auto id = take(host.adopt(next, views::ViewRestoreKey{"reused"})).id;
            assert(id != previous && !next.pane());
            if (turn)
            {
                assert(!host.describe(previous) && !host.close(previous));
            }
            assert(host.focus(id) && host.show(id) && host.close(id));
            assert(take(host.drain()).completed == 3);
            assert(facts.pane == 1 && facts.element == 1 && facts.code == 1);
            assert(take(host.describeAll()).empty());
            previous = id;
        }
    }
    void batchOwnership(object::ObjectDispatcherRef dispatcher)
    {
        auto root = take(ui::Root::create(dispatcher, {.docking = false}));
        Facts existing, first, second, rejected;
        desktop::ViewHost host(*root, {3, 16});
        auto initial = candidate(dispatcher, "batch-existing", existing);
        auto* window = initial.pane();
        auto id = take(host.adopt(initial, views::ViewRestoreKey{"batch-existing"})).id;
        assert(host.hide(id));
        take(host.drain());
        assert(!window->visible());
        std::array candidates{
            desktop::ViewCandidate{views::ViewRestoreKey{"batch-first"}, candidate(dispatcher, "batch-first", first)},
            desktop::ViewCandidate{views::ViewRestoreKey{"batch-second"}, candidate(dispatcher, "batch-second", second)}
        };
        std::array states{desktop::ViewVisibility{id, true}};
        auto prepared = take(host.prepareBatch(candidates, states));
        assert(!candidates[0].owner.pane() && !candidates[1].owner.pane());
        assert(take(host.describeAll()).size() == 1 && !window->visible());
        const std::vector new_ids(prepared.viewIds().begin(), prepared.viewIds().end());
        unsigned notices{};
        auto connected = take(object::LuxObject::connect(
            root.get(),
            &ui::Root::attachmentChanged,
            [&](const ui::AttachmentChanged& change) noexcept
            {
                if (!change.mounted)
                {
                    return;
                }
                ++notices;
                assert(root->findPane(ui::PaneIdView{"batch-first"}));
                assert(root->findPane(ui::PaneIdView{"batch-second"}));
                assert(window->visible());
                assert(!host.describeAll());
                assert(host.hide(id)); // Notification requests belong to the next drain.
            }
        ));
        assert(host.commit(prepared));
        assert(notices == 2 && window->visible() && take(host.describeAll()).size() == 3);
        assert(!host.commit(prepared));
        take(host.drain());
        assert(!window->visible());
        connected.disconnect();
        assert(host.close(new_ids[0]) && host.close(new_ids[1]));
        take(host.drain());
        assert(first.code == 1 && second.code == 1);
        {
            std::array next{
                desktop::ViewCandidate{views::ViewRestoreKey{"abandoned"}, candidate(dispatcher, "abandoned", rejected)}
            };
            auto pending = take(host.prepareBatch(next, states));
            // Changing an observed existing window invalidates preparation before any candidate mounts.
            window->setVisible(true);
            window->setVisible(false);
            assert(!host.commit(pending));
            assert(take(host.describeAll()).size() == 1 && !window->visible() && !rejected.pane);
        }
        assert(rejected.pane == 1 && rejected.element == 1 && rejected.code == 1);
    }
    void atomicClose(object::ObjectDispatcherRef dispatcher)
    {
        auto root = take(ui::Root::create(dispatcher));
        Facts first, second;
        desktop::ViewHost host(*root, {2, 8});
        auto make = [&](const char* name, Facts& facts)
        {
            auto code = std::make_shared<Code>(facts);
            return views::DetachedView{
                lux::object::CodeLease::plugin(code),
                std::make_unique<Window>(dispatcher, name, facts),
                +[](ui::Pane& pane) { return static_cast<Window&>(pane).prepareClose(); }
            };
        };
        auto a = make("close-a", first), b = make("close-b", second);
        auto* window = static_cast<Window*>(b.pane());
        const auto one = take(host.adopt(a, views::ViewRestoreKey{"close-a"})).id;
        const auto two = take(host.adopt(b, views::ViewRestoreKey{"close-b"})).id;
        const std::array ids{one, two};
        const auto windows = root->windowRevision();
        window->close_error = views::ViewPreparationFailure{"session", 1, "Reading", true};
        assert(!host.prepareClose(ids));
        assert(first.pane == 0 && second.pane == 0 && take(host.describeAll()).size() == 2);
        assert(root->windowRevision() == windows);
        window->close_error.reset();
        auto prepared = take(host.prepareClose(ids));
        assert(first.pane == 0 && second.pane == 0 && take(host.describeAll()).size() == 2);
        unsigned detached{};
        auto observer = take(object::LuxObject::connect(
            root.get(),
            &ui::Root::attachmentChanged,
            [&](const ui::AttachmentChanged& change) noexcept
            {
                if (change.mounted)
                {
                    return;
                }
                ++detached;
                assert(!root->findPane(ui::PaneIdView{"close-a"}) && !root->findPane(ui::PaneIdView{"close-b"}));
                assert(first.pane == 0 && second.pane == 0 && first.alive && second.alive);
                assert(!host.close(one) && !host.close(two)); // Both identities retired before notification.
            }
        ));
        unsigned handed_off{};
        const auto handoff = [&]() noexcept
        {
            ++handed_off;
            assert(detached == 2 && first.pane == 0 && second.pane == 0);
            assert(!host.describe(one) && !host.describe(two));
            assert(!host.drain()); // Cleanup stays inside the original Host dispatch.
        };
        assert(host.commitClose(prepared, handoff));
        assert(handed_off == 1 && detached == 2 && first.code == 1 && second.code == 1);
        assert(!host.commitClose(prepared, handoff) && handed_off == 1);
        assert(take(host.describeAll()).empty() && !host.commit(prepared));
    }
    void batchFailure(object::ObjectDispatcherRef dispatcher)
    {
        auto root = take(ui::Root::create(dispatcher, {.attachment_capacity = 3}));
        Facts first, second;
        desktop::ViewHost host(*root);
        std::array candidates{
            desktop::ViewCandidate{views::ViewRestoreKey{"a"}, candidate(dispatcher, "a", first)},
            desktop::ViewCandidate{views::ViewRestoreKey{"b"}, candidate(dispatcher, "b", second)}
        };
        const auto revision = root->windowRevision();
        auto failed = host.prepareBatch(candidates);
        assert(!failed && failed.error() == views::EViewError::CAPACITY);
        assert(root->windowRevision() == revision && root->panes().empty());
        assert(candidates[0].owner.pane() && candidates[1].owner.pane());
        assert(!first.pane && !second.pane && take(host.describeAll()).empty());
    }
    void review(object::ObjectDispatcherRef dispatcher)
    {
        using enum desktop::EReviewChoice;
        auto root = take(ui::Root::create(dispatcher));
        desktop::ViewHost host(*root);
        auto question =
            desktop::ReviewQuestion{91, "Unsaved scene", "Save this captured scene?", {SAVE, DISCARD, CANCEL}};
        auto view = take(desktop::ReviewView::create(dispatcher, ui::PaneId{"review"}, question));
        assert(view->modal() && !view->attachedRoot() && !view->response());
        views::DetachedView candidate{lux::object::CodeLease::builtin(), std::move(view)};
        auto id = take(host.adopt(candidate, views::ViewRestoreKey{"review"})).id;
        bool visited{};
        const auto borrow = [&](ui::Pane& pane)
        {
            visited = true;
            assert(pane.type() == ui::PaneTypeId{"lux.editor.review"});
            auto& modal = static_cast<desktop::ReviewView&>(pane);
            assert(modal.question().request == 91 && !modal.answer(KEEP_CONTENT));
            pane.requestClose(); // Native close means Cancel; it does not destroy the modal.
            assert(modal.response() && modal.response()->choice == CANCEL && modal.response()->request == 91);
            assert(!modal.answer(SAVE)); // First answer cannot be overwritten by late input.
            assert(host.close(id));
            assert(!host.drain()); // Callback cannot invalidate the current borrowed reference.
            const auto nested = [](ui::Pane&) {};
            auto recursive = host.withView(id, nested);
            assert(!recursive && recursive.error() == views::EViewError::BUSY);
        };
        assert(host.withView(id, borrow) && visited && host.describe(id));
        take(host.drain());
        visited = false;
        assert(!host.withView(id, borrow) && !visited);
        question.choices = {SAVE};
        assert(!desktop::ReviewView::create(dispatcher, ui::PaneId{"invalid"}, question));
        question.choices = {CANCEL, CANCEL};
        assert(!desktop::ReviewView::create(dispatcher, ui::PaneId{"invalid"}, question));
    }
} // namespace
void toolCommandFactory(object::ObjectDispatcherRef dispatcher)
{
    auto root = take(ui::Root::create(dispatcher));
    unsigned constructions{};
    services::ServiceRegistry services(dispatcher);
    auto scope = take(services.createScope());
    assert(scope.provide(services::ServiceNameView{"test.constructions"}, constructions));
    desktop::UiRegistry windows(dispatcher, services);
    std::string type{"p10.test"}, label{"External tool"};
    const services::ServiceDependency dependencies[]{
        {services::ServiceNameView{"test.constructions"},
         1,
         cxx::typeToken<unsigned>(),
         services::EDependencyKind::BORROWED}
    };
    desktop::UiDescriptor descriptor{views::ViewTypeIdView{type}, label, dependencies};
    descriptor.create = [](services::ServiceResolver& resolver,
                           const desktop::UiCreateInfo& input) -> desktop::UiResult<std::unique_ptr<ui::Pane>>
    {
        auto count = resolver.require<unsigned>(0);
        assert(count);
        ++count->get();
        return std::make_unique<ui::Pane>(input.dispatcher, input.instance, ui::PaneTypeId{"p10.test"}, "Tool");
    };
    auto factory = desktop::UiEntry::create(object::CodeLease::builtin(), descriptor);
    type.clear();
    label.assign(1000, 'x');
    const auto factories = take(desktop::UiCatalog::prepare({factory}));
    assert(windows.publish(factories));
    auto query = [](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState>
    { return commands::CommandState{true}; };
    auto open = [&](views::ViewTypeId id)
    { return desktop::showTool(*root, windows, scope, factories, std::move(id)); };
    auto entries = take(desktop::makeToolCommands(std::array{factory}, query, open));
    assert(entries.size() == 1 && entries.front()->descriptor().label == "External tool");
    commands::CommandRegistry registry;
    auto snapshot = take(commands::CommandRegistrySnapshot::create(std::move(entries)));
    assert(registry.publish(snapshot));
    const auto handle = take(snapshot.find(commands::CommandIdView{"lux.editor.tool/p10.test"}));
    commands::CommandInvocation input;
    assert(registry.execute(handle, input));
    const auto first = take(windows.describe(*root));
    assert(first.size() == 1 && constructions == 1);
    assert(registry.execute(handle, input));
    const auto second = take(windows.describe(*root));
    assert(second.size() == 1 && constructions == 1 && second.front().handle == first.front().handle);
    assert(!desktop::showTool(*root, windows, scope, factories, views::ViewTypeId{"absent"}));
    assert(!desktop::makeToolCommands(std::array<std::shared_ptr<const desktop::UiEntry>, 1>{}, query, open));
    auto close = desktop::makeCloseViewCommand(
        query,
        [&](ui::PaneHandle id) -> commands::CommandResult<void>
        {
            auto batch = windows.prepareClose(*root, std::span{&id, 1});
            if (!batch)
            {
                return cxx::unexpected(commands::CommandFailure{commands::ECommandError::STALE_TARGET});
            }
            auto committed = root->commit(*batch);
            assert(committed);
            return {};
        }
    );
    const auto closing = take(commands::CommandRegistrySnapshot::create({close}));
    auto target = commands::CommandInvocation::forView(first.front().handle, lux::object::CodeLease::builtin());
    assert(registry.execute(take(closing.at(0)), target));
    assert(take(windows.describe(*root)).empty());
    assert(!registry.execute(take(closing.at(0)), target));
    std::puts("PASS module tool command, frozen dynamic text, actual Root reuse/adoption, close and stale target");
}
int main()
{
    static_assert(!std::is_copy_constructible_v<desktop::ViewHost>);
    static_assert(!std::is_move_constructible_v<desktop::ViewHost>);
    auto queue = take(object::ObjectMessageQueue::create(32));
    toolCommandFactory(queue.dispatcherRef());
    completeViewConnections(queue.dispatcherRef());
    contentAssociations(queue.dispatcherRef());
    ownership(queue.dispatcherRef());
    reentrant(queue.dispatcherRef());
    failedPrepare(queue.dispatcherRef());
    reuseCapacity(queue.dispatcherRef());
    closeFailures(queue.dispatcherRef());
    batchOwnership(queue.dispatcherRef());
    batchFailure(queue.dispatcherRef());
    atomicClose(queue.dispatcherRef());
    review(queue.dispatcherRef());
    std::puts("PASS P10 real ViewHost ownership, bounded requests, generation, callback batches and prepare failure");
}
