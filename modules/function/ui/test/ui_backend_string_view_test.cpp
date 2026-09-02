#ifdef NDEBUG
#undef NDEBUG
#endif

#include <lux/engine/ui/detail/NullTerminatedText.hpp>

#include <array>
#include <cassert>
#include <string>
#include <string_view>

namespace
{
    void check(std::string_view input, std::string_view expected)
    {
        const lux::ui::detail::NullTerminatedText text{input};
        assert(std::string_view{text.c_str()} == expected);
        assert(text.c_str()[expected.size()] == '\0');
    }
}

int main()
{
    std::string source = "SaveXXX";
    check(std::string_view{source}.substr(0U, 4U), "Save");

    const std::array<char, 5U> raw{'V', 'a', 'l', 'u', 'e'};
    check(std::string_view{raw.data(), raw.size()}, "Value");

    check({}, {});
    check("Normal literal", "Normal literal");

    std::string long_text(300U, 'x');
    check(long_text, long_text);

    // The same adapter is used by button, scalar/input, format/hint,
    // table/tree/popup identifiers and menu/toolbar labels.
    return 0;
}
