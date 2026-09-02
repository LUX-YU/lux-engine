#pragma once

#include <lux/engine/description/Visual.hpp>
#include <lux/engine/editor/inspector/GeneratedFieldSpec.hpp>
#include <lux/engine/editor/inspector/InspectorContext.hpp>
#include <lux/engine/resource/identity/AssetId.hpp>
#include <lux/engine/ui/DragDrop.hpp>
#include <lux/engine/ui/UiIds.hpp>

#include <Eigen/Geometry>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <string>
#include <string_view>
#include <type_traits>

namespace lux::editor::inspector
{
    inline constexpr ui::PayloadTypeIdView kAssetIdDragPayload{"lux.editor.asset-id"};

    namespace detail
    {
        inline void merge(ui::EditResult& target, ui::EditResult value) noexcept
        {
            target.changed |= value.changed;
            target.began |= value.began;
            target.committed |= value.committed;
            target.cancelled |= value.cancelled;
        }

        template<class Value>
        [[nodiscard]] ui::ScalarEditSpec<Value> scalarSpec(const GeneratedFieldSpec& spec)
        {
            ui::ScalarEditSpec<Value> result;
            switch (spec.widget)
            {
            case EGeneratedWidget::INPUT:
                result.mode = ui::EScalarEditMode::INPUT;
                break;
            case EGeneratedWidget::SLIDER:
                result.mode = ui::EScalarEditMode::SLIDER;
                break;
            default:
                result.mode = ui::EScalarEditMode::DRAG;
                break;
            }
            result.speed = static_cast<float>(spec.speed);
            if (spec.minimum)
                result.minimum = static_cast<Value>(*spec.minimum);
            if (spec.maximum)
                result.maximum = static_cast<Value>(*spec.maximum);
            if (spec.step)
                result.step = static_cast<Value>(*spec.step);
            return result;
        }

        [[nodiscard]] inline ui::EditResult editAssetId(
            InspectorContext& context,
            asset::AssetId& value
        )
        {
            auto path = context.editor.vfs().pathOf(value);
            const std::string label = value.isNull() ? std::string{"<none>"} :
                (path ? *path : std::string{"<unresolved asset>"});
            ui::EditResult result;
            if (context.frame.button(label))
            {
                // AssetBrowser supplies selection in Wave D. The C vertical
                // slice establishes display, clear and typed drag/drop.
            }
            auto drop = context.frame.dropTarget();
            if (drop.active())
            {
                const auto payload = drop.accept();
                if (payload && payload->type == kAssetIdDragPayload && payload->bytes.size() == 16U)
                {
                    std::array<std::uint8_t, 16> bytes{};
                    std::memcpy(bytes.data(), payload->bytes.data(), bytes.size());
                    value = asset::AssetId{bytes};
                    result = {true, true, true, false};
                }
            }
            if (!value.isNull() && context.frame.smallButton("Clear"))
            {
                value = asset::NullAssetId;
                result = {true, true, true, false};
            }
            return result;
        }
    } // namespace detail

    template<class Value>
    struct EditorValueBinding;

    template<>
    struct EditorValueBinding<bool>
    {
        [[nodiscard]] static ui::EditResult edit(
            InspectorContext& context,
            std::string_view label,
            bool& value,
            const GeneratedFieldSpec&
        )
        {
            return context.frame.checkbox(label, value);
        }
    };

#define LUX_EDITOR_SCALAR_BINDING(Type)                                                                               \
    template<>                                                                                                         \
    struct EditorValueBinding<Type>                                                                                   \
    {                                                                                                                  \
        [[nodiscard]] static ui::EditResult edit(                                                                      \
            InspectorContext& context,                                                                                 \
            std::string_view label,                                                                                    \
            Type& value,                                                                                               \
            const GeneratedFieldSpec& spec                                                                             \
        )                                                                                                              \
        {                                                                                                              \
            return context.frame.editScalar(label, value, detail::scalarSpec<Type>(spec));                            \
        }                                                                                                              \
    }

    LUX_EDITOR_SCALAR_BINDING(std::int32_t);
    LUX_EDITOR_SCALAR_BINDING(std::uint32_t);
    LUX_EDITOR_SCALAR_BINDING(std::int64_t);
    LUX_EDITOR_SCALAR_BINDING(std::uint64_t);
    LUX_EDITOR_SCALAR_BINDING(float);
    LUX_EDITOR_SCALAR_BINDING(double);

#undef LUX_EDITOR_SCALAR_BINDING

    template<>
    struct EditorValueBinding<std::string>
    {
        [[nodiscard]] static ui::EditResult edit(
            InspectorContext& context,
            std::string_view label,
            std::string& value,
            const GeneratedFieldSpec& spec
        )
        {
            return context.frame.inputText(label, value, ui::InputTextSpec{.read_only = spec.read_only});
        }
    };

    template<>
    struct EditorValueBinding<asset::AssetId>
    {
        [[nodiscard]] static ui::EditResult edit(
            InspectorContext& context,
            std::string_view,
            asset::AssetId& value,
            const GeneratedFieldSpec&
        )
        {
            return detail::editAssetId(context, value);
        }
    };

