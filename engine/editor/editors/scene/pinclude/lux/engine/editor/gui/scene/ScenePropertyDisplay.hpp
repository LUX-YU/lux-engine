#pragma once

#include <lux/engine/editor/gui/scene/ComponentReadBinding.hpp>
#include <lux/engine/description/Visual.hpp>
#include <charconv>
#include <span>
#include <array>

namespace lux::editor::gui
{
    template <class Number>
    void drawNumbers(lux::ui::Frame &frame, std::span<const Number> values)
    {
        std::array<char, 256> buffer;
        auto *end = buffer.data();
        const auto *limit = buffer.data() + buffer.size();
        for (const auto value : values)
        {
            if (end == limit)
            {
                frame.text("Value exceeds display width");
                return;
            }
            if (end != buffer.data())
            {
                *end++ = ' ';
            }
            const auto encoded = [&] {
                if constexpr (std::is_floating_point_v<Number>)
                {
                    return std::to_chars(end, buffer.data() + buffer.size(), value, std::chars_format::general, 5);
                }
                else
                {
                    return std::to_chars(end, buffer.data() + buffer.size(), value);
                }
            }();
            if (encoded.ec != std::errc{})
            {
                frame.text("Value exceeds display width");
                return;
            }
            end = encoded.ptr;
        }
        frame.text(std::string_view{buffer.data(), static_cast<std::size_t>(end - buffer.data())});
    }

    template <class T>
    void drawReadOnlyField(scene::SceneEditor &, lux::ui::Frame &frame, std::string_view label, const T &value)
    {
        frame.propertyRow(label);
        if constexpr (std::is_same_v<T, bool>)
        {
            frame.text(value ? "Yes" : "No");
        }
        else if constexpr (std::is_enum_v<T>)
        {
            const auto number = static_cast<std::underlying_type_t<T>>(value);
            drawNumbers(frame, std::span{&number, std::size_t{1}});
        }
        else if constexpr (std::is_arithmetic_v<T>)
        {
            drawNumbers(frame, std::span{&value, std::size_t{1}});
        }
        else if constexpr (requires { value.coeffs(); })
        {
            drawNumbers(frame, std::span{value.coeffs().data(), static_cast<std::size_t>(value.coeffs().size())});
        }
        else if constexpr (requires { value.transpose(); })
        {
            drawNumbers(frame, std::span{value.data(), static_cast<std::size_t>(value.size())});
        }
        else
        {
            frame.text("No read-only formatter for this type");
        }
    }

    inline void drawReadOnlyField(scene::SceneEditor &document, lux::ui::Frame &frame, std::string_view label,
                                  const lux::asset::AssetId &value)
    {
        frame.propertyRow(label);
        frame.text(document.project().assetName(value));
    }

    inline void drawReadOnlyField(scene::SceneEditor &document, lux::ui::Frame &frame, std::string_view,
                                  const lux::rdesc::MeshVisualDescription &value)
    {
        drawReadOnlyField(document, frame, "Mesh", value.mesh);
        drawReadOnlyField(document, frame, "Material", value.material);
        drawReadOnlyField(document, frame, "Visible", value.visible);
        drawReadOnlyField(document, frame, "Cast shadow", value.cast_shadow);
        drawReadOnlyField(document, frame, "Receive shadow", value.receive_shadow);
    }

    inline void drawReadOnlyField(scene::SceneEditor &document, lux::ui::Frame &frame, std::string_view,
                                  const lux::rdesc::LightDescription &value)
    {
        drawReadOnlyField(document, frame, "Type", value.type);
        frame.propertyRow("Color");
        drawNumbers(frame, std::span<const float>{value.color.data(), value.color.size()});
        drawReadOnlyField(document, frame, "Intensity", value.intensity);
        drawReadOnlyField(document, frame, "Range", value.range);
        drawReadOnlyField(document, frame, "Cast shadow", value.cast_shadow);
    }
} // namespace lux::editor::gui
