#pragma once

#include <lux/engine/editor/inspector/InspectorReadOnlyContext.hpp>
#include <lux/engine/description/Visual.hpp>
#include <lux/engine/resource/identity/AssetId.hpp>
#include <lux/cxx/compile_time/TypeToken.hpp>

#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <type_traits>

namespace lux::editor::inspector
{
    template<class Value>
    [[nodiscard]] std::string readOnlyValueText(const Value& value)
    {
        if constexpr (std::is_same_v<Value, bool>)
            return value ? "true" : "false";
        else if constexpr (std::is_same_v<Value, asset::AssetId>)
            return value.isNull() ? "<none>" : uuids::to_string(value.uuid());
        else if constexpr (std::is_enum_v<Value>)
            return std::to_string(static_cast<std::underlying_type_t<Value>>(value));
        else if constexpr (requires(std::ostream& out) { out << value; })
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

    template<class Value>
    void drawReadOnlyField(InspectorReadOnlyContext& context, std::string_view label, const Value& value)
    {
        context.frame.propertyRow(label);
        context.frame.text(readOnlyValueText(value));
    }

    inline void drawReadOnlyField(
        InspectorReadOnlyContext& context, std::string_view label, const rdesc::MeshVisualDescription& value
    )
    {
        context.frame.propertyRow(label);
        context.frame.text("Mesh visual");
        drawReadOnlyField(context, "Mesh", value.mesh);
        drawReadOnlyField(context, "Material", value.material);
        drawReadOnlyField(context, "Visible", value.visible);
        drawReadOnlyField(context, "Cast shadow", value.cast_shadow);
        drawReadOnlyField(context, "Receive shadow", value.receive_shadow);
    }

    inline void drawReadOnlyField(
        InspectorReadOnlyContext& context, std::string_view label, const rdesc::LightDescription& value
    )
    {
        context.frame.propertyRow(label);
        context.frame.text("Light");
        drawReadOnlyField(context, "Type", value.type);
        drawReadOnlyField(context, "Red", value.color[0]);
        drawReadOnlyField(context, "Green", value.color[1]);
        drawReadOnlyField(context, "Blue", value.color[2]);
        drawReadOnlyField(context, "Intensity", value.intensity);
        drawReadOnlyField(context, "Range", value.range);
        drawReadOnlyField(context, "Cast shadow", value.cast_shadow);
    }
}
