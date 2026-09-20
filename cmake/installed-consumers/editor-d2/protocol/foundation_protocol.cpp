#include <cassert>
#include <cstdio>
#include <imgui.h>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/object/detail/MessageEnvelope.hpp>
#include <lux/engine/ui/UISession.hpp>
#include <string>
#include <utility>
#include <vector>

namespace
{
    class DockProbe final : public lux::ui::Pane
    {
      public:
        DockProbe(lux::ui::UISession &ui, const char *id)
            : Pane(ui.dispatcherRef(), lux::ui::PaneId{id}, lux::ui::PaneTypeId{"dock-probe"}, id)
        {
        }

        ImGuiID dock{};
        ImVec2 extent;

      private:
        void draw(lux::ui::Frame &, lux::ui::PaneDrawContext &) override
        {
            assert(ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_DockingEnable);
            assert(!(ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable));
            assert(ImGui::IsWindowDocked());
            dock = ImGui::GetWindowDockID();
            extent = ImGui::GetWindowSize();
            ImGui::TextUnformatted("Dockable content");
        }
    };

    void checkDocking()
    {
        using namespace lux::ui;
        UISession ui{{.docking = true}};
        DockProbe left{ui, "left"}, center{ui, "center"};
        auto left_registration = ui.registerPane(left);
        auto center_registration = ui.registerPane(center);
        ui.setSplitLayout({"left", "center"});
        const auto draw = [](UISession &session)
        {
            auto frame = session.beginFrame({{1200, 800}, 1.0F / 60, {1, 1}});
            frame.drawPanes();
            frame.finish();
        };
        draw(ui);
        draw(ui);
        assert(left.dock != 0 && center.dock != 0 && left.dock != center.dock);
        const auto left_extent = left.extent;
        const auto left_node = left.dock;
        const auto center_node = center.dock;
        // The host may reapply its default, but must not overwrite live docking.
        auto new_default = SplitLayout{"left", "center"};
        new_default.left_width = 400;
        ui.setSplitLayout(new_default);
        draw(ui);
        assert(left.extent.x == left_extent.x && left.dock == left_node && center.dock == center_node);
        left.setVisible(false);
        draw(ui);
        left.setVisible(true);
        draw(ui);
        draw(ui);
        assert(left.dock == left_node && center.dock == center_node);
        const auto layout = ui.captureLayout();

        UISession restored{{.docking = true}};
        DockProbe restored_left{restored, "left"}, restored_center{restored, "center"};
        auto a = restored.registerPane(restored_left);
        auto b = restored.registerPane(restored_center);
        assert(restored.restoreLayout(layout));
        restored.setSplitLayout({"left", "center"});
        assert(restored.requestFocus(PaneIdView{"left"}));
        draw(restored);
        draw(restored);
        assert(restored.requestFocus(PaneIdView{"center"}));
        draw(restored);
        draw(restored);
        assert(restored_left.dock == left_node && restored_center.dock == center_node);
        std::puts("PASS UI docking: default splits, hide/restore, no per-frame reset, capture/restore; single native "
                  "viewport");
    }

    class TextProbe final : public lux::ui::Pane
    {
    public:
        explicit TextProbe(lux::ui::UISession &ui)
            : Pane(ui.dispatcherRef(), lux::ui::PaneId{"text-probe"}, lux::ui::PaneTypeId{"text-probe"}, "Text")
        {
        }

        std::string text;
        bool focus{true}, ctrl{}, shift{}, alt{};

    private:
        void draw(lux::ui::Frame &frame, lux::ui::PaneDrawContext &) override
        {
            if (std::exchange(focus, false))
            {
                ImGui::SetKeyboardFocusHere();
            }
            static_cast<void>(frame.inputText("Value", text));
            const auto &io = ImGui::GetIO();
            ctrl = io.KeyCtrl;
            shift = io.KeyShift;
            alt = io.KeyAlt;
        }
    };

