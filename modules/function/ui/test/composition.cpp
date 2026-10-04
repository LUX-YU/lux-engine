#include <lux/engine/ui/Root.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <cassert>
#include <array>
#include <iostream>
#include <thread>
#include <new>

namespace
{
    using namespace lux;
    struct Counts final
    {
        unsigned destroyed{}, deleted{}, callbacks{};
    };
    class Item final : public ui::Element
    {
    public:
        Item(object::ObjectDispatcherRef queue, const char* id, Counts& counts)
            : Element(std::move(queue), ui::ElementId{id}), counts_(counts)
        {}
        ~Item() override { ++counts_.destroyed; }
        void (*on_draw)(Item&) noexcept {};
        void (*on_measure)(Item&) noexcept {};
        void (*on_event)(Item&) noexcept {};
        ui::Pane* window{};
        ui::Element* candidate{};
        unsigned draws{}, measurements{}, events{};
    private:
        void draw() noexcept override { ++draws; if (on_draw) on_draw(*this); }
        ui::SizeHint measureContent(float) noexcept override
        {
            ++measurements;
            if (on_measure) on_measure(*this);
            return {{10, 10}, {40, 40}};
        }
        void event(object::EventView&) noexcept override { ++events; if (on_event) on_event(*this); }
        Counts& counts_;
    };
    class Window final : public ui::Pane
    {
    public:
        Window(object::ObjectDispatcherRef queue, const char* id, Counts& counts)
            : Pane(std::move(queue), ui::PaneId{id}, ui::PaneTypeId{"test.window"}, id), counts_(counts)
        {}
        ~Window() override { clearChildren(); ++counts_.destroyed; }
        void (*on_update)(Window&) noexcept {};
        object::ObjectMessageQueue* messages{};
        void receive(const unsigned&) noexcept { ++counts_.callbacks; }
        bool dispatching() const noexcept { return isDispatching(); }
    private:
        void update() noexcept override { if (on_update) on_update(*this); }
        Counts& counts_;
    };
    struct DeleteWindow final
    {
        Counts* counts{};
        ui::Root* root{};
        ui::Pane* candidate{};
        bool* moving{};
        DeleteWindow(Counts& value) noexcept : counts(&value) {}
        DeleteWindow(DeleteWindow&& other) noexcept
            : counts(other.counts), root(other.root), candidate(other.candidate), moving(other.moving)
        {
            if (moving && *moving)
            {
                auto attempt = root->addSubPane(*candidate);
                assert(!attempt && attempt.error() == ui::EAttachmentError::BUSY);
                ++counts->callbacks;
            }
        }
        void operator()(Window* value) noexcept { ++counts->deleted; delete value; }
    };
    template<class T> concept PublicChildren = requires(T& parent, ui::Element& child)
    {
        parent.addSubElement(child);
    };
    static_assert(PublicChildren<ui::Layout>);
    static_assert(!PublicChildren<ui::Button>);
    static_assert(!std::derived_from<ui::Pane, ui::Element>);

    void externalRoot(object::ObjectMessageQueue& messages)
    {
        Counts counts;
        auto root = ui::Root::create(messages.dispatcherRef());
        assert(root);
        Window pane(messages.dispatcherRef(), "external", counts);
        ui::Layout layout(messages.dispatcherRef(), ui::ElementId{"layout"});
        ui::Button button(messages.dispatcherRef(), ui::ElementId{"button"}, "Button");
        assert(layout.addSubElement(button));
        assert(pane.setContent(layout));
        assert((*root)->addSubPane(pane));
        const auto identity = (*root)->identify(pane);
        assert(identity && *(*root)->findPane(*identity) == &pane);
        assert((*root)->requestFocus(button) && (*root)->capturePointer(button));
        (*root)->deferChange(button, [](object::LuxObject&) noexcept { std::abort(); });
        root->reset();
        assert(!pane.parent() && !pane.attachedRoot() && !button.attachedRoot());
        assert(button.parent() == &layout && layout.parent() == &pane && pane.content() == &layout);
        auto replacement = ui::Root::create(messages.dispatcherRef());
        assert(replacement && (*replacement)->addSubPane(pane));
        assert(!(*replacement)->findPane(*identity));
        const auto remounted = (*replacement)->identify(pane);
        assert(remounted && *remounted != *identity);
        assert((*replacement)->update({}, nullptr));
        assert((*replacement)->removeSubPane(pane));
        assert(!(*replacement)->findPane(*remounted));
        assert((*replacement)->addSubPane(pane));
        assert(!(*replacement)->findPane(*remounted)); // Same live object and Root, new attachment.
        assert(*(*replacement)->findPane(*(*replacement)->identify(pane)) == &pane);
        std::thread other(
            [&]
            {
                auto lookup = (*replacement)->findPane(*remounted);
                assert(!lookup && lookup.error() == ui::EAttachmentError::WRONG_THREAD);
            }
        );
        other.join();
        assert((*replacement)->removeSubPane(pane));
        assert(!pane.parent() && !button.attachedRoot());
        std::cout << "UI external subtree survives Root, revoked routes and pending input, reattach PASS\n";
    }

