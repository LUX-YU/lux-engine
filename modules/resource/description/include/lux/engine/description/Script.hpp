#pragma once

#include <lux/engine/core/semantic/SemanticType.hpp>
#include <lux/engine/function/script/ScriptSymbol.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace lux::rdesc
{
    struct ScriptValueType final
    {
        std::string canonical_name;
        std::uint64_t type_id{};
        lux::semantic::EValuePass pass{lux::semantic::EValuePass::VALUE};
        std::uint8_t abi_kind{};
        std::uint32_t size{};
        std::uint32_t alignment{};

        friend bool operator==(const ScriptValueType&, const ScriptValueType&)
            noexcept = default;
    };

    template <class Value>
        requires lux::semantic::TypeDeclared<Value>
    [[nodiscard]] inline ScriptValueType makeScriptValueType(
        lux::semantic::EValuePass pass = lux::semantic::EValuePass::VALUE
    )
    {
        using Traits = lux::semantic::TypeTraits<std::remove_cv_t<Value>>;
        return {
            std::string(Traits::CanonicalName),
            lux::semantic::typeId(Traits::CanonicalName),
            pass,
            Traits::AbiKind,
            Traits::Size,
            Traits::Alignment};
    }

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

    class Script final
    {
      public:
        static constexpr std::uint32_t kSchemaVersion = 5U;

        enum class Kind : std::uint8_t
        {
            UNKNOWN = 0,
            LUA_SOURCE = 1,
            NATIVE_MODULE = 3,
            CPP_STATIC = 6,
        };

        using Body = std::variant<
            std::monostate,
            LuaSourceScript,
            NativeModuleScript,
            CppStaticScript>;

        std::uint32_t schema_version{kSchemaVersion};
        std::string module_name;
        std::vector<ScriptFunction> exports;
        std::vector<ScriptDependency> dependencies;
        ScriptProvenance provenance;
        Body body;

        [[nodiscard]] Kind kind() const noexcept
        {
            if (std::holds_alternative<LuaSourceScript>(body))
                return Kind::LUA_SOURCE;
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
            description.kind() == Script::Kind::UNKNOWN)
        {
            return false;
        }

        const auto valid_type = [](const ScriptValueType& type) noexcept
        {
            if (type.type_id == 0U || type.canonical_name.empty() ||
                type.type_id != lux::semantic::typeId(
                    type.canonical_name) ||
                type.pass > lux::semantic::EValuePass::CONST_REF ||
                type.abi_kind == 0U || type.size == 0U ||
                type.alignment == 0U ||
                (type.alignment & (type.alignment - 1U)) != 0U)
            {
                return false;
            }
            const auto* builtin = lux::semantic::builtinLayout(type.type_id);
            return builtin == nullptr ||
                (builtin->canonical_name == type.canonical_name &&
                 builtin->abi_kind == type.abi_kind &&
                 builtin->size == type.size &&
                 builtin->alignment == type.alignment);
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
                    result.pass != lux::semantic::EValuePass::VALUE)
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
        return true;
    }
}
