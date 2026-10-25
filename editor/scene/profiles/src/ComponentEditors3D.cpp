#include <array>
#include <imgui.h>
#include <lux/engine/editor/ComponentEditors3D.hpp>
#include <lux/engine/editor/EditorComposition.hpp>
#include <lux/engine/scene/Camera.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>

namespace lux::editor
{
    namespace
    {
        template <class T> void fields(T&, float) noexcept;

        void assetField(const char* label, asset::AssetId& value) noexcept
        {
            std::array<char, 64> text{};
            constexpr char hex[] = "0123456789abcdef";
            std::size_t cursor = 0, index = 0;
            for (auto byte : value.bytes())
            {
                if (index == 4 || index == 6 || index == 8 || index == 10)
                {
                    text[cursor++] = '-';
                }
                const auto digit = std::to_integer<unsigned>(byte);
                text[cursor++] = hex[digit >> 4];
                text[cursor++] = hex[digit & 15];
                ++index;
            }
            // Commit only a complete identity; a partial typed UUID never replaces the draft.
            if (ImGui::InputText(label, text.data(), text.size(), ImGuiInputTextFlags_EnterReturnsTrue))
            {
                if (auto parsed = uuids::uuid::from_string(text.data()))
                {
                    value = asset::AssetId{*parsed};
                }
            }
        }

        template <class T> void field(const char* label, T& value, float speed) noexcept
        {
            if constexpr (std::same_as<T, bool>)
            {
                ImGui::Checkbox(label, &value);
            }
            else if constexpr (std::same_as<T, double>)
            {
                ImGui::DragScalar(label, ImGuiDataType_Double, &value, speed);
            }
            else if constexpr (std::same_as<T, float>)
            {
                ImGui::DragFloat(label, &value, speed);
            }
            else if constexpr (std::same_as<T, Eigen::Vector3d>)
            {
                ImGui::DragScalarN(label, ImGuiDataType_Double, value.data(), 3, speed);
            }
            else if constexpr (std::same_as<T, Eigen::Quaterniond>)
            {
                Eigen::Vector3d angles = value.toRotationMatrix().eulerAngles(0, 1, 2);
                if (ImGui::DragScalarN(label, ImGuiDataType_Double, angles.data(), 3, speed))
                {
                    value = Eigen::AngleAxisd{angles.x(), Eigen::Vector3d::UnitX()} *
                            Eigen::AngleAxisd{angles.y(), Eigen::Vector3d::UnitY()} *
                            Eigen::AngleAxisd{angles.z(), Eigen::Vector3d::UnitZ()};
                }
            }
            else if constexpr (std::same_as<T, rdesc::MeshVisualDescription>)
            {
                assetField("Mesh asset", value.mesh);
                assetField("Material asset", value.material);
                ImGui::Checkbox("Visible", &value.visible);
                ImGui::Checkbox("Cast shadow", &value.cast_shadow);
                ImGui::Checkbox("Receive shadow", &value.receive_shadow);
            }
            else if constexpr (std::same_as<T, rdesc::LightDescription>)
            {
                int kind = static_cast<int>(value.type);
                if (ImGui::Combo("Type", &kind, "Directional\0Point\0Spot\0Area\0"))
                {
                    value.type = static_cast<rdesc::ELightType>(kind);
                }
                ImGui::ColorEdit3("Color", value.color.data());
                ImGui::DragFloat("Intensity", &value.intensity, 0.1F, 0, 1.0e6F);
                ImGui::DragFloat("Range", &value.range, 0.1F, 0.001F, 1.0e6F);
                ImGui::DragFloat("Inner cone (radians)", &value.inner_cone_angle, 0.01F, 0, 3.14F);
                ImGui::DragFloat("Outer cone (radians)", &value.outer_cone_angle, 0.01F, 0, 3.14F);
                ImGui::Checkbox("Cast shadow", &value.cast_shadow);
                ImGui::InputScalar("Shadow resolution", ImGuiDataType_U32, &value.shadow_map_size);
                ImGui::DragFloat("Shadow bias", &value.shadow_bias, 0.0001F);
            }
            else if constexpr (requires { std::variant_size<T>::value; })
            {
                int selected = static_cast<int>(value.index());
                if (ImGui::Combo(label, &selected, "Perspective\0Orthographic\0"))
                {
                    if (selected == 0)
                    {
                        value = scene::PerspectiveProjection{};
                    }
                    else
                    {
                        value = scene::OrthographicProjection{};
                    }
                }
                std::visit([&](auto& projection) noexcept { fields(projection, speed); }, value);
            }
            else if constexpr (meta::HasTypeStaticInfo<T>)
            {
                fields(value, speed);
            }
            else
            {
                ImGui::Text("%s (read-only type)", label);
            }
        }

        template <class T> void fields(T& value, float) noexcept
        {
            std::apply(
                [&](const auto&... metadata) noexcept
                {
                    const auto show = [&](const auto& member) noexcept
                    {
                        ImGui::PushID(member.name.data());
                        ImGui::BeginDisabled(member.read_only);
                        field(member.display_name.data(), value.*member.pointer, static_cast<float>(member.edit_speed));
                        ImGui::EndDisabled();
                        ImGui::PopID();
                    };
                    (show(metadata), ...);
                },
                meta::TTypeStaticInfo<T>::fields
            );
        }
    } // namespace

    FrameworkResult<void> registerComponentEditors3D(EditorComposition& composition) noexcept
    {
        const ComponentEditor editors[]{
            {{},
             cxx::typeToken<simulation::ecs::Transform3D>(),
             +[](void* value) noexcept { fields(*static_cast<simulation::ecs::Transform3D*>(value), 0.01F); }},
            {{},
             cxx::typeToken<scene::Camera>(),
             +[](void* value) noexcept { fields(*static_cast<scene::Camera*>(value), 0.01F); }},
            {{},
             cxx::typeToken<simulation::ecs::Mesh3D>(),
             +[](void* value) noexcept { fields(*static_cast<simulation::ecs::Mesh3D*>(value), 0.01F); }},
            {{},
             cxx::typeToken<simulation::ecs::Light3D>(),
             +[](void* value) noexcept { fields(*static_cast<simulation::ecs::Light3D*>(value), 0.01F); }}
        };
        for (const auto& editor : editors)
        {
            if (auto added = composition.registerComponentEditor(editor); !added)
            {
                return added;
            }
        }
        return {};
    }
} // namespace lux::editor
