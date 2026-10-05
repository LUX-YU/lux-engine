#include <array>
#include <cassert>
#include <cstdio>
#include <lux/engine/editor/views/ViewInfo.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/object/ObjectOwnership.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>
#include <optional>
#include <type_traits>

using namespace lux;
using namespace lux::editor::views;
using PaneOwner = std::unique_ptr<ui::Pane, object::ObjectDeleter>;
static_assert(!std::is_copy_constructible_v<PaneOwner> && !std::is_copy_assignable_v<PaneOwner>);
static_assert(std::is_nothrow_move_constructible_v<PaneOwner> && std::is_nothrow_move_assignable_v<PaneOwner>);
static_assert(!std::is_copy_constructible_v<ui::PreparedAttachment> && !std::is_copy_assignable_v<ui::PreparedAttachment>);
static_assert(std::is_nothrow_move_constructible_v<ui::PreparedAttachment>);
static_assert(std::is_nothrow_move_assignable_v<ui::PreparedAttachment>);
static_assert(!std::same_as<ViewRestoreKey, ui::PaneId>);
static_assert(std::same_as<ViewTypeId, ui::PaneTypeId>);
namespace
{
    template <class T> auto take(T result)
    {
        assert(result);
        return std::move(*result);
    }
    struct Facts final
    {
        bool code_alive{true};
        unsigned panes{}, elements{}, code{}, draws{}, events{};
    };
    struct Code final
    {
        Facts& facts;
        explicit Code(Facts& value) : facts(value) {}
        ~Code()
        {
            assert(facts.panes == 1 && facts.elements == 1);
            facts.code_alive = false;
            ++facts.code;
        }
    };
    class Content final : public ui::Element
    {
    public:
        Content(ui::Pane& parent, Facts& facts) : Element(parent, ui::ElementId{"content"}), facts_(facts) {}
        ~Content() override
        {
            assert(facts_.code_alive);
            ++facts_.elements;
        }
        bool close_on_draw{};

    private:
        void draw() noexcept override
        {
            ++facts_.draws;
            if (std::exchange(close_on_draw, false))
            {
                containingPane()->requestClose();
                assert(facts_.panes == 0);
            }
        }
        Facts& facts_;
    };
    class Window final : public ui::Pane
    {
    public:
        Window(object::ObjectDispatcherRef dispatcher, Facts& facts, const char* name)
            : Pane(std::move(dispatcher), ui::PaneId{name}, ui::PaneTypeId{"test.p08"}, name), content(*this, facts),
              facts_(facts)
        {
            assert(setContent(content));
        }
        ~Window() override
        {
            assert(facts_.code_alive);
            ++facts_.panes;
        }
        Content content;

    private:
        Facts& facts_;
    };
    PaneOwner candidate(object::ObjectDispatcherRef dispatcher, Facts& facts, const char* name)
    {
        auto code = object::CodeLease::plugin(std::make_shared<Code>(facts));
        auto pane = std::make_unique<Window>(dispatcher, facts, name);
        auto deleter = object::ObjectDeleter::create<Window>(std::default_delete<Window>{}, std::move(code));
        return {pane.release(), std::move(deleter)};
    }
    class Standalone final : public ui::Element
    {
    public:
        using Element::Element;