    template<class Scalar, int Rows, int Options, int MaxRows>
    struct EditorValueBinding<Eigen::Matrix<Scalar, Rows, 1, Options, MaxRows, 1>>
    {
        using Vector = Eigen::Matrix<Scalar, Rows, 1, Options, MaxRows, 1>;

        [[nodiscard]] static ui::EditResult edit(
            InspectorContext& context,
            std::string_view,
            Vector& value,
            const GeneratedFieldSpec& spec
        )
        {
            static constexpr std::array labels{"X", "Y", "Z", "W"};
            ui::EditResult result;
            for (int index = 0; index < Rows; ++index)
            {
                auto part = context.frame.editScalar(
                    labels[static_cast<std::size_t>(index)],
                    value[index],
                    detail::scalarSpec<Scalar>(spec)
                );
                detail::merge(result, part);
            }
            return result;
        }
    };

    template<class Scalar, int Options>
    struct EditorValueBinding<Eigen::Quaternion<Scalar, Options>>
    {
        using Quaternion = Eigen::Quaternion<Scalar, Options>;

        [[nodiscard]] static ui::EditResult edit(
            InspectorContext& context,
            std::string_view,
            Quaternion& value,
            const GeneratedFieldSpec& spec
        )
        {
            constexpr Scalar radians_to_degrees = Scalar{180} / std::numbers::pi_v<Scalar>;
            constexpr Scalar degrees_to_radians = std::numbers::pi_v<Scalar> / Scalar{180};
            auto degrees = (value.toRotationMatrix().eulerAngles(0, 1, 2) * radians_to_degrees).eval();
            ui::EditResult result;
            static constexpr std::array labels{"X", "Y", "Z"};
            for (int index = 0; index < 3; ++index)
            {
                auto part = context.frame.editScalar(
                    labels[static_cast<std::size_t>(index)],
                    degrees[index],
                    detail::scalarSpec<Scalar>(spec)
                );
                detail::merge(result, part);
            }
            if (result.changed)
            {
                const auto radians = degrees * degrees_to_radians;
                value = Quaternion{
                    Eigen::AngleAxis<Scalar>{radians[0], Eigen::Matrix<Scalar, 3, 1>::UnitX()} *
                    Eigen::AngleAxis<Scalar>{radians[1], Eigen::Matrix<Scalar, 3, 1>::UnitY()} *
                    Eigen::AngleAxis<Scalar>{radians[2], Eigen::Matrix<Scalar, 3, 1>::UnitZ()}
                }.normalized();
            }
            return result;
        }
    };

    template<>
    struct EditorValueBinding<rdesc::MeshVisualDescription>
    {
        [[nodiscard]] static ui::EditResult edit(
            InspectorContext& context,
            std::string_view,
            rdesc::MeshVisualDescription& value,
            const GeneratedFieldSpec& spec
        )
        {
            ui::EditResult result;
            detail::merge(result, EditorValueBinding<asset::AssetId>::edit(context, "Mesh", value.mesh, spec));
            detail::merge(result, EditorValueBinding<asset::AssetId>::edit(context, "Material", value.material, spec));
            detail::merge(result, context.frame.checkbox("Visible", value.visible));
            detail::merge(result, context.frame.checkbox("Cast shadow", value.cast_shadow));
            detail::merge(result, context.frame.checkbox("Receive shadow", value.receive_shadow));
            return result;
        }
    };

    template<>
    struct EditorValueBinding<rdesc::LightDescription>
    {
        [[nodiscard]] static ui::EditResult edit(
            InspectorContext& context,
            std::string_view,
            rdesc::LightDescription& value,
            const GeneratedFieldSpec& spec
        )
        {
            static constexpr std::array options{
                ui::ComboOption{0, "Directional"},
                ui::ComboOption{1, "Point"},
                ui::ComboOption{2, "Spot"},
                ui::ComboOption{3, "Area"},
            };
            ui::EditResult result;
            auto type = static_cast<std::int64_t>(value.type);
            auto type_edit = context.frame.editChoice("Type", type, options);
            if (type_edit.changed)
                value.type = static_cast<rdesc::ELightType>(type);
            detail::merge(result, type_edit);
            for (std::size_t index = 0; index < value.color.size(); ++index)
            {
                static constexpr std::array labels{"R", "G", "B"};
                detail::merge(
                    result,
                    context.frame.editScalar(labels[index], value.color[index], detail::scalarSpec<float>(spec))
                );
            }
            detail::merge(
                result,
                context.frame.editScalar("Intensity", value.intensity, detail::scalarSpec<float>(spec))
            );
            detail::merge(result, context.frame.editScalar("Range", value.range, detail::scalarSpec<float>(spec)));
            detail::merge(result, context.frame.checkbox("Cast shadow", value.cast_shadow));
            return result;
        }
    };
} // namespace lux::editor::inspector
