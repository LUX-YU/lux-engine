#pragma once

#include <lux/engine/ui/Frame.hpp>
#include <lux/cxx/compile_time/TypeToken.hpp>

#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <type_traits>

namespace lux::editor::ui
{
    template <class Value> [[nodiscard]] std::string readOnlyValueText(const Value &value)
    {
        if constexpr (std::is_same_v<Value, bool>)
            return value ? "true" : "false";
        else if constexpr (std::is_enum_v<Value>)
            return std::to_string(static_cast<std::underlying_type_t<Value>>(value));
        else if constexpr (requires(std::ostream &out) { out << value; })
        {
            std::ostringstream out;
            out.imbue(std::locale::classic());
            out << std::setprecision(17) << value;
            return out.str();
        }
        else if constexpr (requires { value.coeffs(); })
            return readOnlyValueText(value.coeffs());
        else
            return std::string{lux::cxx::typeToken<Value>().name()} + " <read-only display unavailable>";
    }

    template <class Value> void drawReadOnlyField(lux::ui::Frame &frame, std::string_view label, const Value &value)
    {
        frame.propertyRow(label);
        frame.text(readOnlyValueText(value));
    }

} // namespace lux::editor::ui
