#pragma once

#include <lux/engine/function/script/ScriptSemantic.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace lux::rdesc
{
    enum class EScriptModel : std::uint8_t
    {
        GLOBAL_MODULE,
        ENTITY_BEHAVIOR,
    };

    struct ScriptValueType final
    {
        std::string canonical_name;
        std::uint64_t type_id{};
        lux::script::EScriptPassMode pass{
            lux::script::EScriptPassMode::VALUE};

        friend bool operator==(const ScriptValueType&, const ScriptValueType&)
            noexcept = default;
    };

    struct ScriptFunction final
    {
        std::string name;
        lux::script::ScriptSymbolId symbol_id{
            lux::script::InvalidScriptSymbolId};
        std::vector<ScriptValueType> args;
        std::vector<ScriptValueType> returns;

        friend bool operator==(const ScriptFunction&, const ScriptFunction&)
            noexcept = default;
    };

    struct ScriptDependency final
    {
        std::string kind;
        std::string id;

        friend bool operator==(const ScriptDependency&, const ScriptDependency&)
            noexcept = default;
    };

    struct ScriptProvenance final
    {
        std::string compiler_id;
        std::string compiler_version;
        std::string source_id;
        std::string source_hash;
        std::string built_at;

        friend bool operator==(const ScriptProvenance&, const ScriptProvenance&)
            noexcept = default;
    };

    struct LuaSourceScript final
    {
        std::string entry;

        friend bool operator==(const LuaSourceScript&, const LuaSourceScript&)
            noexcept = default;
    };

    struct NativeModuleScript final
    {
        std::uint32_t abi_version{};
        std::uint64_t state_layout_hash{};
        std::uint32_t state_size{};
        std::uint32_t state_align{1U};
        std::vector<std::byte> state_defaults;

        friend bool operator==(
            const NativeModuleScript&,
            const NativeModuleScript&
        ) noexcept = default;
    };

    struct CppStaticScript final
    {
        std::string descriptor;

        friend bool operator==(
            const CppStaticScript&,
            const CppStaticScript&
        ) noexcept = default;
    };

    struct PythonSourceScript final
    {
        std::string entry;

        friend bool operator==(
            const PythonSourceScript&,
            const PythonSourceScript&
        ) noexcept = default;
    };

    class Script final
    {
      public:
        static constexpr std::uint32_t kSchemaVersion = 4U;

        enum class Kind : std::uint8_t
        {
            UNKNOWN = 0,
            LUA_SOURCE = 1,
            PYTHON_SOURCE = 2,
            NATIVE_MODULE = 3,
            CPP_STATIC = 6,
        };

        using Body = std::variant<
            std::monostate,
            LuaSourceScript,
            PythonSourceScript,
            NativeModuleScript,
            CppStaticScript>;

        std::uint32_t schema_version{kSchemaVersion};
        std::string module_name;
        EScriptModel model{EScriptModel::GLOBAL_MODULE};
        std::vector<ScriptFunction> exports;
        std::vector<ScriptDependency> dependencies;
        ScriptProvenance provenance;
        Body body;

        [[nodiscard]] Kind kind() const noexcept
        {
            if (std::holds_alternative<LuaSourceScript>(body))
                return Kind::LUA_SOURCE;
            if (std::holds_alternative<PythonSourceScript>(body))
                return Kind::PYTHON_SOURCE;
            if (std::holds_alternative<NativeModuleScript>(body))
                return Kind::NATIVE_MODULE;
            if (std::holds_alternative<CppStaticScript>(body))
                return Kind::CPP_STATIC;
            return Kind::UNKNOWN;
        }
    };

    [[nodiscard]] inline bool validScriptDescription(
        const Script& description
    ) noexcept
    {
        if (description.schema_version != Script::kSchemaVersion ||
            description.module_name.empty() ||
            (description.model != EScriptModel::GLOBAL_MODULE &&
             description.model != EScriptModel::ENTITY_BEHAVIOR) ||
            description.kind() == Script::Kind::UNKNOWN)
        {
            return false;
        }

        const auto valid_type = [](const ScriptValueType& type) noexcept
        {
            return type.type_id != 0U && !type.canonical_name.empty() &&
                type.type_id == lux::script::scriptSemanticTypeId(
                    type.canonical_name
                );
        };
        for (std::size_t index{}; index < description.exports.size(); ++index)
        {
            const auto& function = description.exports[index];
            if (function.name.empty() ||
                function.symbol_id == lux::script::InvalidScriptSymbolId)
            {
                return false;
            }
            for (std::size_t previous{}; previous < index; ++previous)
            {
                if (description.exports[previous].symbol_id == function.symbol_id)
                {
                    return false;
                }
            }
            for (const auto& argument : function.args)
            {
                if (!valid_type(argument))
                    return false;
            }
            for (const auto& result : function.returns)
            {
                if (!valid_type(result) ||
                    result.pass != lux::script::EScriptPassMode::VALUE)
                {
                    return false;
                }
            }
        }

        if (const auto* native = std::get_if<NativeModuleScript>(
                &description.body
            ))
        {
            if (native->abi_version == 0U ||
                native->state_align == 0U ||
                (native->state_align & (native->state_align - 1U)) != 0U ||
                native->state_defaults.size() > native->state_size)
            {
                return false;
            }
        }
        if (const auto* cpp_static = std::get_if<CppStaticScript>(
                &description.body
            ); cpp_static != nullptr && cpp_static->descriptor.empty())
        {
            return false;
        }
        if (const auto* lua = std::get_if<LuaSourceScript>(&description.body);
            lua != nullptr && lua->entry.empty())
        {
            return false;
        }
        if (const auto* python = std::get_if<PythonSourceScript>(
                &description.body
            ); python != nullptr && python->entry.empty())
        {
            return false;
        }
        return true;
    }
}
