#include "UiTestHelpers.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Root.hpp>
#include <memory>
#include <vector>

using namespace lux;

namespace
{
    class ExternalControl final : public ui::Element
    {
    public:
        bool hasMenu() const noexcept
        {
            return menuActive();
        }

        unsigned queries{}, executions{};

        void event(object::EventView& event) noexcept override
        {
            auto* command = event.getIf<ui::Command>();
            if (!command || command->id != ui::CommandIdView{"test.external.action"})
            {
                return;
            }
            command->enabled = true;
            if (command->phase == ui::ECommandPhase::QUERY)
            {
                ++queries;
            }
            else
            {
                ++executions;
            }
            event.accept();
        }

        void draw() noexcept override {}
    };

    class CommandPane final : public ui::Pane
    {
    public:
        CommandPane() : Pane("Custom commands")
        {
            assert(addElement(control));
        }

        void event(object::EventView& event) noexcept override
        {
            control.event(event);
        }

        ExternalControl control;
    };

    void shortcuts()
    {
        ExternalControl detached;
        assert(!detached.hasMenu());
        auto made = ui::Root::create({.docking = false});
        assert(made);
        auto& root = **made;
        auto owner = std::make_unique<CommandPane>();
        auto& pane = *owner;
        std::unique_ptr<ui::Pane> candidate = std::move(owner);
        assert(root.addPane(std::move(candidate)));
        ui::DrawData data;
        for (unsigned n{}; n < 3; ++n)
        {
            assert(root.update({{640, 480}, 0.016F}, data));
        }
        assert(root.requestFocus(pane));
        // No implicit editing command when the product supplies no menu binding.
        assert(root.feedInput(ui::Key{ui::EKey::LEFT_CONTROL, true}));
        assert(root.feedInput(ui::Key{ui::EKey::Z, true}));
        assert(root.update({{640, 480}, 0.016F}, data));
        assert(root.update());
        assert(pane.control.executions == 0);
        assert(root.feedInput(ui::Key{ui::EKey::Z, false}));
        for (unsigned n{}; n < 2; ++n)
        {
            assert(root.update({{640, 480}, 0.016F}, data));
        }
        assert(root.setMenu({{{ui::CommandId{"test.external.action"}, "External", "Ctrl+K", {ui::EKey::K, true}}}, {}})
        );
        // Creating the menu bar is a new ImGui window. Establish focus after that first frame.
        assert(root.update({{640, 480}, 0.016F}, data));
        assert(root.requestFocus(pane));
        assert(root.update({{640, 480}, 0.016F}, data));
        assert(root.focusedPane() == &pane);
        assert(root.feedInput(ui::Key{ui::EKey::K, true}));
        assert(root.update({{640, 480}, 0.016F}, data));
        assert(pane.control.executions == 0 && pane.control.queries == 1);
        assert(root.update());
        assert(pane.control.executions == 1 && pane.control.queries == 2);
        assert(root.update());
        assert(pane.control.executions == 1);
    }

    class Tree final : public ui::Pane
    {
    public:
        explicit Tree(unsigned count) : Pane("Statistics")
        {
            assert(addElement(layout));
            for (unsigned index{}; index < count; ++index)
            {
                auto label = std::make_unique<ui::Label>("Retained content");
                assert(layout.addElement(*label));
                labels.push_back(std::move(label));
            }
        }

        ui::Layout layout;
        std::vector<std::unique_ptr<ui::Label>> labels;
    };

    void statistics(unsigned panes, unsigned elements)
    {
        auto made = ui::Root::create();
        assert(made);
        auto& root = **made;
        for (unsigned n{}; n < panes; ++n)
        {
            std::unique_ptr<ui::Pane> pane = std::make_unique<Tree>(elements);
            assert(root.addPane(std::move(pane)));
        }
        ui::DrawData data;
        std::vector<double> maintenance, draw, capture;
        for (unsigned n{}; n < 35; ++n)
        {
            assert(root.update({{1280, 800}, 0.016F}, data));
            const auto value = root.statistics();
            assert(value.panes == panes && value.elements == panes * (elements + 1));
            assert(value.captured && value.draw_vertices && value.draw_indices && value.draw_commands);
            if (n >= 5)
            {
                maintenance.push_back(value.maintenance.count() / 1000.);
                draw.push_back(value.draw.count() / 1000.);
                capture.push_back(value.capture.count() / 1000.);
            }
        }
        const auto report = [&](const char* phase, std::vector<double>& samples)
        {
            std::ranges::sort(samples);
            std::printf(
                "UI panes=%u elements_per_pane=%u phase=%s us_p50=%.3f us_p95=%.3f us_max=%.3f warmup=5 samples=30\n",
                panes,
                elements,
                phase,
                samples[15],
                samples[28],
                samples.back()
            );
        };
        report("maintenance", maintenance);
        report("draw", draw);
        report("capture", capture);
        auto hide = [](ui::Pane& pane) noexcept { pane.setVisible(false); };
        assert(root.forEachPane(hide));
        assert(root.update());
        const auto idle = root.statistics();
        assert(idle.panes == panes && idle.elements == panes * (elements + 1));
        assert(!idle.captured && idle.draw.count() == 0 && idle.capture.count() == 0 && idle.draw_vertices == 0);
    }
} // namespace

int main()
{
    shortcuts();
    for (auto panes : {1U, 3U})
    {
        for (auto elements : {100U, 1000U})
        {
            statistics(panes, elements);
        }
    }
}
