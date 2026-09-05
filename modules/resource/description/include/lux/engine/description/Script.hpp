#pragma once

#include <lux/engine/core/semantic/SemanticType.hpp>
#include <lux/engine/function/script/ScriptApi.hpp>
#include <lux/engine/function/script/ScriptEvent.hpp>
#include <lux/engine/function/script/ScriptSymbol.hpp>

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <new>
#include <string>
#include <type_traits>
#include <unordered_set>
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

    struct ScriptApiRequirement final
    {
        lux::script::ScriptApiContractId contract;
        std::uint64_t expected_schema_hash{};

        friend bool operator==(const ScriptApiRequirement&, const ScriptApiRequirement&) noexcept = default;
    };

    struct ScriptLifecycleRoles final
    {
        lux::script::ScriptSymbolId begin_play{lux::script::InvalidScriptSymbolId};
        lux::script::ScriptSymbolId end_play{lux::script::InvalidScriptSymbolId};

        friend bool operator==(const ScriptLifecycleRoles&, const ScriptLifecycleRoles&) noexcept = default;
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
        std::vector<lux::script::ScriptSymbolId> suspension_capable_exports;

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
        std::vector<lux::script::ScriptSymbolId> suspension_capable_exports;

        friend bool operator==(
            const CppStaticScript&,
            const CppStaticScript&
        ) noexcept = default;
    };

    class Script final
    {
      public:
        static constexpr std::uint32_t kSchemaVersion = 12U;

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
        ScriptLifecycleRoles lifecycle;
        std::vector<ScriptDependency> dependencies;
        std::vector<ScriptApiRequirement> api_requirements;
        std::vector<lux::script::ScriptEventSourceDescription> event_requirements;
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

    namespace detail
    {
        [[nodiscard]] inline bool validScriptValueType(const ScriptValueType& type) noexcept
        {
            if (type.type_id == 0U || type.canonical_name.empty() ||
                type.type_id != lux::semantic::typeId(type.canonical_name) ||
                type.pass > lux::semantic::EValuePass::CONST_REF || type.abi_kind == 0U || type.size == 0U ||
                type.alignment == 0U || (type.alignment & (type.alignment - 1U)) != 0U)
            {
                return false;
            }
            const auto* builtin = lux::semantic::builtinLayout(type.type_id);
            return builtin == nullptr ||
                (builtin->canonical_name == type.canonical_name && builtin->abi_kind == type.abi_kind &&
                 builtin->size == type.size && builtin->alignment == type.alignment);
        }

        [[nodiscard]] inline bool validScriptFunction(const ScriptFunction& function) noexcept
        {
            if (function.name.empty() || function.symbol_id == lux::script::InvalidScriptSymbolId)
            {
                return false;
            }
            for (const auto& argument : function.args)
            {
                if (!validScriptValueType(argument))
                    return false;
            }
            for (const auto& result : function.returns)
            {
                if (!validScriptValueType(result) || result.pass != lux::semantic::EValuePass::VALUE)
                    return false;
            }
            return true;
        }

        [[nodiscard]] inline bool validScriptBody(const Script& description) noexcept
        {
            if (description.schema_version != Script::kSchemaVersion || description.module_name.empty() ||
                description.kind() == Script::Kind::UNKNOWN)
            {
                return false;
            }
            if (const auto* native = std::get_if<NativeModuleScript>(&description.body))
            {
                if (native->abi_version == 0U || native->state_align == 0U ||
                    (native->state_align & (native->state_align - 1U)) != 0U ||
                    native->state_defaults.size() > native->state_size)
                {
                    return false;
                }
            }
            if (const auto* cpp_static = std::get_if<CppStaticScript>(&description.body);
                cpp_static != nullptr && (cpp_static->descriptor.empty() ||
                    cpp_static->suspension_capable_exports.size() > std::numeric_limits<std::uint32_t>::max() ||
                    !std::ranges::is_sorted(cpp_static->suspension_capable_exports) ||
                    std::ranges::adjacent_find(cpp_static->suspension_capable_exports) !=
                        cpp_static->suspension_capable_exports.end()))
            {
                return false;
            }
            if (const auto* lua = std::get_if<LuaSourceScript>(&description.body);
                lua != nullptr && (lua->entry.empty() ||
                    lua->suspension_capable_exports.size() > std::numeric_limits<std::uint32_t>::max() ||
                    !std::ranges::is_sorted(lua->suspension_capable_exports) ||
                    std::ranges::adjacent_find(lua->suspension_capable_exports) !=
                        lua->suspension_capable_exports.end()))
            {
                return false;
            }
            return true;
        }
    }

    [[nodiscard]] inline bool validScriptDescription(const Script& description) noexcept
    {
        if (!detail::validScriptBody(description))
            return false;

        try
        {
            std::unordered_set<lux::script::ScriptSymbolId> symbols;
            symbols.reserve(description.exports.size());
            for (const auto& function : description.exports)
            {
                if (!detail::validScriptFunction(function) || !symbols.insert(function.symbol_id).second)
                    return false;
            }
            const bool has_begin = description.lifecycle.begin_play != lux::script::InvalidScriptSymbolId;
            const bool has_end = description.lifecycle.end_play != lux::script::InvalidScriptSymbolId;
            const bool is_duplicate_role = has_begin && has_end &&
                description.lifecycle.begin_play == description.lifecycle.end_play;
            const bool is_missing_begin = has_begin && !symbols.contains(description.lifecycle.begin_play);
            const bool is_missing_end = has_end && !symbols.contains(description.lifecycle.end_play);
            if (is_duplicate_role || is_missing_begin || is_missing_end)
                return false;
            if (const auto* lua = std::get_if<LuaSourceScript>(&description.body))
            {
                for (const auto symbol : lua->suspension_capable_exports)
                {
                    const bool is_missing_export = !symbols.contains(symbol);
                    const bool is_lifecycle = symbol == description.lifecycle.begin_play ||
                        symbol == description.lifecycle.end_play;
                    if (is_missing_export || is_lifecycle)
                        return false;
                }
            }
            if (const auto* cpp_static = std::get_if<CppStaticScript>(&description.body))
            {
                for (const auto symbol : cpp_static->suspension_capable_exports)
                {
                    const bool is_missing_export = !symbols.contains(symbol);
                    const bool is_lifecycle = symbol == description.lifecycle.begin_play ||
                        symbol == description.lifecycle.end_play;
                    if (is_missing_export || is_lifecycle)
                        return false;
                }
            }
            std::unordered_set<std::uint64_t> contracts;
            contracts.reserve(description.api_requirements.size());
            for (const auto& requirement : description.api_requirements)
            {
                if (!requirement.contract.isValid() || requirement.expected_schema_hash == 0U ||
                    !contracts.insert(requirement.contract.hash()).second)
                {
                    return false;
                }
            }
            if (description.event_requirements.size() > std::numeric_limits<std::uint32_t>::max() ||
                !std::ranges::is_sorted(
                    description.event_requirements,
                    lux::script::ScriptEventSourceLess{}
                ))
            {
                return false;
            }
            for (std::size_t index{}; index < description.event_requirements.size(); ++index)
            {
                const auto& requirement = description.event_requirements[index];
                if (!requirement.valid())
                    return false;
                for (std::size_t previous{}; previous < index; ++previous)
                {
                    const auto& candidate = description.event_requirements[previous];
                    const bool duplicate_identity = candidate.system_id == requirement.system_id &&
                        candidate.event_id == requirement.event_id;
                    const bool duplicate_source_name = candidate.system_name == requirement.system_name &&
                        candidate.event_name == requirement.event_name;
                    if (duplicate_identity || duplicate_source_name)
                        return false;
                }
            }
            return true;
        }
        catch (const std::bad_alloc&)
        {
            return false;
        }
    }
}
