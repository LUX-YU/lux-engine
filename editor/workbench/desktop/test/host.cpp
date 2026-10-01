#include <lux/engine/editor/desktop/ViewHost.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/ui/Element.hpp>
#include <cassert>
#include <algorithm>
#include <cstdio>
#include <array>

using namespace lux;
using namespace lux::editor;
namespace
{
    template <class T> auto take(T value)
    {
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
            setContent(body);
        }
        ~Window() override
        {
            assert(facts_.alive);
            ++facts_.pane;
        }
        Content body;
        unsigned close_attempts{};
        std::optional<views::ViewCloseFailure> close_error;
        views::ViewCloseResult prepareClose()
        {
            ++close_attempts;
            if (close_error)
                return cxx::unexpected(*close_error);
            return {};
        }

    private:
        Facts& facts_;
    };
    views::DetachedView candidate(object::ObjectDispatcherRef dispatcher, const char* id, Facts& facts)
    {
        auto code = std::make_shared<Code>(facts);
        return {contracts::CodeLease::plugin(code), std::make_unique<Window>(dispatcher, id, facts)};
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
            [&](const ui::AttachmentChanged& changed) noexcept {
                ++notifications;
                const auto all = host.describeAll();
                const auto nested = host.drain();
                assert(!all && all.error() == views::EViewError::BUSY);
                assert(!nested && nested.error() == views::EViewError::BUSY);
                saw_busy = true;
                if (changed.mounted && changed.pane == ui::PaneId{"notice-second"})
                    assert(host.close(first_id));
            }
        ));
        first_id = take(host.adopt(a, views::ViewRestoreKey{"notice-first"})).id;
        const auto second_id = take(host.adopt(b, views::ViewRestoreKey{"notice-second"})).id;
        assert(saw_busy && notifications == 2 && first.pane == 0);
        auto after_close =
            take(object::LuxObject::connect(root.get(), &ui::Root::objectRemoved, [&](object::LuxObject*) noexcept {
                assert(host.close(second_id));
                assert(second.pane == 0);
            }));
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
        views::DetachedView view{contracts::CodeLease::plugin(code), std::move(pane), +[](ui::Pane& target) {
                                     return static_cast<Window&>(target).prepareClose();
                                 }};
        code.reset();
        const auto id = take(host.adopt(view, views::ViewRestoreKey{"close-errors"})).id;
        window.close_error = views::ViewCloseFailure{"test.permission", 37, "Explicit refusal", false};
        assert(host.close(id));
        assert(take(host.drain()).pending == 0 && window.close_attempts == 1);
        const auto saved = take(host.closeFailure(id));
        assert(saved && saved->domain == "test.permission" && saved->code == 37 && !saved->retryable);
        for (int i{}; i != 10; ++i)
            take(host.drain());
        assert(window.close_attempts == 1 && facts.alive && !facts.pane);
        window.close_error = views::ViewCloseFailure{"session", 9, "Temporarily reading", true};
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
                assert(!host.describe(previous) && !host.close(previous));
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
        const std::vector new_ids(prepared.created().begin(), prepared.created().end());
        unsigned notices{};
        auto connected = take(object::LuxObject::connect(
            root.get(),
            &ui::Root::attachmentChanged,
            [&](const ui::AttachmentChanged& change) noexcept {
                if (!change.mounted)
                    return;
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
}
int main()
{
    static_assert(!std::is_copy_constructible_v<desktop::ViewHost>);
    static_assert(!std::is_move_constructible_v<desktop::ViewHost>);
    auto queue = take(object::ObjectMessageQueue::create(32));
    ownership(queue.dispatcherRef());
    reentrant(queue.dispatcherRef());
    failedPrepare(queue.dispatcherRef());
    reuseCapacity(queue.dispatcherRef());
    closeFailures(queue.dispatcherRef());
    batchOwnership(queue.dispatcherRef());
    batchFailure(queue.dispatcherRef());
    std::puts("PASS P10 real ViewHost ownership, bounded requests, generation, callback batches and prepare failure");
}
