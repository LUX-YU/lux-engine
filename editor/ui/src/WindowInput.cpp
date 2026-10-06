#include <lux/engine/editor/detail/WindowInput.hpp>
#include <lux/engine/input/InputSnapshot.hpp>
#include <optional>
namespace lux::editor
{
    namespace
    {
        [[nodiscard]] lux::ui::EKey uiKey(input::EKey key) noexcept
        {
            switch (key)
            {
            case input::EKey::KEY_A:
                return lux::ui::EKey::A;
            case input::EKey::KEY_B:
                return lux::ui::EKey::B;
            case input::EKey::KEY_C:
                return lux::ui::EKey::C;
            case input::EKey::KEY_D:
                return lux::ui::EKey::D;
            case input::EKey::KEY_E:
                return lux::ui::EKey::E;
            case input::EKey::KEY_F:
                return lux::ui::EKey::F;
            case input::EKey::KEY_G:
                return lux::ui::EKey::G;
            case input::EKey::KEY_H:
                return lux::ui::EKey::H;
            case input::EKey::KEY_I:
                return lux::ui::EKey::I;
            case input::EKey::KEY_J:
                return lux::ui::EKey::J;
            case input::EKey::KEY_K:
                return lux::ui::EKey::K;
            case input::EKey::KEY_L:
                return lux::ui::EKey::L;
            case input::EKey::KEY_M:
                return lux::ui::EKey::M;
            case input::EKey::KEY_N:
                return lux::ui::EKey::N;
            case input::EKey::KEY_O:
                return lux::ui::EKey::O;
            case input::EKey::KEY_P:
                return lux::ui::EKey::P;
            case input::EKey::KEY_Q:
                return lux::ui::EKey::Q;
            case input::EKey::KEY_R:
                return lux::ui::EKey::R;
            case input::EKey::KEY_S:
                return lux::ui::EKey::S;
            case input::EKey::KEY_T:
                return lux::ui::EKey::T;
            case input::EKey::KEY_U:
                return lux::ui::EKey::U;
            case input::EKey::KEY_V:
                return lux::ui::EKey::V;
            case input::EKey::KEY_W:
                return lux::ui::EKey::W;
            case input::EKey::KEY_X:
                return lux::ui::EKey::X;
            case input::EKey::KEY_Y:
                return lux::ui::EKey::Y;
            case input::EKey::KEY_Z:
                return lux::ui::EKey::Z;
            case input::EKey::KEY_LEFT_SHIFT:
                return lux::ui::EKey::LEFT_SHIFT;
            case input::EKey::KEY_RIGHT_SHIFT:
                return lux::ui::EKey::RIGHT_SHIFT;
            case input::EKey::KEY_LEFT_CONTROL:
                return lux::ui::EKey::LEFT_CONTROL;
            case input::EKey::KEY_RIGHT_CONTROL:
                return lux::ui::EKey::RIGHT_CONTROL;
            case input::EKey::KEY_LEFT_ALT:
                return lux::ui::EKey::LEFT_ALT;
            case input::EKey::KEY_RIGHT_ALT:
                return lux::ui::EKey::RIGHT_ALT;
            case input::EKey::KEY_TAB:
                return lux::ui::EKey::TAB;
            case input::EKey::KEY_ENTER:
                return lux::ui::EKey::ENTER;
            case input::EKey::KEY_ESCAPE:
                return lux::ui::EKey::ESCAPE;
            case input::EKey::KEY_SPACE:
                return lux::ui::EKey::SPACE;
            case input::EKey::KEY_BACKSPACE:
                return lux::ui::EKey::BACKSPACE;
            case input::EKey::KEY_DELETE:
                return lux::ui::EKey::DELETE_KEY;
            case input::EKey::KEY_LEFT:
                return lux::ui::EKey::LEFT;
            case input::EKey::KEY_RIGHT:
                return lux::ui::EKey::RIGHT;
            case input::EKey::KEY_UP:
                return lux::ui::EKey::UP;
            case input::EKey::KEY_DOWN:
                return lux::ui::EKey::DOWN;
            case input::EKey::KEY_HOME:
                return lux::ui::EKey::HOME;
            case input::EKey::KEY_END:
                return lux::ui::EKey::END;
            default:
                return lux::ui::EKey::NONE;
            }
        }

