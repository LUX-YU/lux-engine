#include <array>
#include <cassert>
#include <iostream>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/object/ObjectRuntime.hpp>
#include <lux/engine/ui/Command.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>
#include <vector>

namespace
{
    using namespace lux;

    struct Counts final
    {
        unsigned destroyed{}, updated{}, queried{}, executed{};
    };

    void command(object::EventView& event, Counts& counts, bool accepts, bool enabled) noexcept
    {
        auto* request = event.getIf<ui::Command>();
        if (!request || !accepts)
        {
            return;
        }
        event.accept();
        request->enabled = enabled;
        if (request->phase == ui::ECommandPhase::QUERY)
        {
            ++counts.queried;
        }
        else
        {
            ++counts.executed;
        }
    }

    class Pane final : public ui::Pane
    {
    public:
        explicit Pane(Counts& counts) : ui::Pane("Same title"), counts_(counts) {}

        ~Pane() override
        {
            ++counts_.destroyed;
        }

        bool accepts{}, enabled{true};

    private:
        void update() noexcept override
        {
            ++counts_.updated;
        }

        void event(object::EventView& event) noexcept override
        {
            command(event, counts_, accepts, enabled);
        }

        Counts& counts_;
    };

    class Fallback final : public object::LuxObject
    {
    public:
        explicit Fallback(Counts& counts) : counts_(counts) {}

    private:
        void event(object::EventView& event) noexcept override
        {
            command(event, counts_, true, true);
        }

        Counts& counts_;
    };

    class OrderedPane final : public ui::Pane
    {
    public:
        explicit OrderedPane(std::vector<unsigned>& order) : ui::Pane("Ordered work"), order_(order) {}

        OrderedPane* preceding_owner{};
        ui::PaneHandle erase_target;

        static void mutation(object::LuxObject& object) noexcept
        {
            auto& self = static_cast<OrderedPane&>(object);
            self.order_.push_back(2);
        }

    private:
        static void removeTarget(object::LuxObject& object) noexcept
        {
            auto& self = static_cast<OrderedPane&>(object);
            auto* victim = self.root().resolvePane(self.erase_target);
            assert(victim);
            auto removed = self.root().removePane(*victim);
            assert(removed);
            removed->reset();
            self.order_.push_back(4);
            self.root().deferChange(self, following);
        }

        static void following(object::LuxObject& object) noexcept
        {
            auto& self = static_cast<OrderedPane&>(object);
            self.order_.push_back(3);
        }

        void event(object::EventView& event) noexcept override
        {
            const auto* input = event.getIf<ui::VInputEvent>();
            const auto* key = input ? std::get_if<ui::Key>(input) : nullptr;
            const bool is_removal_key = key && key->down && key->key == ui::EKey::K;
            if (preceding_owner && is_removal_key)
            {
                root().deferChange(*preceding_owner, removeTarget);
            }
            auto* command = event.getIf<ui::Command>();
            if (!command)
            {
                return;
            }
            event.accept();
            command->enabled = true;
            if (command->phase == ui::ECommandPhase::EXECUTE)
            {
                order_.push_back(1);
                root().deferChange(*this, following);
                auto nested = root().update();
                assert(!nested && nested.error() == ui::ECaptureError::FRAME_OPEN);
            }
        }

        std::vector<unsigned>& order_;
    };

    void mixedSafePointOrder()
    {
        auto made = ui::Root::create({.docking = false});
        assert(made);
        auto& root = **made;
        std::vector<unsigned> order;
        auto owner = std::make_unique<OrderedPane>(order);
        auto& pane = *owner;
        assert(root.addPane(std::move(owner)));
        assert(root.setMenu({{{ui::CommandId{"test.ordered"}, "Ordered", "Ctrl+K", {ui::EKey::K, true}}}, {}}));
        ui::DrawData draw;
        for (unsigned index{}; index != 3; ++index)
        {
            assert(root.update({{640, 480}, 0.016f}, draw));
        }
        assert(root.requestFocus(pane));
        assert(root.feedInput(ui::Key{ui::EKey::LEFT_CONTROL, true}));
        assert(root.feedInput(ui::Key{ui::EKey::K, true}));
        assert(root.update({{640, 480}, 0.016f}, draw));
        assert(order.empty());
        root.deferChange(pane, OrderedPane::mutation);
        root.deferChange(pane, OrderedPane::mutation); // Only mutation intents coalesce.
        assert(root.update());
        for (auto value : order)
        {
            std::cout << value << ' ';
        }
        std::cout << std::endl;
        assert((order == std::vector<unsigned>{1, 2}));
        assert(root.update());
        assert((order == std::vector<unsigned>{1, 2, 3}));
        assert(root.update());
        assert(order.size() == 3);
        assert(root.clearPanes());
    }