    bool checkTextShortcuts()
    {
        using namespace lux::ui;
        auto made = UISession::create();
        assert(made);
        auto &ui = **made;
        TextProbe pane{ui};
        auto registration = ui.registerPane(pane);
        const auto draw = [&]()
        {
            auto frame = ui.beginFrame({{400, 300}, 1.0F / 60, {1, 1}});
            frame.drawPanes();
            frame.finish();
        };
        const auto key = [&](EKey code, bool down)
        {
            ui.feedInput(UiKey{code, down});
            draw();
        };
        draw();
        draw();
        ui.feedInput(UiText{U'x'});
        draw();
        assert(pane.text == "x");
        key(EKey::LEFT_CONTROL, true);
        if (!pane.ctrl)
        {
            std::puts("FAIL UI shortcut: physical LeftCtrl is down but ImGui aggregate Ctrl is false; text=x");
            return false;
        }
        key(EKey::RIGHT_CONTROL, true);
        key(EKey::LEFT_CONTROL, false);
        assert(pane.ctrl);
        key(EKey::Z, true);
        assert(pane.text.empty());
        key(EKey::Z, false);
        key(EKey::RIGHT_CONTROL, false);
        assert(!pane.ctrl);
        key(EKey::LEFT_SHIFT, true);
        key(EKey::RIGHT_SHIFT, true);
        key(EKey::LEFT_SHIFT, false);
        assert(pane.shift);
        key(EKey::RIGHT_SHIFT, false);
        assert(!pane.shift);
        key(EKey::LEFT_ALT, true);
        key(EKey::RIGHT_ALT, true);
        key(EKey::LEFT_ALT, false);
        assert(pane.alt);
        ui.feedInput(UiWindowFocus{false});
        draw();
        assert(!pane.ctrl && !pane.shift && !pane.alt);
        ui.feedInput(UiWindowFocus{true});
        draw();
        key(EKey::LEFT_ALT, true);
        key(EKey::LEFT_ALT, false);
        assert(!pane.alt);
        std::puts("PASS UI event-injection: InputText Ctrl+Z; paired Ctrl/Shift/Alt; focus loss clears modifiers");
        return true;
    }
}

int main()
{
    checkDocking();
    if (!checkTextShortcuts())
    {
        return 2;
    }
    using namespace lux;
    object::ObjectMessageQueue queue;
    const auto dispatcher = queue.dispatcherRef();
    std::vector<int> received;
    const auto post = [&](int value)
    {
        return object::detail::post(dispatcher,
                                    object::detail::makeMessage(
                                        [&, value]() noexcept
                                        {
                                            received.push_back(value);
                                            if (value == 1)
                                            {
                                                assert(object::detail::post(dispatcher, object::detail::makeMessage(
                                                                                            [&]() noexcept
                                                                                            {
                                                                                                received.push_back(4);
                                                                                            })) ==
                                                       object::detail::EPostStatus::POSTED);
                                            }
                                        }));
    };
    assert(post(1) == object::detail::EPostStatus::POSTED);
    assert(post(2) == object::detail::EPostStatus::POSTED);
    assert(post(3) == object::detail::EPostStatus::POSTED);
    assert(queue.dispatchPending(0) == 0 && received.empty());
    assert(queue.dispatchPending(2) == 2 && received == std::vector<int>({1, 2}));
    assert(queue.dispatchPending(1) == 1 && received.back() == 3);
    assert(queue.dispatchPending(64) == 1 && received.back() == 4);

    auto first = ui::UISession::create({}, dispatcher);
    auto second = ui::UISession::create({}, dispatcher);
    assert(first && second);
    assert(post(5) == object::detail::EPostStatus::POSTED);
    {
        auto frame = (*first)->beginFrame({{400, 300}, 1.0F / 60.0F, {1, 1}});
        frame.drawPanes();
        frame.finish();
    }
    assert(received.size() == 4);
    first->reset();
    assert(post(6) == object::detail::EPostStatus::POSTED);
    {
        auto frame = (*second)->beginFrame({{400, 300}, 1.0F / 60.0F, {1, 1}});
        frame.finish();
    }
    second->reset();
    assert(queue.dispatchPending(8) == 2 && received.back() == 6);
    auto owned = ui::UISession::create();
    assert(owned);
    const auto owned_dispatcher = (*owned)->dispatcherRef();
    bool dispatched{};
    assert(object::detail::post(owned_dispatcher, object::detail::makeMessage(
                                                      [&]() noexcept
                                                      {
                                                          dispatched = true;
                                                      })) == object::detail::EPostStatus::POSTED);
    {
        auto frame = (*owned)->beginFrame({{400, 300}, 1.0F / 60.0F, {1, 1}});
        frame.finish();
    }
    assert(dispatched);
    owned->reset();
    assert(object::detail::post(owned_dispatcher, object::detail::makeMessage(
                                                      []() noexcept
                                                      {
                                                      })) == object::detail::EPostStatus::CLOSED);
    queue.close();
    assert(post(7) == object::detail::EPostStatus::CLOSED);
    std::puts("PASS bounded FIFO/reentrant batch/borrowed UI/standalone UI/late closed dispatcher");
}
