#include <lux/engine/editor/views/IViewHost.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <array>
#include <cassert>
#include <cstdio>
#include <optional>
#include <type_traits>

using namespace lux;
using namespace lux::editor::views;
static_assert(!std::is_copy_constructible_v<DetachedView> && !std::is_copy_assignable_v<DetachedView>);
static_assert(std::is_nothrow_move_constructible_v<DetachedView> && std::is_nothrow_move_assignable_v<DetachedView>);
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
        ViewRequests* requests{};
        ViewId target;

    private:
        void draw() noexcept override
        {
            ++facts_.draws;
            if (requests)
            {
                assert(requests->close(target));
                requests = nullptr;
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
            setContent(content);
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
    DetachedView candidate(object::ObjectDispatcherRef dispatcher, Facts& facts, const char* name)
    {
        auto code = std::make_shared<Code>(facts);
        return {lux::object::CodeLease::plugin(code), std::make_unique<Window>(dispatcher, facts, name)};
    }
    // Protocol fixture only: bounded identities and real Root transactions, not a desktop product host.
    class FakeHost final : public IViewHost
    {
    public:
        explicit FakeHost(ui::Root& root) : root_(root) {}
        ~FakeHost() override
        {
            for (auto& slot : slots_)
                if (slot.view)
                    detach(slot);
        }
        FakeHost(const FakeHost&) = delete;
        FakeHost& operator=(const FakeHost&) = delete;
        FakeHost(FakeHost&&) = delete;
        FakeHost& operator=(FakeHost&&) = delete;
        bool owner_capacity{true}, notification_capacity{true};
        ViewResult<ViewId> attach(DetachedView& input)
        {
            if (!owner_capacity || !notification_capacity)
                return cxx::unexpected(EViewError::CAPACITY);
            for (std::size_t i{}; i != slots_.size(); ++i)
            {
                auto& slot = slots_[i];
                if (slot.view)
                    continue;
                auto prepared = root_.prepareMount(*input.pane());
                if (!prepared)
                    return cxx::unexpected(EViewError::BUSY);
                slot.view.emplace(std::move(input));
                const ViewId id{71, static_cast<std::uint32_t>(i), ++slot.generation};
                const auto committed = root_.commit(*prepared);
                assert(committed && committed->mounted);
                return id;
            }
            return cxx::unexpected(EViewError::CAPACITY);
        }
        ViewResult<ViewInfo> describe(ViewId id) const override
        {
            const auto* slot = find(id);
            if (!slot)
                return cxx::unexpected(EViewError::INVALID_ID);
            auto* pane = slot->view->pane();
            return ViewInfo{
                id,
                pane->type(),
                ViewRestoreKey{"restore-test"},
                std::string(pane->title()),
                pane->visible(),
                pane->focused()
            };
        }
        ViewResult<void> close(ViewId id) noexcept override
        {
            return request(id, EAction::CLOSE);
        }
        ViewResult<void> show(ViewId id) noexcept override
        {
            return request(id, EAction::SHOW);
        }
        ViewResult<void> focus(ViewId id) noexcept override
        {
            return request(id, EAction::FOCUS);
        }
        void drain()
        {
            const auto batch = requests_;
            const auto count = std::exchange(size_, 0);
            for (std::size_t i{}; i != count; ++i)
            {
                auto* slot = find(batch[i].id);
                if (!slot)
                    continue;
                if (batch[i].action == EAction::CLOSE)
                    detach(*slot);
                else if (batch[i].action == EAction::SHOW)
                    slot->view->pane()->setVisible(true);
                else
                    assert(root_.requestFocus(*slot->view->pane()));
            }
        }

    private:
        struct Slot final
        {
            std::uint64_t generation{};
            std::optional<DetachedView> view;
        };
        enum class EAction : std::uint8_t
        {
            CLOSE,
            SHOW,
            FOCUS
        };
        struct Request final
        {
            ViewId id;
            EAction action{};
        };
        Slot* find(ViewId id) noexcept
        {
            if (id.domain != 71 || id.slot >= slots_.size())
                return nullptr;
            auto& slot = slots_[id.slot];
            return slot.view && slot.generation == id.generation ? &slot : nullptr;
        }
        const Slot* find(ViewId id) const noexcept
        {
            return const_cast<FakeHost*>(this)->find(id);
        }
        ViewResult<void> request(ViewId id, EAction action) noexcept
        {
            if (!find(id))
                return cxx::unexpected(EViewError::INVALID_ID);
            if (size_ == requests_.size())
                return cxx::unexpected(EViewError::CAPACITY);
            requests_[size_++] = {id, action};
            return {};
        }
        void detach(Slot& slot)
        {
            auto prepared = take(root_.prepareDetach(*slot.view->pane()));
            auto result = take(root_.commit(prepared));
            assert(!result.mounted);
            slot.view.reset();
        }
        ui::Root& root_;
        std::array<Slot, 2> slots_;
        std::array<Request, 8> requests_;
        std::size_t size_{};
    };
    class Standalone final : public ui::Element
    {
    public:
        using Element::Element;

    private:
        void draw() noexcept override {}
    };
    void protocol(object::ObjectDispatcherRef dispatcher)
    {
        auto root = take(ui::Root::create(dispatcher, {.docking = false}));
        const auto revision = root->windowRevision();
        Facts first;
        auto view = candidate(dispatcher, first, "detached");
        auto* pane = static_cast<Window*>(view.pane());
        assert(!pane->attachedRoot() && !pane->content.attachedRoot());
        assert(!pane->focused() && !pane->content.focused());
        assert(!root->requestFocus(*pane) && !root->requestFocus(pane->content));
        assert(!root->capturePointer(pane->content));
        pane->setTitle("configured off tree");
        pane->setModal(false);
        assert(root->windowRevision() == revision && root->panes().empty());
        FakeHost host(*root);
        host.owner_capacity = false;
        assert(!host.attach(view) && view.pane() == pane);
        host.owner_capacity = true;
        host.notification_capacity = false;
        assert(!host.attach(view) && root->windowRevision() == revision);
        host.notification_capacity = true;
        auto id = take(host.attach(view));
        assert(!view.pane());
        assert(pane->attachedRoot() == root.get() && pane->content.attachedRoot() == root.get());
        assert(root->findPane(pane->id().view()) == pane);
        assert(root->capturePointer(pane->content));
        pane->content.requests = &host;
        pane->content.target = id;
        ui::DrawData data;
        assert(root->update({{640, 480}, .016F}, &data));
        assert(first.draws && !first.panes && host.describe(id));
        host.drain();
        assert(first.panes == 1 && first.elements == 1 && first.code == 1);
        assert(!host.describe(id));
        assert(!root->focusedPane() && !root->focusedElement());
        Facts replacement;
        auto next = candidate(dispatcher, replacement, "replacement");
        const auto next_id = take(host.attach(next));
        assert(next_id.slot == id.slot && next_id.generation != id.generation);
        assert(!host.close(id));
        assert(host.describe(next_id));
        assert(host.close(next_id));
        host.drain();
        assert(replacement.panes == 1 && replacement.code == 1);
    }
    void preparation(object::ObjectDispatcherRef dispatcher)
    {
        auto root = take(ui::Root::create(dispatcher, {.docking = false, .attachment_capacity = 1}));
        Facts a, b;
        auto first = candidate(dispatcher, a, "one");
        assert(!root->prepareMount(*first.pane()));
        assert(root->panes().empty() && a.panes == 0);
        // Replacement must destroy all old nodes before the last old code owner.
        auto second = candidate(dispatcher, b, "two");
        first = std::move(second);
        assert(a.panes == 1 && a.elements == 1 && a.code == 1);
        assert(!second.pane() && b.panes == 0);
        auto roomy = take(ui::Root::create(dispatcher, {.docking = false}));
        auto pending = take(roomy->prepareMount(*first.pane()));
        first.pane()->setModal(true); // Local structure/configuration invalidates the prepared association.
        auto stale = roomy->commit(pending);
        assert(!stale && stale.error() == ui::EAttachmentError::STALE_PREPARATION);
        assert(!first.pane()->attachedRoot());
        {
            auto orphan = take(ui::Root::create(dispatcher));
            Facts c;
            auto input = candidate(dispatcher, c, "candidate-lifetime");
            auto prepared = take(orphan->prepareMount(*input.pane()));
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
    void notifications(object::ObjectDispatcherRef dispatcher)
    {
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
        assert(object::detail::post(messages.dispatcherRef(), object::detail::makeMessage([]() noexcept {
                                    })) == object::detail::EPostStatus::POSTED);
        Facts facts;
        auto view = candidate(dispatcher, facts, "notifications");
        auto prepared = take(root->prepareMount(*view.pane()));
        unsigned calls{};
        auto direct = take(object::LuxObject::connect(
            root.get(),
            &ui::Root::attachmentChanged,
            [&](const ui::AttachmentChanged& changed) noexcept {
                ++calls;
                assert(changed.mounted == (view.pane()->attachedRoot() == root.get()));
                auto recursive = root->prepareDetach(*view.pane());
                assert(!recursive && recursive.error() == ui::EAttachmentError::BUSY);
                assert(!facts.panes);
            }
        ));
        auto mounted = take(root->commit(prepared));
        assert(mounted.mounted && mounted.notifications.full == 1 && mounted.notifications.direct == 1);
        assert(view.pane()->attachedRoot() == root.get() && calls == 1);
        assert(messages.dispatchPending() == 1 && receiver.calls == 0);
        std::optional<ui::PreparedAttachment> detach{take(root->prepareDetach(*view.pane()))};
        unsigned removed{};
        auto release_token =
            take(object::LuxObject::connect(root.get(), &ui::Root::objectRemoved, [&](object::LuxObject*) noexcept {
                ++removed;
                detach.reset(); // The commit now owns its plan until every post-commit callback returns.
            }));
        assert(root->commit(*detach));
        assert(!detach && removed == 2);
        release_token.disconnect();
        assert(calls == 2 && !view.pane()->attachedRoot());
        assert(messages.dispatchPending() == 1 && receiver.calls == 1);
        FakeHost host(*root);
        auto connection = take(object::LuxObject::connect(view.pane(), &ui::Pane::closeRequested, [&]() noexcept {
            const auto expected = ViewId{71, 0, 1};
            assert(host.close(expected));
            assert(!facts.panes);
        }));
        direct.disconnect();
        const auto id = take(host.attach(view));
        root->findPane(ui::PaneId{"notifications"}.view())->requestClose();
        assert(host.describe(id) && !facts.panes);
        host.drain();
        assert(!host.describe(id) && facts.panes == 1 && facts.code == 1);
    }
    class PartialChild final : public ui::Element
    {
    public:
        PartialChild(ui::Pane& parent, int& live, bool reject)
            : Element(parent, ui::ElementId{reject ? "rejected-child" : "kept-child"}), live_(live)
        {
            if (reject)
                throw 42; // Foreign constructor failure, test-only fault injection.
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
        {}

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
        const auto factory = [&]() -> ViewResult<DetachedView> {
            auto partial = candidate(dispatcher, failed, "partial");
            // A later fallible child/provider step rejects the fully owned partial subtree.
            return cxx::unexpected(EViewError::CAPACITY);
        };
        assert(!factory());
        assert(failed.panes == 1 && failed.elements == 1 && failed.code == 1);
        assert(root->panes().empty() && root->windowRevision() == revision);
        ui::Pane standalone(dispatcher, ui::PaneId{"standalone"}, ui::PaneTypeId{"test"}, "standalone");
        Standalone layout(dispatcher, ui::ElementId{"standalone-element"});
        Standalone child(dispatcher, ui::ElementId{"standalone-child"});
        layout.addChild(child);
        standalone.setContent(layout);
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
}
int main()
{
    auto messages = take(object::ObjectMessageQueue::create(32));
    protocol(messages.dispatcherRef());
    preparation(messages.dispatcherRef());
    construction(messages.dispatcherRef());
    notifications(messages.dispatcherRef());
    std::puts("PASS X08-03..06 real detached tree, preparation, callback close, IDs, code lifetime and remount");
}
