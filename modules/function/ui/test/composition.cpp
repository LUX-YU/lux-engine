#include "../../../../cmake/installed-consumers/common/UiTestContent.hpp"
#include "UiTestHelpers.hpp"
#include "lifetime_checks.hpp"
#include <array>
#include <cassert>
#include <iostream>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/object/ObjectRuntime.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>
#include <new>
#include <thread>

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
        Item(const char* id, Counts& counts) : Element{}, counts_(counts) {}
        ~Item() override
        {
            ++counts_.destroyed;
        }
        void (*on_draw)(Item&) noexcept {};
        void (*on_measure)(Item&) noexcept {};
        void (*on_event)(Item&) noexcept {};
        void (*on_finish)(Item&) noexcept {};
        ui::Pane* window{};
        ui::Element* candidate{};
        unsigned draws{}, measurements{}, events{}, finishes{};
        void finishEdit(bool = false) noexcept override
        {
            ++finishes;
            if (on_finish)
            {
                on_finish(*this);
            }
        }

    private:
        void draw() noexcept override
        {
            ++draws;
            if (on_draw)
            {
                on_draw(*this);
            }
        }
        ui::SizeHint measureContent(float) noexcept override
        {
            ++measurements;
            if (on_measure)
            {
                on_measure(*this);
            }
            return {{10, 10}, {40, 40}};
        }
        void event(object::EventView&) noexcept override
        {
            ++events;
            if (on_event)
            {
                on_event(*this);
            }
        }
        Counts& counts_;
    };
    class Window final : public ui::Pane
    {
    public:
        Window(const char* id, Counts& counts) : Pane(id), counts_(counts) {}
        ~Window() override
        {
            clearContent();
            ++counts_.destroyed;
        }
        void (*on_update)(Window&) noexcept {};
        object::ObjectRuntime* messages{};
        void receive(const unsigned&) noexcept
        {
            ++counts_.callbacks;
        }
        bool dispatching() const noexcept
        {
            return isDispatching();
        }

    private:
        void update() noexcept override
        {
            if (on_update)
            {
                on_update(*this);
            }
        }
        Counts& counts_;
    };
    static_assert(!std::derived_from<ui::Pane, ui::Element>);
    static_assert(!std::is_copy_constructible_v<ui::Root> && !std::is_move_constructible_v<ui::Root>);

    void closeIntent(object::ObjectRuntime& messages)
    {
        Counts counts;
        auto root = ui::Root::create();
        assert(root);
        auto& pane = ui_test::makePane<Window>(**root, "close-intent", counts);
        const auto initial = ui_test::paneHandle(pane);
        unsigned direct{};
        auto connection = object::LuxObject::connect(
            &pane,
            &ui::Pane::closeRequested,
            [&]() noexcept
            {
                assert(pane.hasCloseRequest() && pane.visible());
                ++direct;
            }
        );
        assert(connection);
        pane.requestClose();
        pane.requestClose();
        assert(direct == 2 && pane.hasCloseRequest());
        assert((*root)->update() && pane.hasCloseRequest() && pane.visible());
        pane.dismissCloseRequest();
        assert(!pane.hasCloseRequest() && pane.visible());
        pane.requestClose();
        auto owner = (*root)->removePane(pane);
        assert(owner && !pane.hasCloseRequest() && !ui_test::resolvePane(**root, initial));
        assert((*root)->addPane(std::move(*owner)) && ui_test::paneHandle(pane) != initial);
        pane.requestClose();
        owner = (*root)->removePane(pane);
        assert(owner);
        root->reset();
        assert(!pane.hasCloseRequest() && !pane.attachedRoot() && !pane.parent() && counts.destroyed == 0);
        auto receiver = std::make_unique<object::LuxObject>();
        unsigned queued{};
        auto hint = object::LuxObject::connect(
            &pane,
            &ui::Pane::closeRequested,
            receiver.get(),
            [&]() noexcept { ++queued; },
            object::EDelivery::QUEUED
        );
        assert(hint);
        const auto capacity = messages.statistics().capacity_per_batch;
        for (std::size_t i = 1; i < capacity; ++i)
        {
            assert(
                object::detail::post(object::detail::makeMessage([]() noexcept {})) ==
                object::detail::EPostStatus::POSTED
            );
        }
        pane.requestClose();
        pane.dismissCloseRequest();
        pane.requestClose();
        assert(pane.hasCloseRequest() && messages.statistics().pending == capacity);
        assert(messages.dispatchPending() == capacity && queued == 1 && pane.hasCloseRequest());
        receiver.reset();
        assert(!hint->connected());
        pane.dismissCloseRequest();
        pane.requestClose();
        assert(pane.hasCloseRequest() && queued == 1);
        std::cout << "Pane close intent, FULL/CLOSED hints, removal and fresh registration PASS\n";
    }

    void externalRoot(object::ObjectRuntime&)
    {
        Counts counts;
        auto root = ui::Root::create();
        assert(root);
        auto& pane = ui_test::makePane<Window>(**root, "external", counts);
        ui::Layout layout{};
        ui::Button button("Button");
        assert(layout.addElement(button) && pane.addElement(layout));
        const auto root_id = (*root)->objectId();
        const auto identity = ui_test::paneHandle(pane);
        assert((*root)->requestFocus(button) && (*root)->capturePointer(button));
        (*root)->deferChange(button, [](object::LuxObject&) noexcept { std::abort(); });
        auto owner = (*root)->removePane(pane);
        assert(owner && !(*root)->focusedElement() && !pane.parent() && !button.attachedRoot());
        root->reset();
        assert(button.parent() == &layout && layout.parent() == &pane && pane.content() == &layout);
        auto replacement = ui::Root::create();
        assert(replacement && (*replacement)->addPane(std::move(*owner)));
        auto forbidden = [](ui::Pane&) noexcept { std::abort(); };
        assert(!object::ObjectRuntime::instance().resolve(root_id));
        const auto remounted = ui_test::paneHandle(pane);
        assert((*replacement)->update());
        owner = (*replacement)->removePane(pane);
        assert(owner && !ui_test::resolvePane(**replacement, remounted));
        assert((*replacement)->addPane(std::move(*owner)) && ui_test::paneHandle(pane) != remounted);
        const auto current_root = (*replacement)->objectId();
        const auto current_id = ui_test::paneHandle(pane);
        std::thread other(
            [&]
            {
                auto result = (*replacement)->forEachPane(forbidden);
                assert(!result && result.error() == ui::EPaneError::WRONG_THREAD);
            }
        );
        other.join();
        owner = (*replacement)->removePane(pane);
        assert(owner && !button.attachedRoot());
        std::cout << "Transferred owner survives Root, stale Root/ObjectId rejected, routes revoked PASS\n";
    }

    void ownership(object::ObjectRuntime&)
    {
        Counts counts;
        auto root = ui::Root::create();
        assert(root);
        Item external("external-item", counts);
        class Composite final : public ui::Pane
        {
        public:
            Composite(Counts& counts, Item& external) : Pane("owned"), counts_(counts)
            {
                assert(layout_.addElement(external) && addElement(layout_));
            }
            ~Composite() override
            {
                ++counts_.destroyed;
            }

        private:
            Counts& counts_;
            ui::Layout layout_{};
        };
        auto& pane = ui_test::makePane<Composite>(**root, counts, external);
        assert(external.parent() == pane.content() && external.attachedRoot() == root->get());
        root->reset();
        assert(counts.destroyed == 1 && !external.parent() && !external.containingPane() && !external.attachedRoot());
        std::cout << "Root owns windows; member layout unbinds borrowed descendants exactly once PASS\n";
    }

    void rejection(object::ObjectRuntime&)
    {
        Counts counts;
        auto root = ui::Root::create({.pane_capacity = 2});
        assert(root);
        auto& first = ui_test::makePane<Window>(**root, "same", counts);
        auto& second = ui_test::makePane<Window>(**root, "same", counts);
        assert(ui_test::paneHandle(first) != ui_test::paneHandle(second) && ui_test::paneCount(**root) == 2);
        std::unique_ptr<ui::Pane> candidate = std::make_unique<Window>("capacity", counts);
        auto* original = candidate.get();
        auto refused = (*root)->addPane(std::move(candidate));
        assert(!refused && refused.error() == ui::EPaneError::CAPACITY);
        assert(candidate.get() == original && !candidate->parent() && !!ui_test::paneHandle(*candidate).isNull());
        object::LuxObject plain;
        assert(!plain.addChild(first) && !static_cast<object::LuxObject&>(first).addChild(plain));
        assert(
            !static_cast<object::LuxObject&>(second).setParent(&first) && first.parent() == root->get() &&
            second.parent() == root->get()
        );
        ui::Button leaf("Leaf");
        Item child("child", counts);
        auto invalid = leaf.addElement(child);
        assert(!invalid && invalid.error() == ui::EPaneError::INVALID_TREE && !child.parent());
        root->reset();
        assert(counts.destroyed == 2 && candidate.get() == original);
        candidate.reset();
        assert(counts.destroyed == 3);
        std::cout << "Capacity preserves candidate; same titles coexist; generic topology bypass refused PASS\n";
    }

    void replacement(object::ObjectRuntime&)
    {
        Counts counts;
        auto root = ui::Root::create();
        assert(root);
        auto& pane = ui_test::makePane<Window>(**root, "replace", counts);
        auto old = std::make_unique<Item>("old", counts);
        assert(pane.addElement(*old));
        assert((*root)->capturePointer(*old) && (*root)->requestFocus(*old));
        (*root)->deferChange(*old, [](object::LuxObject&) noexcept { std::abort(); });
        auto next = std::make_unique<Item>("next", counts);
        auto occupied = pane.addElement(*next);
        assert(!occupied && occupied.error() == ui::EPaneError::OCCUPIED);
        assert(next && pane.content() == old.get() && old->attachedRoot());
        unsigned notices{};
        auto notice = object::LuxObject::connect(
            root->get(),
            &ui::Root::objectRemoved,
            [&](const ui::ObjectRemoved& notice) noexcept
            {
                auto* value = notice.object;
                if (value != old.get())
                {
                    return;
                }
                assert(pane.content() == next.get() && !old->parent() && !(*root)->focusedElement());
                assert(!pane.replaceContent(*old));
                ++notices;
            }
        );
        assert(notice && pane.replaceContent(*next) && notices == 1);
        assert(!old->attachedRoot() && counts.destroyed == 0);
        old.reset();
        assert(counts.destroyed == 1);
        auto owner = (*root)->removePane(pane);
        assert(owner && !next->attachedRoot());
        ui_test::apply(**root);
        std::cout << "Single content root; replacement revokes input before complete-fact notification PASS\n";
    }

    void callbackLifetime(object::ObjectRuntime& messages)
    {
        Counts counts;
        auto root = ui::Root::create();
        assert(root);
        auto& pane = ui_test::makePane<Window>(**root, "callback", counts);
        pane.messages = &messages;
        pane.on_update = [](Window& self) noexcept
        {
            auto removal = self.root().removePane(self);
            assert(!removal && removal.error() == ui::EPaneError::BUSY);
            assert(self.messages->collectRetired() == 0);
            self.on_update = nullptr;
            self.root().deferChange(
                self,
                [](object::LuxObject& target) noexcept { static_cast<Window&>(target).setVisible(false); }
            );
        };
        assert((*root)->update() && counts.destroyed == 0 && pane.visible());
        ui_test::apply(**root);
        assert(!pane.visible());
        auto owner = (*root)->removePane(pane);
        assert(owner && counts.destroyed == 0);
        owner->reset();
        assert(counts.destroyed == 1 && (ui_test::paneCount(**root) == 0));
        std::cout << "Update cannot destroy owner; safe-point intent and explicit C++ lifetime PASS\n";
    }

    void batch(object::ObjectRuntime&)
    {
        Counts counts;
        auto root = ui::Root::create();
        assert(root);
        auto& old = ui_test::makePane<Window>(**root, "old", counts);
        assert((*root)->requestFocus(old));
        std::array<std::unique_ptr<ui::Pane>, 2> candidates;
        candidates[0] = std::make_unique<Window>("a", counts);
        auto* original = candidates[0].get();
        auto failed = (*root)->addPanes(candidates);
        assert(!failed && failed.error() == ui::EPaneError::INVALID_TREE);
        assert(candidates[0].get() == original && !!ui_test::paneHandle(*original).isNull() && !original->parent());
        assert(ui_test::paneCount(**root) == 1);
        assert(ui_test::resolvePane(**root, ui_test::paneHandle(old)) == &old);
        std::thread worker(
            [&]
            {
                auto result = (*root)->addPane(std::move(candidates[0]));
                assert(!result && result.error() == ui::EPaneError::WRONG_THREAD);
            }
        );
        worker.join();
        assert(candidates[0].get() == original && (*root)->addPane(std::move(candidates[0])));
        std::cout << "Nth invalid candidate and wrong thread preserve original owners/UI PASS\n";
    }

    void frozenChanges(object::ObjectRuntime& messages)
    {
        Counts counts;
        auto root = ui::Root::create({.docking = false});
        assert(root);
        auto& pane = ui_test::makePane<Window>(**root, "frozen", counts);
        Item old("old", counts), candidate("new", counts);
        old.window = &pane;
        old.candidate = &candidate;
        const auto rejected = [](Item& self) noexcept
        {
            const auto result = self.window->replaceContent(*self.candidate);
            assert(!result && result.error() == ui::EPaneError::BUSY);
            assert(self.window->content() == &self && !self.candidate->parent());
        };
        old.on_measure = old.on_draw = old.on_event = rejected;
        assert(pane.addElement(old));
        assert((*root)->requestFocus(old) && (*root)->capturePointer(old));
        ui::DrawData draw;
        assert((*root)->update({{640, 480}, 0.016F}, draw));
        assert((*root)->update({{640, 480}, 0.016F}, draw));
        ui::VInputEvent event = ui::PointerCancel{};
        static_cast<void>(object::sendEvent(old, event));
        assert(old.draws && old.measurements && old.events);
        assert(pane.content() == &old && !candidate.parent());
        assert(pane.replaceContent(candidate));
        assert(!old.parent() && !old.attachedRoot());
        std::cout << "UI draw/measurement/event rejects replacement without changing content PASS\n";
    }

    void retiredLayout(object::ObjectRuntime&)
    {
        Counts counts;
        auto root = ui::Root::create({.docking = false});
        assert(root);
        auto& pane = ui_test::makePane<Window>(**root, "layout-retirement", counts);
        ui::Layout layout(ui::ELayoutType::FORM);
        ui::Label label("Field");
        auto old = std::make_unique<Item>("field", counts);
        assert(layout.addElement(label) && layout.addElement(*old) && pane.addElement(layout));
        Item candidate("field", counts);
        assert(layout.replaceElement(*old, candidate));
        assert(layout.status() == ui::ELayoutStatus::VALID && counts.destroyed == 0 && !old->parent());
        ui::DrawData draw;
        assert((*root)->update({{640, 480}, 0.016F}, draw));
        old.reset();
        assert(counts.destroyed == 1);
        auto& independent = ui_test::makePane<Window>(**root, "independent", counts);
        pane.setVisible(false);
        assert(independent.visible() && independent.parent() == root->get());
        assert((*root)->removePane(independent) && pane.attachedRoot() == root->get());
        std::cout << "Detached fields leave layout before destruction; windows are independent Root children PASS\n";
    }

    void ownedBatch(object::ObjectRuntime&)
    {
        Counts counts;
        auto root = ui::Root::create({.pane_capacity = 3});
        assert(root);
        auto& existing = ui_test::makePane<Window>(**root, "existing", counts);
        std::vector<std::unique_ptr<ui::Pane>> owners;
        for (auto name : {"first", "second", "third"})
        {
            owners.push_back(std::make_unique<Window>(name, counts));
        }
        auto* first = owners[0].get();
        auto* second = owners[1].get();
        auto dock = (*root)->setDockTree(
            {{{ui::EDockSplit::LEAF, UINT32_MAX, UINT32_MAX, 0.5F, {ui_test::paneHandle(existing)}}},
             {{0, {{20, 30}, {600, 400}}, false}}}
        );
        assert(dock);
        auto failed = (*root)->addPanes(owners);
        assert(!failed && failed.error() == ui::EPaneError::CAPACITY);
        assert(owners[0].get() == first && owners[1].get() == second && owners[2]);
        assert(!first->parent() && !second->parent() && ui_test::paneCount(**root) == 1);
        const auto unchanged = (*root)->captureDockTree();
        assert(
            unchanged.nodes.size() == 1 &&
            unchanged.nodes[0].panes == std::vector<ui::PaneHandle>{ui_test::paneHandle(existing)}
        );
        assert(unchanged.surfaces.size() == 1 && unchanged.surfaces[0].node == 0 && !unchanged.surfaces[0].floating);
        owners.pop_back();
        unsigned notifications{};
        auto connection = object::LuxObject::connect(
            root->get(),
            &ui::Root::paneChanged,
            [&](const ui::PaneChanged& change) noexcept
            {
                if (!change.attached)
                {
                    return;
                }
                assert(!owners[0] && !owners[1] && first->parent() == root->get() && second->parent() == root->get());
                assert(
                    ui_test::resolvePane(**root, ui_test::paneHandle(*first)) == first &&
                    ui_test::resolvePane(**root, ui_test::paneHandle(*second)) == second
                );
                auto result = (*root)->clearPanes();
                assert(!result && result.error() == ui::EPaneError::BUSY);
                ++notifications;
            }
        );
        assert(connection && (*root)->addPanes(owners) && notifications == 2 && counts.destroyed == 1);
        const auto first_id = ui_test::paneHandle(*first);
        auto removed = (*root)->removePane(*first);
        assert(removed && counts.destroyed == 1 && !ui_test::resolvePane(**root, first_id));
        removed->reset();
        assert(counts.destroyed == 2);
        root->reset();
        assert(counts.destroyed == 4);
        std::cout << "Owned batch: capacity refusal preserves docking; complete commit before notifications PASS\n";
    }

    void identityReuse(object::ObjectRuntime& messages)
    {
        class Sender final : public object::LuxObject
        {
        public:
            object::TSignal<unsigned> changed{*this};
            auto send() noexcept
            {
                return emit(changed, 1U);
            }
        } sender;
        Counts counts;
        auto root = ui::Root::create();
        assert(root);
        for (unsigned i{}; i < 16; ++i)
        {
            auto& old = ui_test::makePane<Window>(**root, "same", counts);
            const auto identity = ui_test::paneHandle(old);
            auto connection = object::LuxObject::connect(
                &sender,
                &Sender::changed,
                &old,
                &Window::receive,
                object::EDelivery::QUEUED
            );
            assert(connection && sender.send().queued == 1);
            (*root)->deferChange(old, [](object::LuxObject&) noexcept { std::abort(); });
            auto owner = (*root)->removePane(old);
            assert(owner && !!ui_test::paneHandle(old).isNull());
            // The exact same object/address gets a fresh Root registration.
            assert((*root)->addPane(std::move(*owner)) && ui_test::paneHandle(old) != identity);
            assert(!ui_test::resolvePane(**root, identity));
            ui_test::apply(**root);
            // Connection remains attached to ObjectId, independent from the old PaneId.
            assert(messages.dispatchPending() == 1 && counts.callbacks == i + 1);
            assert(sender.send().queued == 1);
            assert((*root)->clearPanes());
            auto& current = ui_test::makePane<Window>(**root, "same", counts);
            assert(
                !ui_test::resolvePane(**root, identity) &&
                ui_test::resolvePane(**root, ui_test::paneHandle(current)) == &current
            );
            assert(messages.dispatchPending() == 1 && counts.callbacks == i + 1);
            assert((*root)->clearPanes());
        }
        assert(counts.destroyed == 32);
        std::cout << "Object and Pane generations independently reject stale routing and reused registrations PASS\n";
    }

    void reparenting()
    {
        Counts counts;
        auto first = ui::Root::create({.docking = false});
        auto second = ui::Root::create({.docking = false});
        assert(first && second);
        auto& source = ui_test::makePane<Window>(**first, "Source", counts);
        auto& destination = ui_test::makePane<Window>(**second, "Destination", counts);
        ui::Layout a, b, nested;
        Item child("identical label", counts), other("identical label", counts), candidate("new", counts);
        assert(child.objectId() != other.objectId());
        assert(source.addElement(a) && destination.addElement(b));
        assert(a.addElement(nested) && nested.addElement(child) && a.addElement(other));
        assert(child.finishes == 0 && other.finishes == 0);
        assert((*first)->requestFocus(child) && (*first)->capturePointer(child));
        (*first)->deferChange(child, [](object::LuxObject&) noexcept { std::abort(); });
        child.window = &destination;
        child.candidate = &candidate;
        child.on_finish = [](Item& self) noexcept
        {
            const auto blocked = self.window->content()->addElement(*self.candidate);
            assert(!blocked && blocked.error() == ui::EPaneError::BUSY && !self.candidate->parent());
        };
        auto connection = object::LuxObject::connect(
            first->get(),
            &ui::Root::objectRemoved,
            [&](const ui::ObjectRemoved& notice) noexcept
            {
                auto* removed = notice.object;
                if (removed == &child)
                {
                    assert(child.parent() == &b && child.containingPane() == &destination);
                    assert(!(*first)->focusedElement() && !nested.firstChild());
                }
            }
        );
        assert(connection && b.addElement(child));
        assert(child.finishes == 1 && child.attachedRoot() == second->get());
        assert(!(*first)->focusedElement() && (*second)->requestFocus(child));
        ui_test::apply(**first); // The old-root intent was revoked, not rebound to the same object.
        assert(b.addElement(child) && child.finishes == 1);
        auto occupied = source.addElement(child);
        assert(!occupied && occupied.error() == ui::EPaneError::OCCUPIED && child.parent() == &b);
        auto cycle = nested.addElement(a);
        assert(!cycle && cycle.error() == ui::EPaneError::INVALID_TREE && nested.parent() == &a);
        child.on_finish = nullptr;
        ui::Layout detached;
        assert(detached.addElement(child) && !child.containingPane() && !(*second)->focusedElement());
        assert(child.finishes == 2 && child.parent() == &detached);
        assert(b.addElement(child) && child.finishes == 3);

        // Promoting an existing descendant replaces its old composite without ending the gesture twice.
        assert(source.replaceContent(other));
        assert(source.content() == &other && !a.parent() && other.containingPane() == &source);
        assert(other.finishes == 1 && nested.containingPane() == nullptr);
        std::cout << "UI reparent preserves ownership, ends interaction once and revokes both routing domains PASS\n";
    }

    void synchronousBorrow(object::ObjectRuntime&)
    {
        Counts counts;
        auto root = ui::Root::create();
        assert(root);
        auto& window = ui_test::makePane<Window>(**root, "borrowed", counts);
        const auto root_id = (*root)->objectId();
        const auto id = ui_test::paneHandle(window);
        bool called{};
        auto forbidden = [](ui::Pane&) noexcept { std::abort(); };
        auto visit = [&](ui::Pane& pane) noexcept
        {
            called = true;
            assert(&pane == &window && !window.dispatching());
            auto nested = (*root)->forEachPane(forbidden);
            assert(!nested && nested.error() == ui::EPaneError::BUSY);
            auto removed = (*root)->removePane(pane);
            assert(!removed && removed.error() == ui::EPaneError::BUSY);
            auto maintained = (*root)->update();
            assert(!maintained && maintained.error() == ui::ECaptureError::FRAME_OPEN);
            ui::Layout content{};
            auto refused = pane.addElement(content);
            assert(!refused && refused.error() == ui::EPaneError::BUSY && !content.parent());
        };
        assert((*root)->forEachPane(visit) && called);
        assert((*root)->clearPanes() && counts.destroyed == 1);
        auto stale = (*root)->forEachPane(forbidden);
        assert(stale && !ui_test::resolvePane(**root, id));
        std::cout << "Synchronous borrow freezes structure, nested maintenance and stale targets PASS\n";
    }
} // namespace

int main()
{
    auto& created = lux::object::ObjectRuntime::instance();
    auto& messages = created;
    lifetime_checks::run();
    reparenting();
    closeIntent(messages);
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