    void mixedTargetRemoval()
    {
        auto made = ui::Root::create({.docking = false});
        assert(made);
        auto& root = **made;
        std::vector<unsigned> order;
        auto first = std::make_unique<OrderedPane>(order);
        auto victim = std::make_unique<OrderedPane>(order);
        auto& survivor = *first;
        auto& removed_pane = *victim;
        removed_pane.preceding_owner = &survivor;
        assert(root.addPane(std::move(first)));
        assert(root.addPane(std::move(victim)));
        survivor.erase_target = root.paneHandle(removed_pane);
        assert(root.setMenu({{{ui::CommandId{"test.removed"}, "Removed", "Ctrl+K", {ui::EKey::K, true}}}, {}}));
        Counts application;
        Fallback fallback(application);
        root.setCommandFallback(&fallback);
        ui::DrawData draw;
        for (unsigned index{}; index != 3; ++index)
        {
            assert(root.update({{640, 480}, 0.016f}, draw));
        }
        assert(root.requestFocus(removed_pane));
        assert(root.feedInput(ui::Key{ui::EKey::LEFT_CONTROL, true}));
        assert(root.feedInput(ui::Key{ui::EKey::K, true}));
        assert(root.update({{640, 480}, 0.016f}, draw));
        root.deferChange(removed_pane, OrderedPane::mutation);
        assert(root.update());
        assert((order == std::vector<unsigned>{4}));
        assert(!root.resolvePane(survivor.erase_target));
        assert(application.queried == 0 && application.executed == 0);
        assert(root.update());
        assert((order == std::vector<unsigned>{4, 3}));
        assert(root.clearPanes());
    }

    class ActionReceiver final : public object::LuxObject
    {
    public:
        explicit ActionReceiver(ui::Root& root) noexcept : root_(root) {}

        unsigned queried{}, executed{};
        bool enabled{true};

    private:
        void event(object::EventView& event) noexcept override
        {
            auto* request = event.getIf<ui::Command>();
            if (!request)
            {
                return;
            }
            assert(request->id == ui::CommandIdView{"test.shared.action"});
            const auto refused = root_.setMenu({});
            assert(!refused && refused.error() == ui::EMenuError::BUSY);
            event.accept();
            if (request->phase == ui::ECommandPhase::QUERY)
            {
                ++queried;
                request->enabled = enabled;
                request->checked = true;
            }
            else
            {
                ++executed;
            }
        }

        ui::Root& root_;
    };

