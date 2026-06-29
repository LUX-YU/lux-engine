#pragma once

#include <imgui.h>
#include <imgui_stdlib.h>
#include <lux/engine/meta/Meta.hpp>
#include <string>
#include <string_view>

// Editor-tier helper (was a lux::ui squatter; renamespaced in the GraphKit P2
// sweep — namespace follows the tier).
namespace lux::editor
{
    // 用于编辑不同类型常量的特性系统
    template<typename T>
    struct ConstantTypeTraits {
        // 默认实现：显示不支持编辑
        static bool editValue(const char* label, lux::meta::RuntimeObject& constant, const lux::meta::RefType* type) {
            ImGui::Text("(%s 不支持编辑)", label);
            return false;
        }
    };

    // 布尔类型特化
    template<>
    struct ConstantTypeTraits<bool> {
        static bool editValue(const char* label, lux::meta::RuntimeObject& constant, const lux::meta::RefType* type) {
            bool* value_ptr = static_cast<bool*>(constant.data());
            if (value_ptr && ImGui::Checkbox(label, value_ptr)) {
                return true; // 值已被修改
            }
            return false;
        }
    };

    // 8位有符号整数特化
    template<>
    struct ConstantTypeTraits<int8_t> {
        static bool editValue(const char* label, lux::meta::RuntimeObject& constant, const lux::meta::RefType* type) {
            int8_t* value_ptr = static_cast<int8_t*>(constant.data());
            if (value_ptr && ImGui::InputScalar(label, ImGuiDataType_S8, value_ptr)) {
                return true;
            }
            return false;
        }
    };

    // 16位有符号整数特化
    template<>
    struct ConstantTypeTraits<int16_t> {
        static bool editValue(const char* label, lux::meta::RuntimeObject& constant, const lux::meta::RefType* type) {
            int16_t* value_ptr = static_cast<int16_t*>(constant.data());
            if (value_ptr && ImGui::InputScalar(label, ImGuiDataType_S16, value_ptr)) {
                return true;
            }
            return false;
        }
    };

    // 32位有符号整数特化
    template<>
    struct ConstantTypeTraits<int32_t> {
        static bool editValue(const char* label, lux::meta::RuntimeObject& constant, const lux::meta::RefType* type) {
            int32_t* value_ptr = static_cast<int32_t*>(constant.data());
            if (value_ptr && ImGui::InputScalar(label, ImGuiDataType_S32, value_ptr)) {
                return true;
            }
            return false;
        }
    };

    // 64位有符号整数特化
    template<>
    struct ConstantTypeTraits<int64_t> {
        static bool editValue(const char* label, lux::meta::RuntimeObject& constant, const lux::meta::RefType* type) {
            int64_t* value_ptr = static_cast<int64_t*>(constant.data());
            if (value_ptr && ImGui::InputScalar(label, ImGuiDataType_S64, value_ptr)) {
                return true;
            }
            return false;
        }
    };

    // 8位无符号整数特化
    template<>
    struct ConstantTypeTraits<uint8_t> {
        static bool editValue(const char* label, lux::meta::RuntimeObject& constant, const lux::meta::RefType* type) {
            uint8_t* value_ptr = static_cast<uint8_t*>(constant.data());
            if (value_ptr && ImGui::InputScalar(label, ImGuiDataType_U8, value_ptr)) {
                return true;
            }
            return false;
        }
    };

    // 16位无符号整数特化
    template<>
    struct ConstantTypeTraits<uint16_t> {
        static bool editValue(const char* label, lux::meta::RuntimeObject& constant, const lux::meta::RefType* type) {
            uint16_t* value_ptr = static_cast<uint16_t*>(constant.data());
            if (value_ptr && ImGui::InputScalar(label, ImGuiDataType_U16, value_ptr)) {
                return true;
            }
            return false;
        }
    };

    // 32位无符号整数特化
    template<>
    struct ConstantTypeTraits<uint32_t> {
        static bool editValue(const char* label, lux::meta::RuntimeObject& constant, const lux::meta::RefType* type) {
            uint32_t* value_ptr = static_cast<uint32_t*>(constant.data());
            if (value_ptr && ImGui::InputScalar(label, ImGuiDataType_U32, value_ptr)) {
                return true;
            }
            return false;
        }
    };

    // 64位无符号整数特化
    template<>
    struct ConstantTypeTraits<uint64_t> {
        static bool editValue(const char* label, lux::meta::RuntimeObject& constant, const lux::meta::RefType* type) {
            uint64_t* value_ptr = static_cast<uint64_t*>(constant.data());
            if (value_ptr && ImGui::InputScalar(label, ImGuiDataType_U64, value_ptr)) {
                return true;
            }
            return false;
        }
    };