        [[nodiscard]] std::optional<lux::ui::EPointerButton> pointerButton(input::EMouseButton button) noexcept
        {
            switch (button)
            {
            case input::EMouseButton::MOUSE_BUTTON_LEFT:
                return lux::ui::EPointerButton::LEFT;
            case input::EMouseButton::MOUSE_BUTTON_MIDDLE:
                return lux::ui::EPointerButton::MIDDLE;
            case input::EMouseButton::MOUSE_BUTTON_RIGHT:
                return lux::ui::EPointerButton::RIGHT;
            default:
                return std::nullopt;
            }
        }

    } // namespace

    lux::cxx::expected<void, lux::ui::EInputError> feedWindowInput(
        lux::ui::Root& root,
        const input::InputSnapshot& snapshot
    ) noexcept
    {
        lux::cxx::expected<void, lux::ui::EInputError> result;
        const auto feedPlatformInput = [&](const lux::ui::VInputEvent& event, std::uint64_t sequence)
        {
            if (result)
            {
                result = root.feedInput(event, sequence);
            }
        };
        for (const auto& event : snapshot.events)
        {
            std::visit(
                [&](const auto& value)
                {
                    using Value = std::remove_cvref_t<decltype(value)>;
                    if constexpr (std::same_as<Value, input::KeyAction>)
                    {
                        const auto key = uiKey(value.key);
                        if (key != lux::ui::EKey::NONE)
                        {
                            feedPlatformInput(
                                lux::ui::Key{key, value.state != input::EInputState::RELEASE},
                                value.sequence
                            );
                        }
                    }
                    else if constexpr (std::same_as<Value, input::MouseButtonAction>)
                    {
                        const auto button = pointerButton(value.button);
                        if (button)
                        {
                            feedPlatformInput(
                                lux::ui::PointerButton{*button, value.state != input::EInputState::RELEASE},
                                value.sequence
                            );
                        }
                    }
                    else if constexpr (std::same_as<Value, input::MouseScrollAction>)
                    {
                        feedPlatformInput(
                            lux::ui::PointerWheel{{static_cast<float>(value.x), static_cast<float>(value.y)}},
                            value.sequence
                        );
                    }
                    else if constexpr (std::same_as<Value, input::CharInput>)
                    {
                        feedPlatformInput(lux::ui::Text{static_cast<char32_t>(value.codepoint)}, value.sequence);
                    }
                    else if constexpr (std::same_as<Value, input::CursorAction>)
                    {
                        feedPlatformInput(lux::ui::PointerMove{{float(value.x), float(value.y)}}, value.sequence);
                    }
                    else if constexpr (std::same_as<Value, input::FocusAction>)
                    {
                        feedPlatformInput(lux::ui::WindowFocus{value.focused}, value.sequence);
                    }
                    else if constexpr (std::same_as<Value, input::CompositionAction>)
                    {
                        lux::ui::ECompositionStage stage{};
                        switch (value.stage)
                        {
                        case input::ECompositionStage::STARTED:
                            stage = lux::ui::ECompositionStage::STARTED;
                            break;
                        case input::ECompositionStage::UPDATED:
                            stage = lux::ui::ECompositionStage::UPDATED;
                            break;
                        case input::ECompositionStage::COMMITTED:
                            stage = lux::ui::ECompositionStage::COMMITTED;
                            break;
                        case input::ECompositionStage::CANCELLED:
                            stage = lux::ui::ECompositionStage::CANCELLED;
                            break;
                        }
                        feedPlatformInput(lux::ui::Composition{stage}, value.sequence);
                    }
                },
                event
            );
        }
        return result;
    }
} // namespace lux::editor