    void ownership(object::ObjectMessageQueue& messages)
    {
        Counts counts;
        auto root = ui::Root::create(messages.dispatcherRef());
        assert(root);
        Item external(messages.dispatcherRef(), "external-item", counts);
        auto pane = std::make_unique<Window>(messages.dispatcherRef(), "owned", counts);
        auto layout = std::make_unique<ui::Layout>(messages.dispatcherRef(), ui::ElementId{"layout"});
        assert(layout->addSubElement(external));
        auto* layout_ptr = layout.get();
        assert(pane->setContent(std::move(layout)) && !layout);
        assert((*root)->addSubPane(std::move(pane)) && !pane);
        assert(external.parent() == layout_ptr && external.attachedRoot() == root->get());
        root->reset();
        assert(counts.destroyed == 1 && !external.parent() && !external.containingPane() && !external.attachedRoot());
        std::cout << "UI owned subtree reclaims once, external descendant unbound PASS\n";
    }

    void rejection(object::ObjectMessageQueue& messages)
    {
        Counts counts;
        auto root = ui::Root::create(messages.dispatcherRef(), {.attachment_capacity = 2});
        assert(root);
        Window existing(messages.dispatcherRef(), "existing", counts);
        assert((*root)->addSubPane(existing));
        std::unique_ptr<Window, DeleteWindow> candidate(
            new Window(messages.dispatcherRef(), "existing", counts), DeleteWindow{counts}
        );
        auto* original = candidate.get();
        auto rejected = (*root)->addSubPane(std::move(candidate));
        assert(!rejected && rejected.error() == ui::EAttachmentError::DUPLICATE_ID);
        assert(candidate.get() == original && !candidate->parent() && counts.deleted == 0);
        std::unique_ptr<Window, DeleteWindow> second(
            new Window(messages.dispatcherRef(), "second", counts), DeleteWindow{counts}
        );
        Window reentrant(messages.dispatcherRef(), "reentrant", counts);
        bool moving = true;
        second.get_deleter().root = root->get();
        second.get_deleter().candidate = &reentrant;
        second.get_deleter().moving = &moving;
        assert((*root)->addSubPane(std::move(second)) && !second);
        assert(counts.callbacks > 0 && !reentrant.parent());
        moving = false;
        auto capacity = (*root)->addSubPane(reentrant);
        assert(!capacity && capacity.error() == ui::EAttachmentError::CAPACITY && !reentrant.parent());
        root->reset();
        assert(counts.deleted == 1 && !existing.parent());
        candidate.reset();
        assert(counts.deleted == 2);
        std::cout << "UI candidate/deleter preserved on refusal, reentrant move and capacity PASS\n";
    }

    void replacement(object::ObjectMessageQueue& messages)
    {
        Counts counts;
        auto root = ui::Root::create(messages.dispatcherRef());
        assert(root);
        Window pane(messages.dispatcherRef(), "replace", counts);
        assert((*root)->addSubPane(pane));
        auto old = std::make_unique<Item>(messages.dispatcherRef(), "old", counts);
        auto* old_ptr = old.get();
        assert(pane.setContent(std::move(old)) && !old);
        assert((*root)->capturePointer(*old_ptr) && (*root)->requestFocus(*old_ptr));
        (*root)->deferChange(*old_ptr, [](object::LuxObject&) noexcept { std::abort(); });
        auto next = std::make_unique<Item>(messages.dispatcherRef(), "next", counts);
        auto* next_ptr = next.get();
        auto occupied = pane.setContent(std::move(next));
        assert(!occupied && occupied.error() == ui::EAttachmentError::OCCUPIED);
        assert(next && pane.content() == old_ptr && old_ptr->attachedRoot());
        assert(pane.replaceContent(std::move(next)) && !next);
        assert(pane.content() == next_ptr && !old_ptr->attachedRoot() && counts.destroyed == 0);
        assert((*root)->removeSubPane(pane)); // Pending old content must not obstruct logical removal.
        (*root)->applyPendingChanges();
        assert(messages.collectRetired() == 1 && counts.destroyed == 1);
        assert(!next_ptr->attachedRoot());
        std::cout << "UI content occupied, atomic replacement, delayed reclaim and invalid input PASS\n";
    }