    private:
        void draw() noexcept override {}
    };
    void protocol(object::ObjectMessageQueue& messages)
    {
        const auto dispatcher = messages.dispatcherRef();
        auto root = take(ui::Root::create(dispatcher, {.docking = false}));
        const auto revision = root->windowRevision();
        Facts first;
        auto view = candidate(dispatcher, first, "detached");
        auto* pane = static_cast<Window*>(view.get());
        assert(!pane->attachedRoot() && !pane->content.attachedRoot());
        assert(!pane->focused() && !pane->content.focused());
        assert(!root->requestFocus(*pane) && !root->requestFocus(pane->content));
        assert(!root->capturePointer(pane->content));
        pane->setTitle("configured off tree");
        pane->setModal(false);
        assert(root->windowRevision() == revision && root->panes().empty());
        auto limited = take(ui::Root::create(dispatcher, {.docking = false, .attachment_capacity = 1}));
        const auto full = limited->addSubPane(std::move(view));
        assert(!full && full.error() == ui::EAttachmentError::CAPACITY && view.get() == pane);
        assert(limited->panes().empty() && !first.panes && root->windowRevision() == revision);
        // Real registration refusal, not a fake host flag. Both original tree and candidate survive.
        ui::Pane occupied(dispatcher, ui::PaneId{"detached"}, ui::PaneTypeId{"test"}, "Occupied");
        assert(root->addSubPane(occupied));
        const auto occupied_revision = root->windowRevision();
        const auto duplicate = root->addSubPane(std::move(view));
        assert(!duplicate && duplicate.error() == ui::EAttachmentError::DUPLICATE_ID && view.get() == pane);
        assert(root->windowRevision() == occupied_revision && root->panes().size() == 1);
        assert(root->removeSubPane(occupied));
        assert(root->addSubPane(std::move(view)) && !view);
        const auto id = take(root->identify(*pane));
        assert(pane->ownership() == object::EObjectOwnership::PARENT_OWNED);
        assert(pane->attachedRoot() == root.get() && pane->content.attachedRoot() == root.get());
        assert(root->findPane(pane->id().view()) == pane);
        assert(root->capturePointer(pane->content));
        std::optional<ui::PaneHandle> pending;
        auto close = take(object::LuxObject::connect(
            pane,
            &ui::Pane::closeRequested,
            [&]() noexcept
            {
                pending = id;
                const auto recursive = root->prepareDetach(*pane);
                assert(!recursive && recursive.error() == ui::EAttachmentError::BUSY);
                assert(!first.panes);
            }
        ));
        pane->content.close_on_draw = true;
        ui::DrawData data;
        assert(root->update({{640, 480}, .016F}, &data));
        assert(first.draws && !first.panes && pending == id && root->findPane(id));
        auto detach = take(root->prepareDetach(*take(root->findPane(*pending))));
        assert(root->commit(detach));
        assert(!root->findPane(id) && !first.panes); // Routing revoked; destructor waits for owner safe point.
        assert(messages.collectRetired() == 1);
        assert(first.panes == 1 && first.elements == 1 && first.code == 1);
        assert(!root->focusedPane() && !root->focusedElement());
        Facts replacement;
        auto next = candidate(dispatcher, replacement, "detached");
        auto* replacement_pane = next.get();
        assert(root->addSubPane(std::move(next)));
        const auto next_id = take(root->identify(*replacement_pane));
        assert(next_id != id && !root->findPane(id));
        assert(root->findPane(next_id));
        assert(root->removeSubPane(*replacement_pane));
        assert(!root->findPane(next_id) && !replacement.panes);
        assert(messages.collectRetired() == 1 && replacement.panes == 1 && replacement.code == 1);
    }
    void preparation(object::ObjectDispatcherRef dispatcher)
    {
        auto root = take(ui::Root::create(dispatcher, {.docking = false, .attachment_capacity = 1}));
        Facts a, b;
        auto first = candidate(dispatcher, a, "one");
        assert(!root->prepareMount(*first.get()));
        assert(root->panes().empty() && a.panes == 0);
        // Replacement must destroy all old nodes before the last old code owner.
        auto second = candidate(dispatcher, b, "two");
        first = std::move(second);
        assert(a.panes == 1 && a.elements == 1 && a.code == 1);
        assert(!second.get() && b.panes == 0);
        auto roomy = take(ui::Root::create(dispatcher, {.docking = false}));
        auto pending = take(roomy->prepareMount(*first.get()));
        first.get()->setModal(true); // Local structure/configuration invalidates the prepared association.
        auto stale = roomy->commit(pending);
        assert(!stale && stale.error() == ui::EAttachmentError::STALE_PREPARATION);
        assert(!first.get()->attachedRoot());
        {
            auto orphan = take(ui::Root::create(dispatcher));
            Facts c;
            auto input = candidate(dispatcher, c, "candidate-lifetime");
            auto prepared = take(orphan->prepareMount(*input.get()));
            orphan.reset(); // Token destruction does not dereference a dead Root.
        }
    }
    class NoticeReceiver final : public object::LuxObject
    {
    public:
        using LuxObject::LuxObject;
        unsigned calls{};
        void receive(const ui::AttachmentChanged&) noexcept
        {
            ++calls;
        }
    };
    void notifications(object::ObjectMessageQueue& owner)
    {
        const auto dispatcher = owner.dispatcherRef();
        auto messages = take(object::ObjectMessageQueue::create(1));
        NoticeReceiver receiver(messages.dispatcherRef());
        auto root = take(ui::Root::create(dispatcher));
        auto queued = take(object::LuxObject::connect(
            root.get(),
            &ui::Root::attachmentChanged,
            &receiver,
            &NoticeReceiver::receive,
            object::EDelivery::QUEUED
        ));
        assert(
            object::detail::post(messages.dispatcherRef(), object::detail::makeMessage([]() noexcept {})) ==
            object::detail::EPostStatus::POSTED
        );
        Facts facts;
        auto view = candidate(dispatcher, facts, "notifications");
        auto prepared = take(root->prepareMount(*view.get()));
        unsigned calls{};
        auto direct = take(object::LuxObject::connect(
            root.get(),
            &ui::Root::attachmentChanged,
            [&](const ui::AttachmentChanged& changed) noexcept
            {
                ++calls;
                assert(changed.mounted == (view.get()->attachedRoot() == root.get()));
                auto recursive = root->prepareDetach(*view.get());
                assert(!recursive && recursive.error() == ui::EAttachmentError::BUSY);
                assert(!facts.panes);
            }
        ));
        auto mounted = take(root->commit(prepared));
        assert(mounted.mounted && mounted.notifications.full == 1 && mounted.notifications.direct == 1);
        assert(view.get()->attachedRoot() == root.get() && calls == 1);
        assert(messages.dispatchPending() == 1 && receiver.calls == 0);
        std::optional<ui::PreparedAttachment> detach{take(root->prepareDetach(*view.get()))};
        unsigned removed{};
        auto release_token = take(object::LuxObject::connect(
            root.get(),
            &ui::Root::objectRemoved,
            [&](object::LuxObject*) noexcept
            {
                ++removed;
                detach.reset(); // The commit now owns its plan until every post-commit callback returns.
            }
        ));
        assert(root->commit(*detach));
        assert(!detach && removed == 2);
        release_token.disconnect();
        assert(calls == 2 && !view.get()->attachedRoot());
        assert(messages.dispatchPending() == 1 && receiver.calls == 1);
        std::optional<ui::PaneHandle> pending;
        auto* pane = view.get();
        auto connection = take(object::LuxObject::connect(
            pane,
            &ui::Pane::closeRequested,
            [&]() noexcept
            {
                pending = take(root->identify(*pane));
                assert(!facts.panes);
            }
        ));
        direct.disconnect();
        assert(root->addSubPane(std::move(view)));
        const auto id = take(root->identify(*pane));
        pane->requestClose();
        assert(pending == id && root->findPane(id) && !facts.panes);
        auto removal = take(root->prepareDetach(*take(root->findPane(*pending))));
        assert(root->commit(removal));
        assert(!root->findPane(id) && !facts.panes);
        assert(owner.collectRetired() == 1 && facts.panes == 1 && facts.code == 1);
    }
    class PartialChild final : public ui::Element
    {
    public:
        PartialChild(ui::Pane& parent, int& live, bool reject)
            : Element(parent, ui::ElementId{reject ? "rejected-child" : "kept-child"}), live_(live)
        {
            if (reject)
            {
                throw 42; // Foreign constructor failure, test-only fault injection.
            }
            ++live_;
        }
        ~PartialChild() override
        {
            --live_;
        }

