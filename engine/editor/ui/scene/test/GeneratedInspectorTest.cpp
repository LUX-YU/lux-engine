#include "InspectorFixtureSession.hpp"
#include <InspectorWidget.hpp>
#if !defined(__LUX_PARSE_TIME__)
#include <fixture_inspectors.inspector.generated.hpp>
#endif
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/ui/UISession.hpp>
#include <cassert>
#include <cstdio>

namespace
{
    class FixturePane final : public lux::object::Object<FixturePane, lux::ui::Pane>
    {
    public:
        explicit FixturePane(lux::object::ObjectDispatcherRef dispatcher)
            : Object(dispatcher, lux::ui::PaneId{"generated.test"}, lux::ui::PaneTypeId{"test"}, "Generated") {}
        inspector_test::FixtureSession model;
        inspector_fixture::Component value;
        bool preview{};
        std::array<char, 192> last_error{};
        lux::editor::ui::InspectorInteraction interaction;
        unsigned calls{};
        void draw(lux::ui::Frame &frame, lux::ui::PaneDrawContext &) override
        {
            auto table = frame.table({lux::ui::WidgetIdView{"fields"}, 2, false, false, false, 140});
            if (!table.visible()) return;
#if !defined(__LUX_PARSE_TIME__)
            // Generated names are deliberately referenced from a manifest checked by the CMake wrapper.
            if (!preview) value = model.value();
            interaction.error.fill('\0');
            const auto edit = generatedDraw(value, interaction);
            if (interaction.error[0])
            {
                last_error = interaction.error;
                preview = false;
            }
            else
            {
                preview |= edit.changed;
                if (edit.committed && preview)
                {
                    model.commit(value);
                    preview = false;
                }
            }
#endif
            ++calls;
        }
        static lux::ui::EditResult generatedDraw(inspector_fixture::Component &value,
                                                lux::editor::ui::InspectorInteraction &interaction)
        {
#if !defined(__LUX_PARSE_TIME__)
            return lux::editor::ui::generated::draw_inspector_fixture__Component_2be435d4(value, interaction);
#else
            return {};
#endif
        }
    };
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    lux::meta::ReflectionRegistry::initRegistry();
    {
        lux::ui::UISession session;
        FixturePane pane(session.dispatcherRef());
        auto registration = session.registerPane(pane);
        assert(registration);
        session.setSplitLayout({"", "generated.test", "", ""});
        float viewport_width = 1000;
        const auto step = [&] {
            inspector_test::count = 0;
            auto frame = session.beginFrame({{viewport_width, 4200}, 1.0F / 60.0F, {1, 1}});
            frame.drawPanes();
            frame.finish();
            auto snapshot = session.captureFrame();
            assert(snapshot && snapshot->valid() && !inspector_test::depth);
        };
        const auto click = [&](std::string_view field, std::string_view kind, unsigned index = 0) {
            const auto item = inspector_test::find(field, kind, index);
            std::printf("click %.*s/%.*s [%.1f,%.1f]-[%.1f,%.1f]\n", int(field.size()), field.data(),
                int(kind.size()), kind.data(), item.low.x, item.low.y, item.high.x, item.high.y);
            session.feedInput(lux::ui::UiPointerMove{{(item.low.x + item.high.x) / 2,
                                                    (item.low.y + item.high.y) / 2}});
            step();
            session.feedInput(lux::ui::UiPointerButton{lux::ui::EPointerButton::LEFT, true}); step();
            session.feedInput(lux::ui::UiPointerButton{lux::ui::EPointerButton::LEFT, false}); step();
            step();
        };
        session.feedInput(lux::ui::UiWindowFocus{true});
        for (unsigned i = 0; i < 4; ++i) step();
        const auto original = pane.model.value();
        assert(original.precise == 2 && original.map.at(1) == "one");
        for (const float width : {340.0F, 1000.0F})
        {
            viewport_width = width;
            for (unsigned i = 0; i < 4; ++i) step();
            for (const auto field : {"vector", "rotation"})
            {
                const auto x = inspector_test::find(field, "value", 0);
                const auto y = inspector_test::find(field, "value", 1);
                const auto z = inspector_test::find(field, "value", 2);
                assert(x.low.y == y.low.y && y.low.y == z.low.y);
                assert(x.high.x < y.low.x && y.high.x < z.low.x && z.high.x <= width);
                std::printf("compact %s width=%.0f y=%.1f bounds=%.1f..%.1f\n",
                    field, width, x.low.y, x.low.x, z.high.x);
            }
        }
        for (unsigned axis = 0; axis < 3; ++axis)
        {
            const auto item = inspector_test::find("vector", "value", axis);
            const float x = (item.low.x + item.high.x) / 2, y = (item.low.y + item.high.y) / 2;
            session.feedInput(lux::ui::UiPointerMove{{x, y}}); step();
            session.feedInput(lux::ui::UiPointerButton{lux::ui::EPointerButton::LEFT, true}); step();
            session.feedInput(lux::ui::UiPointerMove{{x + 30, y}}); step();
            session.feedInput(lux::ui::UiPointerButton{lux::ui::EPointerButton::LEFT, false}); step();
            assert(pane.model.value().vector[axis] != original.vector[axis]);
            for (unsigned other = 0; other < 3; ++other)
                if (other != axis) assert(pane.model.value().vector[other] == original.vector[other]);
            assert(pane.model.history->undo() && pane.model.value() == original);
            for (unsigned i = 0; i < 24; ++i) step();
        }
        click("enabled", "value");
        assert(pane.model.value().enabled && pane.model.history->view()->snapshot.entry_count == 1);
        assert(pane.model.history->undo() && pane.model.value() == original);
        assert(pane.model.history->redo() && pane.model.value().enabled);
        step();
        for (const auto field : {"records", "deque", "list", "bits", "map", "hash_map", "set", "hash_set"})
        {
            const auto before = pane.model.value();
            const auto revision = pane.model.history->view()->snapshot.revision.value;
            click(field, "Add");
            assert(pane.model.history->view()->snapshot.revision.value == revision + 1);
            const auto added = pane.model.value();
            assert(added != before);
            assert(pane.model.history->undo() && pane.model.value() == before);
            assert(pane.model.history->redo() && pane.model.value() == added);
            step();
            click(field, "Remove");
            assert(pane.model.value() != added);
            assert(pane.model.history->undo() && pane.model.value() == added);
            step();
        }
        const auto optional_before = pane.model.value();
        click("optional", "Present");
        assert(!pane.model.value().optional && pane.model.history->undo());
        assert(pane.model.value() == optional_before); step();
        click("variant", "Type");
        click("variant", "alternative-0");
        assert(pane.model.value().variant.index() == 0);
        assert(pane.model.history->undo() && pane.model.value() == optional_before); step();
        const auto before_failure = pane.model.value();
        click("growing", "Add");
        assert(pane.model.value().growing.size() == before_failure.growing.size() + 1);
        assert(pane.model.history->undo() && pane.model.value() == before_failure);
        step();
        // Renaming a sorted key is staged until deactivation; sorting must not redirect the active drag.
        const auto key_before = pane.model.value();
        const auto key_stamp = pane.model.history->view()->snapshot;
        const auto key_item = inspector_test::find("map");
        const float key_x = (key_item.low.x + key_item.high.x) / 2;
        const float key_y = (key_item.low.y + key_item.high.y) / 2;
        session.feedInput(lux::ui::UiPointerMove{{key_x, key_y}}); step();
        session.feedInput(lux::ui::UiPointerButton{lux::ui::EPointerButton::LEFT, true}); step();
        session.feedInput(lux::ui::UiPointerMove{{key_x + 30, key_y}}); step();
        session.feedInput(lux::ui::UiPointerMove{{key_x + 60, key_y}}); step();
        assert(pane.model.value() == key_before);
        session.feedInput(lux::ui::UiPointerButton{lux::ui::EPointerButton::LEFT, false}); step(); step();
        assert(pane.model.history->view()->snapshot.revision.value == key_stamp.revision.value + 1);
        assert(pane.model.value().map.contains(1) && pane.model.value().map.at(1) == "one");
        assert(pane.model.value().map.size() == key_before.map.size() && !pane.model.value().map.contains(0));
        assert(pane.model.history->undo() && pane.model.value() == key_before); step();
        // Text uses the real UiText route; this is not native IME or a font/glyph assertion.
        click("caption", "value");
        session.feedInput(lux::ui::UiKey{lux::ui::EKey::END, true}); step();
        session.feedInput(lux::ui::UiKey{lux::ui::EKey::END, false}); step();
        session.feedInput(lux::ui::UiText{U'Z'}); step();
        session.feedInput(lux::ui::UiText{U'你'}); step();
        assert(session.inputSnapshot().keyboard_blocked);
        session.feedInput(lux::ui::UiPointerMove{{900, 4000}}); step();
        session.feedInput(lux::ui::UiPointerButton{lux::ui::EPointerButton::LEFT, true}); step();
        session.feedInput(lux::ui::UiPointerButton{lux::ui::EPointerButton::LEFT, false}); step();
        assert(pane.model.value().caption == before_failure.caption + "Z你");
        assert(pane.model.history->undo() && pane.model.value() == before_failure);
        step();
        // A duplicate insertion is an exact negative, preserving the complete model and history.
        const auto before_duplicate = pane.model.value();
        const auto stamp = pane.model.history->view()->snapshot;
        click("map", "Add");
        assert(pane.model.value() == before_duplicate);
        assert(std::string_view{pane.last_error.data()}.find("duplicates an existing key") != std::string_view::npos);
        assert(pane.model.history->view()->snapshot.revision == stamp.revision);
        // The same owned operation survives a failed business preparation and succeeds on retry.
        auto candidate = pane.model.value();
        candidate.nested.amount = 4;
        auto operation = pane.model.operation(candidate);
        const auto *identity = operation.get();
        pane.model.reject = true;
        const auto rejected = pane.model.history->execute(operation);
        assert(!rejected && rejected.error().domain_code == 812 && operation.get() == identity);
        assert(pane.model.value() == before_duplicate);
        assert(pane.model.history->view()->snapshot.current == stamp.current);
        pane.model.reject = false;
        assert(pane.model.history->execute(operation) && !operation && pane.model.value() == candidate);
        assert(pane.model.history->undo() && pane.model.value() == before_duplicate);
        // Read-only applies recursively, including structural container buttons.
        pane.interaction.read_only = true; step();
        click("enabled", "value"); click("records", "Add");
        assert(pane.model.value() == before_duplicate);
        assert(pane.model.value().derived == 9 && pane.model.value().hidden == 42);
        registration->reset();
    }
    lux::meta::ReflectionRegistry::destroyRegistry();
    std::puts("Generated Inspector rendering, input, container history, "
              "duplicate rejection and same-operation retry PASS");
}
