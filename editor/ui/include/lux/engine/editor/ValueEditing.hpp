#pragma once

#include <lux/engine/resource/identity/AssetId.hpp>

#include <Eigen/Geometry>
#include <imgui.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>

namespace lux::editor
{
    // UI-owned control policy. This is neither a type description nor a
    // domain constraint: callers edit a draft and validate it before adoption.
    struct ValueEditOptions final
    {
        const char* label = "";
        float speed = 0.01F;
        std::optional<double> minimum;
        std::optional<double> maximum;
        const char* tooltip = "";
        bool read_only{};
        bool color{};
    };

    template <class T> struct EnumChoice final
    {
        T value;
        const char* label;
    };

    template <class T> constexpr const char* valueLabel() noexcept
    {
        return "Value";
    }

    template <class T> bool editValue(const ValueEditOptions&, T&) noexcept;

    // Generated code specializes only concrete value edits. There is no runtime
    // field lookup; this wrapper scopes the widget ID and read-only state.
    template <class T> bool editField(const char* identity, T& value, const ValueEditOptions& options) noexcept
    {
        ImGui::PushID(identity);
        ImGui::BeginDisabled(options.read_only);
        const bool edited = editValue(options, value);
        ImGui::EndDisabled();
        if (options.tooltip[0] != '\0' && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("%s", options.tooltip);
        }
        ImGui::PopID();
        return edited;
    }

    template <class T>
    bool editEnumeration(const ValueEditOptions& options, T& value, std::span<const EnumChoice<T>> choices) noexcept
    {
        const char* selected = "Unknown value";
        for (const auto& choice : choices)
        {
            if (choice.value == value)
            {
                selected = choice.label;
                break;
            }
        }
        bool edited{};
        if (ImGui::BeginCombo(options.label, selected))
        {
            for (const auto& choice : choices)
            {
                if (ImGui::Selectable(choice.label, choice.value == value))
                {
                    value = choice.value;
                    edited = true;
                }
            }
            ImGui::EndCombo();
        }
        return edited;
    }

    namespace detail
    {
        template <class T> constexpr ImGuiDataType numericWidgetType() noexcept
        {
            if constexpr (std::same_as<T, float>)
            {
                return ImGuiDataType_Float;
            }
            else if constexpr (std::same_as<T, double>)
            {
                return ImGuiDataType_Double;
            }
            else
            {
                constexpr ImGuiDataType
                    signed_types[]{ImGuiDataType_S8, ImGuiDataType_S16, ImGuiDataType_S32, ImGuiDataType_S64};
                constexpr ImGuiDataType
                    unsigned_types[]{ImGuiDataType_U8, ImGuiDataType_U16, ImGuiDataType_U32, ImGuiDataType_U64};
                constexpr std::size_t index = sizeof(T) == 8 ? 3 : sizeof(T) == 4 ? 2 : sizeof(T) == 2 ? 1 : 0;
                return std::is_signed_v<T> ? signed_types[index] : unsigned_types[index];
            }
        }

        template <class T, std::size_t... I>
        bool editVariant(const ValueEditOptions& options, T& value, std::index_sequence<I...>) noexcept
        {
            static constexpr std::array labels{valueLabel<std::variant_alternative_t<I, T>>()...};
            bool edited{};
            if (ImGui::BeginCombo(options.label, value.valueless_by_exception() ? "No value" : labels[value.index()]))
            {
                const auto choose = [&]<std::size_t Index>() noexcept
                {
                    using Alternative = std::variant_alternative_t<Index, T>;
                    if constexpr (std::is_nothrow_default_constructible_v<Alternative>)
                    {
                        ImGui::PushID(static_cast<int>(Index));
                        if (ImGui::Selectable(labels[Index], value.index() == Index) && value.index() != Index)
                        {
                            value.template emplace<Index>();
                            edited = true;
                        }
                        ImGui::PopID();
                    }
                };
                (choose.template operator()<I>(), ...);
                ImGui::EndCombo();
            }
            if (!value.valueless_by_exception())
            {
                edited |= std::visit(
                    [](auto& alternative) noexcept { return editValue(ValueEditOptions{}, alternative); },
                    value
                );
            }
            return edited;
        }
    } // namespace detail

    template <class T> bool editValue(const ValueEditOptions& options, T& value) noexcept
    {
        if constexpr (std::is_const_v<T>)
        {
            ImGui::TextDisabled("%s (constant)", options.label);
            return false;
        }
        else if constexpr (std::same_as<T, bool>)
        {
            return ImGui::Checkbox(options.label, &value);
        }
        else if constexpr (std::is_arithmetic_v<T>)
        {
            const T minimum = options.minimum ? static_cast<T>(*options.minimum) : T{};
            const T maximum = options.maximum ? static_cast<T>(*options.maximum) : T{};
            return ImGui::DragScalar(
                options.label,
                detail::numericWidgetType<T>(),
                &value,
                options.speed,
                options.minimum ? &minimum : nullptr,
                options.maximum ? &maximum : nullptr
            );
        }
        else if constexpr (requires { std::variant_size<T>::value; })
        {
            return detail::editVariant(options, value, std::make_index_sequence<std::variant_size_v<T>>{});
        }
        else if constexpr (requires {
                               typename T::value_type;
                               value.size();
                               value.data();
                           })
        {
            if constexpr (std::same_as<typename T::value_type, float>)
            {
                if (options.color && value.size() == 3)
                {
                    return ImGui::ColorEdit3(options.label, value.data());
                }
            }
            bool edited{};
            if (ImGui::TreeNode(options.label))
            {
                for (std::size_t index{}; index < value.size(); ++index)
                {
                    ImGui::PushID(static_cast<int>(index));
                    edited |= editValue(ValueEditOptions{}, value[index]);
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
            return edited;
        }
        else
        {
            ImGui::TextDisabled("%s (no value editor)", options.label);
            return false;
        }
    }

    template <> bool editValue(const ValueEditOptions&, asset::AssetId&) noexcept;
    template <> bool editValue(const ValueEditOptions&, Eigen::Vector2d&) noexcept;
    template <> bool editValue(const ValueEditOptions&, Eigen::Vector3d&) noexcept;
    template <> bool editValue(const ValueEditOptions&, Eigen::Quaterniond&) noexcept;
    template <> bool editValue(const ValueEditOptions&, Eigen::Quaternionf&) noexcept;
} // namespace lux::editor