    private:
        void draw() noexcept override {}
        int& live_;
    };
    class PartialWindow final : public ui::Pane
    {
    public:
        PartialWindow(object::ObjectDispatcherRef dispatcher, int& live)
            : Pane(dispatcher, ui::PaneId{"partial-constructor"}, ui::PaneTypeId{"test"}, "partial"),
              first_(*this, live, false), rejected_(*this, live, true)
        {
        }

    private:
        PartialChild first_, rejected_;
    };
    void construction(object::ObjectDispatcherRef dispatcher)
    {
        auto root = take(ui::Root::create(dispatcher));
        const auto revision = root->windowRevision();
        int live{};
        bool rejected{};
        try
        {
            auto partial = std::make_unique<PartialWindow>(dispatcher, live);
        }
        catch (int code)
        {
            assert(code == 42);
            rejected = true;
        }
        assert(rejected && live == 0 && root->panes().empty() && root->windowRevision() == revision);
        Facts failed;
        const auto factory = [&]() -> cxx::expected<PaneOwner, ui::EAttachmentError>
        {
            auto partial = candidate(dispatcher, failed, "partial");
            // A later fallible child/provider step rejects the fully owned partial subtree.
            return cxx::unexpected(ui::EAttachmentError::CAPACITY);
        };
        assert(!factory());
        assert(failed.panes == 1 && failed.elements == 1 && failed.code == 1);
        assert(root->panes().empty() && root->windowRevision() == revision);
        ui::Pane standalone(dispatcher, ui::PaneId{"standalone"}, ui::PaneTypeId{"test"}, "standalone");
        ui::Layout layout(dispatcher, ui::ElementId{"standalone-element"});
        Standalone child(dispatcher, ui::ElementId{"standalone-child"});
        assert(layout.addSubElement(child));
        assert(standalone.setContent(layout));
        assert(layout.containingPane() == &standalone && child.containingPane() == &standalone);
        auto mount = take(root->prepareMount(standalone));
        auto moved = std::move(mount);
        assert(root->commit(moved));
        assert(!root->commit(moved));
        auto detach = take(root->prepareDetach(standalone));
        assert(root->commit(detach));
        assert(!standalone.parent() && !child.attachedRoot());
        auto remount = take(root->prepareMount(standalone));
        assert(root->commit(remount));
        auto remove = take(root->prepareDetach(standalone));
        assert(root->commit(remove));
        for (int i = 0; i != 64; ++i)
        {
            auto next = take(root->prepareMount(standalone));
            assert(root->commit(next) && root->panes().size() == 1);
            auto retired = take(root->prepareDetach(standalone));
            assert(root->commit(retired));
        }
    }
} // namespace
int main()
{
    auto messages = take(object::ObjectMessageQueue::create(32));
    protocol(messages);
    preparation(messages.dispatcherRef());
    construction(messages.dispatcherRef());
    notifications(messages);
    std::puts("PASS X08-03..06 real detached tree, preparation, callback close, IDs, code lifetime and remount");
}
