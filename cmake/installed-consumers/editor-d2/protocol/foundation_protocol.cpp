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