    void callbackLifetime(object::ObjectMessageQueue& messages)
    {
        Counts counts;
        auto root = ui::Root::create(messages.dispatcherRef());
        assert(root);
        auto pane = std::make_unique<Window>(messages.dispatcherRef(), "callback", counts);
        pane->messages = &messages;
        pane->on_update = [](Window& self) noexcept {
            assert(self.requestDestruction());
            assert(self.messages->collectRetired() == 0);
            self.on_update = nullptr;
        };
        assert((*root)->addSubPane(std::move(pane)));
        assert((*root)->update({}, nullptr) && counts.destroyed == 0);
        assert(messages.collectRetired() == 1 && counts.destroyed == 1);
        assert(!(*root)->findPane(ui::PaneIdView{"callback"}));
        std::cout << "UI update borrows defer self reclamation until callback returns PASS\n";
    }

    void batch(object::ObjectMessageQueue& messages)
    {
        Counts counts;
        auto root = ui::Root::create(messages.dispatcherRef());
        assert(root);
        Window old(messages.dispatcherRef(), "old", counts), a(messages.dispatcherRef(), "a", counts),
            conflict(messages.dispatcherRef(), "old", counts);
        assert((*root)->addSubPane(old));
        assert((*root)->requestFocus(old));
        const auto revision = (*root)->windowRevision();
        const std::array<ui::Pane*, 2> candidates{&a, &conflict};
        auto prepared = (*root)->prepareMount(candidates);
        assert(!prepared && prepared.error() == ui::EAttachmentError::DUPLICATE_ID);
        assert((*root)->windowRevision() == revision && !a.parent() && !conflict.parent());
        assert((*root)->panes().size() == 1 && (*root)->findPane(old.id().view()) == &old);
        std::optional<ui::EAttachmentError> wrong_thread;
        std::thread worker([&] { auto result = (*root)->addSubPane(a); assert(!result); wrong_thread = result.error(); });
        worker.join();
        assert(wrong_thread == ui::EAttachmentError::WRONG_THREAD);
        auto other_result = object::ObjectMessageQueue::create(32);
        assert(other_result);
        auto other = std::move(*other_result);
        Window foreign(other.dispatcherRef(), "foreign", counts);
        auto mismatch = (*root)->addSubPane(foreign);
        assert(!mismatch && mismatch.error() == ui::EAttachmentError::WRONG_DISPATCHER);
        std::cout << "UI failed batch leaves windows unchanged, precise thread/dispatcher failures PASS\n";
    }

    void frozenChanges(object::ObjectMessageQueue& messages)
    {
        Counts counts;
        auto root = ui::Root::create(messages.dispatcherRef(), {.docking = false});
        assert(root);
        Window pane(messages.dispatcherRef(), "frozen", counts);
        Item old(messages.dispatcherRef(), "old", counts), candidate(messages.dispatcherRef(), "new", counts);
        old.window = &pane;
        old.candidate = &candidate;
        const auto rejected = [](Item& self) noexcept {
            const auto result = self.window->replaceContent(*self.candidate);
            assert(!result && result.error() == ui::EAttachmentError::BUSY);
            assert(self.window->content() == &self && !self.candidate->parent());
        };
        old.on_measure = old.on_draw = old.on_event = rejected;
        assert(pane.setContent(old) && (*root)->addSubPane(pane));
        assert((*root)->requestFocus(old) && (*root)->capturePointer(old));
        ui::DrawData draw;
        assert((*root)->update({{640, 480}, 0.016F}, &draw));
        assert((*root)->update({{640, 480}, 0.016F}, &draw));
        ui::VInputEvent event = ui::PointerCancel{};
        static_cast<void>(object::sendEvent(old, event));
        assert(old.draws && old.measurements && old.events);
        assert(pane.content() == &old && !candidate.parent());
        assert(pane.replaceContent(candidate));
        assert(!old.parent() && !old.attachedRoot());
        std::cout << "UI draw/measurement/event rejects replacement without changing content PASS\n";
    }

