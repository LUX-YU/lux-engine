#pragma once
#include "../../../../cmake/installed-consumers/common/UiTestContent.hpp"
#include "UiTestHelpers.hpp"
#include <cassert>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/ui/Command.hpp>
#include <thread>

namespace api_checks
{
    using namespace lux;
    class CommandPane final : public ui::Pane
    {
    public:
        CommandPane() : Pane("Identical title") {}
        unsigned queries{}, executions{};
        bool enabled{true};

    private:
        void event(object::EventView& event) noexcept override
        {
            if (auto* command = event.getIf<ui::Command>())
            {
                assert(command->id == ui::CommandIdView{"test.owned.menu"});
                if (command->phase == ui::ECommandPhase::QUERY)
                {
                    ++queries;
                    command->enabled = enabled;
                }
                else
                {
                    ++executions;
                }
                event.accept();
            }
        }
    };
    inline void run()
    {
        auto created = ui::Root::create();
        auto other = ui::Root::create();
        assert(created && other);
        auto& root = **created;
        auto& a = ui_test::makePane<CommandPane>(root);
        auto& b = ui_test::makePane<CommandPane>(root);
        auto& foreign = ui_test::makePane<CommandPane>(**other);
        ui::DockTree tree{
            {{ui::EDockSplit::LEAF, UINT32_MAX, UINT32_MAX, .5F, {ui_test::paneHandle(a)}}},
            {{0, {{0, 0}, {640, 480}}, false}}
        };
        assert(root.setDockTree(tree));
        auto captured = root.captureDockTree();
        assert(
            captured.nodes.size() == 2 && captured.nodes[0].panes == std::vector<ui::PaneHandle>{ui_test::paneHandle(a)}
        );
        assert(
            captured.nodes[1].panes == std::vector<ui::PaneHandle>{ui_test::paneHandle(b)} &&
            captured.surfaces[1].floating
        );
        const auto refused = [&](ui::DockTree invalid)
        {
            auto result = root.setDockTree(std::move(invalid));
            assert(!result && result.error() == ui::EDockError::INVALID_DATA);
            assert(root.captureDockTree().nodes[0].panes == std::vector<ui::PaneHandle>{ui_test::paneHandle(a)});
        };
        auto invalid = tree;
        invalid.nodes[0].panes.push_back(ui_test::paneHandle(a));
        refused(invalid);
        invalid.nodes[0].panes = {ui_test::paneHandle(foreign)};
        refused(invalid);
        invalid.nodes[0].panes = {{}};
        refused(invalid);
        invalid = tree;
        invalid.nodes[0].first = 0;
        refused(invalid);
        b.setModal(true);
        invalid = tree;
        invalid.nodes[0].panes = {ui_test::paneHandle(b)};
        refused(invalid);
        b.setModal(false);
        auto busy = [&](ui::Pane&) noexcept
        {
            auto result = root.setDockTree(tree);
            assert(!result && result.error() == ui::EDockError::BUSY);
        };
        assert(root.forEachPane(busy));
        std::thread worker(
            [&]
            {
                auto result = root.setDockTree(tree);
                assert(!result && result.error() == ui::EDockError::WRONG_THREAD);
            }
        );
        worker.join();
        ui::DrawData draw;
        const auto frame = [&] { assert(root.update({{640, 480}, .016F}, draw)); };
        for (unsigned i{}; i < 3; ++i)
        {
            frame();
        }
        captured = root.captureDockTree();
        assert(root.setDockTree(captured));
        frame();
        // Every text/command value survives its builder. No source token is necessary.
        {
            ui::MenuItem
                action{ui::CommandId{"test.owned.menu"}, std::string("Execute"), std::string("K"), {ui::EKey::K}};
            ui::MenuItem menu{{}, std::string("Actions"), {}, {}, {std::move(action)}};
            root.setMenu({std::move(menu)});
        }
        assert(root.menu()[0].children[0].label == "Execute");
        assert(root.requestFocus(a));
        frame();
        assert(root.feedInput(ui::Key{ui::EKey::K, true}));
        frame();
        assert(a.queries && a.executions == 0 && b.executions == 0);
        assert(root.requestFocus(b));
        ui_test::apply(root);
        assert(a.executions == 1 && b.executions == 0);
        assert(root.feedInput(ui::Key{ui::EKey::K, false}));
        frame();
        assert(root.requestFocus(a));
        frame();
        assert(root.feedInput(ui::Key{ui::EKey::K, true}));
        frame();
        auto removed = root.removePane(a);
        assert(removed);
        ui_test::apply(root);
        assert(a.executions == 1 && b.executions == 0);
        // Remounting the same object must not revive a queued command from its prior registration.
        assert(root.addPane(std::move(*removed)));
        ui_test::apply(root);
        assert(a.executions == 1);
    }
} // namespace api_checks