    void actionProjection()
    {
        auto made = ui::Root::create({.docking = false});
        assert(made);
        auto& root = **made;
        ActionReceiver receiver(root);
        root.setCommandFallback(&receiver);
        const ui::CommandId action{"test.shared.action"};
        {
            ui::MenuDefinition definition{
                {{action, "Shared", "Ctrl+K", {ui::EKey::K, true}, true}},
                {{ui::MenuId{"test.one"}, "One", {ui::MenuAction{action}}},
                 {ui::MenuId{"test.two"}, "Two", {ui::MenuAction{action}}}}
            };
            assert(root.setMenu(std::move(definition)));
        }
        assert(root.menu().actions.size() == 1 && root.menu().menus.size() == 2);
        assert(root.menu().actions[0].label == "Shared");
        const auto* installed = &root.menu();
        auto rejected = [&](ui::MenuDefinition candidate, ui::EMenuError expected)
        {
            const auto result = root.setMenu(std::move(candidate));
            assert(!result && result.error() == expected);
            assert(&root.menu() == installed);
        };
        auto invalid = root.menu();
        invalid.actions.push_back(invalid.actions[0]);
        rejected(std::move(invalid), ui::EMenuError::DUPLICATE_ACTION);
        invalid = root.menu();
        invalid.actions[0].id = {};
        rejected(std::move(invalid), ui::EMenuError::INVALID_ACTION);
        invalid = root.menu();
        invalid.actions.push_back({ui::CommandId{"test.other"}, "Other", "Ctrl+K", {ui::EKey::K, true}});
        rejected(std::move(invalid), ui::EMenuError::DUPLICATE_SHORTCUT);
        invalid = root.menu();
        invalid.menus[1].id = invalid.menus[0].id;
        rejected(std::move(invalid), ui::EMenuError::DUPLICATE_MENU);
        invalid = root.menu();
        invalid.menus[1].children.push_back(ui::MenuAction{ui::CommandId{"missing"}});
        rejected(std::move(invalid), ui::EMenuError::UNKNOWN_ACTION);
        invalid = root.menu();
        invalid.menus[1].id = {};
        rejected(std::move(invalid), ui::EMenuError::INVALID_MENU);

        ui::DrawData draw;
        const auto frame = [&] { assert(root.update({{640, 480}, 0.016f}, draw)); };
        for (unsigned index{}; index != 3; ++index)
        {
            frame();
        }
        const auto point = [&](float x, float y)
        {
            assert(root.feedInput(ui::PointerMove{{x, y}}));
            frame();
        };
        const auto button = [&](bool down)
        {
            assert(root.feedInput(ui::PointerButton{ui::EPointerButton::LEFT, down}));
            frame();
        };
        for (float x : {15.f, 60.f})
        {
            const auto queries = receiver.queried;
            const auto executions = receiver.executed;
            point(x, 10);
            button(true);
            button(false);
            assert(receiver.queried > queries && receiver.executed == executions);
            point(x, 38);
            button(true);
            button(false);
            assert(receiver.executed == executions); // EXECUTE remains outside the drawing/input stack.
            assert(root.update());
            assert(receiver.executed == executions + 1);
        }
        // Surface placement is independent of the one action and shortcut registration.
        auto customized = root.menu();
        customized.menus.clear();
        assert(root.setMenu(std::move(customized)));
        assert(root.feedInput(ui::Key{ui::EKey::LEFT_CONTROL, true}));
        assert(root.feedInput(ui::Key{ui::EKey::K, true}));
        frame();
        assert(receiver.executed == 2);
        assert(root.setMenu({})); // Already accepted command keeps its owning semantic identity.
        assert(root.update());
        assert(receiver.executed == 3);
        assert(root.feedInput(ui::Key{ui::EKey::K, false}));
        frame();
        assert(root.setMenu({{{action, "Customized label", "Ctrl+J", {ui::EKey::J, true}}}, {}}));
        assert(root.feedInput(ui::Key{ui::EKey::K, true}));
        frame();
        assert(root.update());
        assert(receiver.executed == 3);
        assert(root.feedInput(ui::Key{ui::EKey::K, false}));
        frame();
        assert(root.feedInput(ui::Key{ui::EKey::J, true}));
        frame();
        assert(receiver.executed == 3);
        assert(root.update());
        assert(receiver.executed == 4);
        assert(root.feedInput(ui::Key{ui::EKey::J, false}));
        frame();
        receiver.enabled = false;
        assert(root.feedInput(ui::Key{ui::EKey::J, true}));
        frame();
        assert(root.update());
        assert(receiver.executed == 4); // Dynamic QUERY policy remains at the receiver.
    }