    void retiredLayout(object::ObjectMessageQueue& messages)
    {
        Counts counts;
        auto root = ui::Root::create(messages.dispatcherRef(), {.docking = false});
        assert(root);
        Window pane(messages.dispatcherRef(), "layout-retirement", counts);
        ui::Layout layout(messages.dispatcherRef(), ui::ElementId{"fields"}, ui::ELayoutType::FORM);
        ui::Label label(messages.dispatcherRef(), ui::ElementId{"label"}, "Field");
        auto old = std::make_unique<Item>(messages.dispatcherRef(), "field", counts);
        auto* previous = old.get();
        assert(layout.addSubElement(label) && layout.addSubElement(std::move(old)));
        assert(pane.setContent(layout) && (*root)->addSubPane(pane));
        Item candidate(messages.dispatcherRef(), "field", counts);
        assert(layout.replaceSubElement(*previous, candidate));
        assert(layout.status() == ui::ELayoutStatus::VALID && counts.destroyed == 0);
        ui::DrawData draw;
        assert((*root)->update({{640, 480}, 0.016F}, &draw));
        assert(messages.collectRetired() == 1 && counts.destroyed == 1);
        Window nested(messages.dispatcherRef(), "nested", counts);
        assert(pane.addSubPane(nested));
        const std::array<ui::Pane*, 2> overlapping{&pane, &nested};
        auto invalid = (*root)->prepareDetach(overlapping);
        assert(!invalid && invalid.error() == ui::EAttachmentError::INVALID_TREE);
        assert(nested.parent() == &pane && nested.attachedRoot() == root->get());
        assert((*root)->removeSubPane(nested));
        assert(!nested.parent() && pane.attachedRoot() == root->get());
        std::cout << "UI retired content leaves layout before physical collection; nested removal PASS\n";
    }

    void ownedBatch(object::ObjectMessageQueue& messages)
    {
        Counts counts;
        auto root = ui::Root::create(messages.dispatcherRef(), {.attachment_capacity = 3});
        assert(root);
        Window existing(messages.dispatcherRef(), "existing", counts);
        assert((*root)->addSubPane(existing));
        std::vector<std::unique_ptr<ui::Pane, object::ObjectDeleter>> owners;
        const auto append = [&](const char* id)
        {
            auto value = std::make_unique<Window>(messages.dispatcherRef(), id, counts);
            auto deleter = object::ObjectDeleter::create<Window>(DeleteWindow{counts});
            owners.emplace_back(value.release(), std::move(deleter));
        };
        append("first");
        append("second");
        append("third");
        auto* first = owners[0].get();
        auto* second = owners[1].get();
        auto dock = (*root)->prepareDockTree({
            {{ui::EDockSplit::LEAF, UINT32_MAX, UINT32_MAX, 0.5F, {"existing"}}},
            {{0, {{20, 30}, {600, 400}}, false}}
        });
        assert(dock);
        (*root)->commitDockTree(std::move(*dock));
        const auto revision = (*root)->windowRevision();
        auto failed = (*root)->addSubPanes(owners);
        assert(!failed && failed.error() == ui::EAttachmentError::CAPACITY);
        assert(owners[0].get() == first && owners[1].get() == second && owners[2]);
        assert(!first->parent() && !second->parent() && (*root)->panes().size() == 1);
        const auto unchanged = (*root)->captureDockTree();
        assert((*root)->windowRevision() == revision);
        assert(unchanged.nodes.size() == 1 && unchanged.nodes[0].windows == std::vector<std::string>{"existing"});
        assert(unchanged.surfaces.size() == 1 && unchanged.surfaces[0].node == 0 && !unchanged.surfaces[0].floating);
        owners.pop_back();
        unsigned notifications{};
        auto connection = object::LuxObject::connect(
            root->get(), &ui::Root::attachmentChanged,
            [&](const ui::AttachmentChanged& change) noexcept
            {
                if (!change.mounted) return;
                assert(!owners[0] && !owners[1]);
                assert(first->parent() == root->get() && second->parent() == root->get());
                assert(first->ownership() == object::EObjectOwnership::PARENT_OWNED);
                assert(second->ownership() == object::EObjectOwnership::PARENT_OWNED);
                assert((*root)->findPane(first->id().view()) == first);
                assert((*root)->findPane(second->id().view()) == second);
                ++notifications;
            }
        );
        assert(connection && (*root)->addSubPanes(owners));
        assert(notifications == 2 && counts.deleted == 1);
        assert((*root)->removeSubPane(*first));
        assert(counts.deleted == 1 && messages.collectRetired() == 1 && counts.deleted == 2);
        root->reset();
        assert(counts.deleted == 3 && !existing.parent() && !existing.attachedRoot());
        std::cout << "UI owned batch rejects Nth capacity failure intact, commits before notification, retires once PASS\n";
    }

