#include <lux/engine/editor/ValueEditing.hpp>

namespace lux::editor
{
    template <> bool editValue(const ValueEditOptions& options, asset::AssetId& value) noexcept
    {
        std::array<char, 64> text{};
        constexpr char hex[] = "0123456789abcdef";
        std::size_t cursor{}, index{};
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
        if (ImGui::InputText(options.label, text.data(), text.size(), ImGuiInputTextFlags_EnterReturnsTrue))
        {
            if (auto parsed = uuids::uuid::from_string(text.data()))
            {
                value = asset::AssetId{*parsed};
                return true;
            }
        }
        return false;
    }

    template <> bool editValue(const ValueEditOptions& options, Eigen::Vector2d& value) noexcept
    {
        return ImGui::DragScalarN(options.label, ImGuiDataType_Double, value.data(), 2, options.speed);
    }

    template <> bool editValue(const ValueEditOptions& options, Eigen::Vector3d& value) noexcept
    {
        return ImGui::DragScalarN(options.label, ImGuiDataType_Double, value.data(), 3, options.speed);
    }

    template <> bool editValue(const ValueEditOptions& options, Eigen::Quaterniond& value) noexcept
    {
        Eigen::Vector3d angles = value.toRotationMatrix().eulerAngles(0, 1, 2);
        if (!ImGui::DragScalarN(options.label, ImGuiDataType_Double, angles.data(), 3, options.speed))
        {
            return false;
        }
        value = Eigen::AngleAxisd{angles.x(), Eigen::Vector3d::UnitX()} *
                Eigen::AngleAxisd{angles.y(), Eigen::Vector3d::UnitY()} *
                Eigen::AngleAxisd{angles.z(), Eigen::Vector3d::UnitZ()};
        return true;
    }

    template <> bool editValue(const ValueEditOptions& options, Eigen::Quaternionf& value) noexcept
    {
        Eigen::Quaterniond draft = value.cast<double>();
        if (!editValue(options, draft))
        {
            return false;
        }
        value = draft.cast<float>();
        return true;
    }
} // namespace lux::editor