    void replacement()
    {
        auto made = ui::Root::create({.docking = false, .pane_capacity = 2});
        assert(made);
        auto& root = **made;
        Counts global, old, next;
        auto global_owner = root.addPane(std::make_unique<Pane>(global));
        auto old_owner = root.addPane(std::make_unique<Pane>(old));
        assert(global_owner && old_owner);
        const auto global_handle = root.paneHandle(global_owner->get());
        const auto old_handle = root.paneHandle(old_owner->get());
        std::array remove{old_handle};
        std::array<std::unique_ptr<ui::Pane>, 2> invalid{std::make_unique<Pane>(next), nullptr};
        unsigned commits{};
        ui::PaneHandle next_handle;
        auto commit = [&](std::span<const ui::PaneHandle> added) noexcept
        {
            ++commits;
            assert(!root.resolvePane(old_handle) && root.resolvePane(global_handle));
            assert(added.size() == 1 && root.resolvePane(added.front()));
            next_handle = added.front();
            auto blocked = root.clearPanes();
            assert(!blocked && blocked.error() == ui::EPaneError::BUSY);
        };
        auto rejected = root.replacePanes(remove, invalid, commit);
        assert(!rejected && rejected.error() == ui::EPaneError::INVALID_TREE);
        assert(commits == 0 && invalid[0] && root.resolvePane(old_handle) && old.destroyed == 0);
        invalid[1] = std::make_unique<Pane>(next);
        rejected = root.replacePanes(remove, invalid, commit);
        assert(!rejected && rejected.error() == ui::EPaneError::CAPACITY);
        assert(commits == 0 && invalid[0] && invalid[1] && root.resolvePane(old_handle));
        const std::array duplicate{old_handle, old_handle};
        rejected = root.replacePanes(duplicate, {}, commit);
        assert(!rejected && rejected.error() == ui::EPaneError::DUPLICATE_PANE && commits == 0);
        invalid[1].reset();
        unsigned notifications{}, removals{};
        auto changed = object::LuxObject::connect(
            &root,
            &ui::Root::paneChanged,
            [&](const ui::PaneChanged& change) noexcept
            {
                assert(commits == 1 && root.resolvePane(next_handle) && root.resolvePane(global_handle));
                assert(!root.resolvePane(old_handle) && old.destroyed == 0);
                assert(change.attached == (notifications == 1));
                ++notifications;
                assert(!root.clearPanes());
            }
        );
        auto removed = object::LuxObject::connect(
            &root,
            &ui::Root::objectRemoved,
            [&](const ui::ObjectRemoved&) noexcept
            {
                assert(commits == 1 && !root.focusedPane());
                ++removals;
            }
        );
        assert(changed && removed && root.requestFocus(old_owner->get()) && root.capturePointer(old_owner->get()));
        auto owners = root.replacePanes(remove, std::span{invalid}.first(1), commit);
        assert(owners && owners->size() == 1 && !invalid[0]);
        assert(commits == 1 && notifications == 2 && removals == 1 && old.destroyed == 0);
        changed->disconnect();
        removed->disconnect();
        assert(root.update() && global.updated == 1 && old.updated == 0 && next.updated == 1);
        owners->clear();
        assert(old.destroyed == 1);
        unsigned stale_commits{};
        auto stale_commit = [&](std::span<const ui::PaneHandle> added) noexcept
        {
            assert(added.empty());
            ++stale_commits;
        };
        auto stale = root.replacePanes(remove, {}, stale_commit);
        assert(stale && stale->empty() && stale_commits == 1 && root.resolvePane(next_handle));
        assert(root.resolvePane(global_handle) && global.destroyed == 0);
        auto visit = [&](ui::Pane&) noexcept
        {
            auto busy = root.replacePanes({}, {}, stale_commit);
            assert(!busy && busy.error() == ui::EPaneError::BUSY);
        };
        assert(root.forEachPane(visit) && stale_commits == 1);
        assert(root.clearPanes());
    }

    void fallback(bool with_pane, bool handled, bool enabled, bool remove_before_dispatch, bool destroy_fallback)
    {
        auto made = ui::Root::create({.docking = false});
        assert(made);
        auto& root = **made;
        Counts local, application;
        auto fallback = std::make_unique<Fallback>(application);
        root.setCommandFallback(fallback.get());
        assert(root.setMenu({{{ui::CommandId{"test.global"}, "Global", "Ctrl+Z", {ui::EKey::Z, true}}}, {}}));
        Pane* pane{};
        if (with_pane)
        {
            auto candidate = std::make_unique<Pane>(local);
            pane = candidate.get();
            pane->accepts = handled;
            pane->enabled = enabled;
            assert(root.addPane(std::move(candidate)));
        }
        ui::DrawData draw;
        for (unsigned n{}; n != 3; ++n)
        {
            assert(root.update({{640, 480}, 0.016f}, draw));
        }
        if (pane)
        {
            assert(root.requestFocus(*pane));
        }
        assert(root.feedInput(ui::Key{ui::EKey::LEFT_CONTROL, true}));
        assert(root.feedInput(ui::Key{ui::EKey::Z, true}));
        assert(root.update({{640, 480}, 0.016f}, draw));
        assert(local.executed == 0 && application.executed == 0);
        if (remove_before_dispatch)
        {
            auto removed = root.removePane(*pane);
            assert(removed);
            removed->reset();
        }
        std::unique_ptr<Fallback> replacement;
        if (destroy_fallback)
        {
            fallback.reset();
            replacement = std::make_unique<Fallback>(application);
        }
        assert(root.update());
        assert(root.update());
        const bool should_execute = !remove_before_dispatch && !(handled && !enabled);
        assert(local.executed == unsigned(should_execute && handled));
        assert(application.executed == unsigned(should_execute && !handled && !destroy_fallback));
        if (handled)
        {
            assert(application.queried == 0);
        }
        root.setCommandFallback(nullptr);
        assert(root.clearPanes());
    }
} // namespace

int main()
{
    static_cast<void>(lux::object::ObjectRuntime::instance());
    replacement();
    fallback(false, false, true, false, false);
    fallback(true, true, true, false, false);
    fallback(true, true, false, false, false);
    fallback(true, false, true, false, false);
    fallback(true, false, true, true, false);
    fallback(false, false, true, false, true);
    mixedSafePointOrder();
    mixedTargetRemoval();
    actionProjection();
    std::cout << "PASS: atomic replacement, retained owners, stale handles, semantic commit and command fallback\n";
}
