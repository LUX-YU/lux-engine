#pragma once
#include <lux/engine/ui/Frame.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <numbers>
namespace lux::editor::ui::detail
{
    inline void merge(lux::ui::EditResult &result, lux::ui::EditResult next) noexcept
    {
        result.changed |= next.changed;
        result.began |= next.began;
        result.committed |= next.committed;
        result.cancelled |= next.cancelled;
    }
    inline lux::ui::EditResult editVector(lux::ui::Frame &frame, std::string_view label,
                                          Eigen::Vector3d &value, float speed)
    {
        frame.propertyRow(label);
        auto id = frame.id(lux::ui::WidgetIdView{label});
        lux::ui::EditResult result;
        constexpr std::array labels{"X", "Y", "Z"};
        lux::ui::ScalarEditSpec<double> spec;
        spec.speed = speed;
        for (int i = 0; i < 3; ++i)
            merge(result, frame.editScalar(labels[i], value[i], spec));
        return result;
    }
    inline lux::ui::EditResult editTransform(lux::ui::Frame &frame, lux::simulation::ecs::Transform3D &value)
    {
        auto result = editVector(frame, "Translation", value.translation, 0.05F);
        auto degrees = (value.rotation.toRotationMatrix().eulerAngles(0, 1, 2) *
                        (180.0 / std::numbers::pi)).eval();
        const auto rotation = editVector(frame, "Rotation (degrees)", degrees, 0.25F);
        if (rotation.changed)
        {
            const auto radians = (degrees * (std::numbers::pi / 180.0)).eval();
            value.rotation = Eigen::Quaterniond{Eigen::AngleAxisd{radians[0], Eigen::Vector3d::UnitX()} *
                Eigen::AngleAxisd{radians[1], Eigen::Vector3d::UnitY()} *
                Eigen::AngleAxisd{radians[2], Eigen::Vector3d::UnitZ()}}.normalized();
        }
        merge(result, rotation);
        merge(result, editVector(frame, "Scale", value.scale, 0.01F));
        return result;
    }
    inline lux::ui::EditResult editLight(lux::ui::Frame &frame, lux::simulation::ecs::Light3D &light)
    {
        auto &value = light.value;
        static constexpr std::array options{
            lux::ui::ComboOption{0, "Directional"}, lux::ui::ComboOption{1, "Point"},
            lux::ui::ComboOption{2, "Spot"}, lux::ui::ComboOption{3, "Area"}
        };
        frame.propertyRow("Type");
        auto type = static_cast<std::int64_t>(value.type);
        auto result = frame.editChoice("##type", type, options);
        if (result.changed)
            value.type = static_cast<lux::rdesc::ELightType>(type);
        const auto scalar = [&](const char *label, float &number) {
            frame.propertyRow(label);
            lux::ui::ScalarEditSpec<float> spec;
            spec.speed = 0.01F;
            merge(result, frame.editScalar(label, number, spec));
        };
        scalar("Red", value.color[0]);
        scalar("Green", value.color[1]);
        scalar("Blue", value.color[2]);
        scalar("Intensity", value.intensity);
        scalar("Range", value.range);
        frame.propertyRow("Cast shadow");
        merge(result, frame.checkbox("##cast-shadow", value.cast_shadow));
        return result;
    }
}