    // 浮点数特化
    template<>
    struct ConstantTypeTraits<float> {
        static bool editValue(const char* label, lux::meta::RuntimeObject& constant, const lux::meta::RefType* type) {
            float* value_ptr = static_cast<float*>(constant.data());
            if (value_ptr && ImGui::InputFloat(label, value_ptr)) {
                return true;
            }
            return false;
        }
    };

    // 双精度浮点数特化
    template<>
    struct ConstantTypeTraits<double> {
        static bool editValue(const char* label, lux::meta::RuntimeObject& constant, const lux::meta::RefType* type) {
            double* value_ptr = static_cast<double*>(constant.data());
            if (value_ptr && ImGui::InputDouble(label, value_ptr)) {
                return true;
            }
            return false;
        }
    };

    // 字符串特化 (std::string_view)
    template<>
    struct ConstantTypeTraits<std::string_view> {
        static bool editValue(const char* label, lux::meta::RuntimeObject& constant, const lux::meta::RefType* type) {
            // 由于std::string_view是只读的，我们只能显示它
            const std::string_view* value_ptr = static_cast<const std::string_view*>(constant.data());
            if (value_ptr) {
                ImGui::LabelText(label, "%.*s", static_cast<int>(value_ptr->size()), value_ptr->data());
            }
            return false; // 字符串视图无法修改
        }
    };

    // 字符串特化 (std::string)
    template<>
    struct ConstantTypeTraits<std::string> {
        static bool editValue(const char* label, lux::meta::RuntimeObject& constant, const lux::meta::RefType* type) {
            // 注意：这里假设我们可以转换和访问std::string
            // 如果Constant实际上不存储std::string而是std::string_view，这需要特殊处理
            std::string* value_ptr = static_cast<std::string*>(constant.data());
            if (value_ptr && ImGui::InputText(label, value_ptr)) {
                return true;
            }
            return false;
        }
    };

    // 基于类型调度常量编辑器
    class ConstantEditor {
    public:
        static bool editConstant(const char* label, lux::meta::RuntimeObject& constant) {
            if (!constant.isValid()) {
                ImGui::Text("(无效常量)");
                return false;
            }

            const lux::meta::RefType* type = constant.type();
            if (!type) {
                ImGui::Text("(无类型信息)");
                return false;
            }

            // 根据类型分发到相应的编辑器
            using namespace lux::meta;
            auto baseType = static_cast<EBaseType>(type->qtype.base);
            
            // 处理基本类型
            switch (baseType) {
                case EBaseType::Bool:
                    return ConstantTypeTraits<bool>::editValue(label, constant, type);
                case EBaseType::Int8:
                    return ConstantTypeTraits<int8_t>::editValue(label, constant, type);
                case EBaseType::Int16:
                    return ConstantTypeTraits<int16_t>::editValue(label, constant, type);
                case EBaseType::Int32:
                    return ConstantTypeTraits<int32_t>::editValue(label, constant, type);
                case EBaseType::Int64:
                    return ConstantTypeTraits<int64_t>::editValue(label, constant, type);
                case EBaseType::Uint8:
                    return ConstantTypeTraits<uint8_t>::editValue(label, constant, type);
                case EBaseType::Uint16:
                    return ConstantTypeTraits<uint16_t>::editValue(label, constant, type);
                case EBaseType::Uint32:
                    return ConstantTypeTraits<uint32_t>::editValue(label, constant, type);
                case EBaseType::Uint64:
                    return ConstantTypeTraits<uint64_t>::editValue(label, constant, type);
                case EBaseType::Float:
                    return ConstantTypeTraits<float>::editValue(label, constant, type);
                case EBaseType::Double:
                    return ConstantTypeTraits<double>::editValue(label, constant, type);
                case EBaseType::Record:
                    {
                        if(type->hash == lux::cxx::type_hash<std::string_view>())
                        {
                            return ConstantTypeTraits<std::string_view>::editValue(label, constant, type);
                        }
                        else if(type->hash == lux::cxx::type_hash<std::string>())
                        {
                            return ConstantTypeTraits<std::string>::editValue(label, constant, type);
                        }
                        else
                        {
                            ImGui::Text("(不支持类型 %s)", type->name.data());
                            return false;
                        }
                    }
                default:
                    ImGui::Text("WTF");
                    return false;
            }
			ImGui::EndGroup();
        }
    };

} // namespace lux::editor