    void identityReuse(object::ObjectMessageQueue& messages)
    {
        class Sender final : public object::LuxObject
        {
        public:
            using LuxObject::LuxObject;
            object::TSignal<unsigned> changed{*this};
            auto send() noexcept { return emit(changed, 1U); }
        } sender(messages.dispatcherRef());
        Counts counts;
        auto root = ui::Root::create(messages.dispatcherRef());
        assert(root);
        alignas(Window) std::byte storage[sizeof(Window)];
        for (unsigned i{}; i < 16; ++i)
        {
            auto* old = ::new (storage) Window(messages.dispatcherRef(), "same", counts);
            assert((*root)->addSubPane(*old));
            const auto identity = (*root)->identify(*old);
            assert(identity && *(*root)->findPane(*identity) == old);
            auto connection = object::LuxObject::connect(
                &sender, &Sender::changed, old, &Window::receive, object::EDelivery::QUEUED
            );
            assert(connection && sender.send().queued == 1);
            (*root)->deferChange(*old, [](object::LuxObject&) noexcept { std::abort(); });
            assert((*root)->removeSubPane(*old));
            old->~Window();
            auto* current = ::new (storage) Window(messages.dispatcherRef(), "same", counts);
            assert((*root)->addSubPane(*current));
            assert(!(*root)->findPane(*identity));
            assert(*(*root)->identify(*current) != *identity);
            (*root)->applyPendingChanges();
            static_cast<void>(messages.dispatchPending());
            assert(counts.callbacks == 0 && (*root)->findPane(ui::PaneIdView{"same"}) == current);
            assert((*root)->removeSubPane(*current));
            current->~Window();
        }
        assert(counts.destroyed == 32);
        std::cout << "UI same address/PaneId reopens do not inherit queued requests or signal receivers PASS\n";
    }

    void synchronousBorrow(object::ObjectMessageQueue& messages)
    {
        Counts counts;
        auto root = ui::Root::create(messages.dispatcherRef());
        assert(root);
        auto owner = std::make_unique<Window>(messages.dispatcherRef(), "borrowed", counts);
        auto* window = owner.get();
        assert((*root)->addSubPane(std::move(owner)));
        const auto handle = *(*root)->identify(*window);
        bool called{};
        auto forbidden = [](ui::Pane&) { std::abort(); };
        std::thread wrong_thread([&]
        {
            auto result = (*root)->withPane(handle, forbidden);
            assert(!result && result.error() == ui::EAttachmentError::WRONG_THREAD);
        });
        wrong_thread.join();
        auto visit = [&](ui::Pane& pane)
        {
            called = true;
            assert(&pane == window && !window->dispatching());
            auto nested = (*root)->withPane(handle, forbidden);
            assert(!nested && nested.error() == ui::EAttachmentError::BUSY);
            auto removed = (*root)->removeSubPane(pane);
            assert(!removed && removed.error() == ui::EAttachmentError::BUSY);
            auto maintained = (*root)->update({}, nullptr);
            assert(!maintained && maintained.error() == ui::ECaptureError::FRAME_OPEN);
            // Rebinding can replace this owner's content without treating the call as an input signal.
            auto content = std::make_unique<ui::Layout>(messages.dispatcherRef(), ui::ElementId{"local"});
            assert(pane.setContent(std::move(content)));
            assert(pane.requestDestruction());
            assert(messages.collectRetired() == 0 && counts.destroyed == 0);
        };
        assert((*root)->withPane(handle, visit) && called);
        assert(messages.collectRetired() == 1 && counts.destroyed == 1);
        auto stale = (*root)->withPane(handle, forbidden);
        assert(!stale && stale.error() == ui::EAttachmentError::NOT_ATTACHED);
        std::cout << "UI synchronous borrow protects original owner and rejects nested maintenance/retirement PASS\n";
    }
}

int main()
{
    auto created = lux::object::ObjectMessageQueue::create(128);
    assert(created);
    auto messages = std::move(*created);
    externalRoot(messages);
    ownership(messages);
    rejection(messages);
    replacement(messages);
    callbackLifetime(messages);
    batch(messages);
    frozenChanges(messages);
    retiredLayout(messages);
    ownedBatch(messages);
    identityReuse(messages);
    synchronousBorrow(messages);
    static_cast<void>(messages.collectRetired());
    assert(messages.pendingRetirements() == 0);
}
