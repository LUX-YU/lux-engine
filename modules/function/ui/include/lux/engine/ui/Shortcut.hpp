#pragma once
#include <lux/engine/ui/InputEvent.hpp>
#include <lux/cxx/compile_time/expected.hpp>
#include <string_view>

namespace lux::ui
{
    struct Shortcut final
    {
        EKey key{EKey::NONE};
        bool control{}, shift{}, alt{};
        friend bool operator==(const Shortcut&, const Shortcut&) = default;
    };
    enum class EShortcutError : std::uint8_t
    {
        UNKNOWN_KEY,
        UNKNOWN_MODIFIER,
        DUPLICATE_MODIFIER,
        MODIFIER_ORDER,
        MISSING_KEY
    };
    using ShortcutResult = cxx::expected<Shortcut, EShortcutError>;

    // Cold canonical parsing. Empty text explicitly removes a binding; supported keys and modifier
    // order match the original UI protocol. Native Ctrl/Shift/Alt mapping remains in the input owner.
    [[nodiscard]] inline ShortcutResult parseShortcut(std::string_view text) noexcept
    {
        Shortcut result;
        if (text.empty())
            return result;
        unsigned modifiers{}, previous{};
        for (auto plus = text.find('+'); plus != std::string_view::npos; plus = text.find('+'))
        {
            const auto modifier = text.substr(0, plus);
            unsigned order{};
            if (modifier == "Ctrl")
                order = 1;
            else if (modifier == "Shift")
                order = 2;
            else if (modifier == "Alt")
                order = 3;
            else
                return cxx::unexpected(EShortcutError::UNKNOWN_MODIFIER);
            const auto bit = 1u << order;
            if ((modifiers & bit) != 0)
                return cxx::unexpected(EShortcutError::DUPLICATE_MODIFIER);
            if (order < previous)
                return cxx::unexpected(EShortcutError::MODIFIER_ORDER);
            modifiers |= bit;
            previous = order;
            text.remove_prefix(plus + 1);
        }
        if (text.empty())
            return cxx::unexpected(EShortcutError::MISSING_KEY);
        result.control = (modifiers & (1u << 1)) != 0;
        result.shift = (modifiers & (1u << 2)) != 0;
        result.alt = (modifiers & (1u << 3)) != 0;
        const bool is_letter = text.size() == 1 && text.front() >= 'A' && text.front() <= 'Z';
        if (is_letter)
            result.key = static_cast<EKey>(static_cast<unsigned>(EKey::A) + text.front() - 'A');
        else if (text == "Delete")
            result.key = EKey::DELETE_KEY;
        else if (text == "Enter")
            result.key = EKey::ENTER;
        else if (text == "Escape")
            result.key = EKey::ESCAPE;
        else
            return cxx::unexpected(EShortcutError::UNKNOWN_KEY);
        return result;
    }
}
