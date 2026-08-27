#include <lux/engine/authoring/ScriptAuthoringSemanticCatalog.hpp>

#include <lux/engine/simulation/ScriptMountDescription.hpp>
#include <lux/engine/simulation/SimulationStepInfo.hpp>

#include <algorithm>
#include <charconv>
#include <format>
#include <new>

namespace lux::authoring
{
    namespace
    {
        [[nodiscard]] std::string passName(
            lux::script::EScriptPassMode pass
        )
        {
            return pass == lux::script::EScriptPassMode::CONST_REF
                ? "CONST_REF"
                : "VALUE";
        }

        [[nodiscard]] ScriptAuthoringSemanticEntry entry(
            std::string_view name,
            std::uint8_t abi_kind,
            std::uint32_t size,
            std::uint32_t alignment,
            std::vector<lux::script::EScriptPassMode> passes,
            lux::script::EScriptPassMode default_pass,
            bool return_allowed
        )
        {
            const auto id = lux::script::scriptSemanticTypeId(name);
            return {
                std::string{name},
                std::format("0x{:016X}", id),
                id,
                abi_kind,
                size,
                alignment,
                std::move(passes),
                default_pass,
                return_allowed};
        }

        [[nodiscard]] bool validHexId(
            std::string_view text,
            std::uint64_t expected
        ) noexcept
        {
            if (text.size() != 18U || text[0] != '0' || text[1] != 'x')
                return false;
            std::uint64_t parsed{};
            const auto result = std::from_chars(
                text.data() + 2U,
                text.data() + text.size(),
                parsed,
                16
            );
            return result.ec == std::errc{} &&
                result.ptr == text.data() + text.size() && parsed == expected;
        }
    }

    lux::cxx::expected<void, EScriptAuthoringSemanticCatalogError>
    ScriptAuthoringSemanticCatalog::add(
        ScriptAuthoringSemanticEntry value
    ) noexcept
    {
        if (value.canonical_name.empty() || value.type_id == 0U ||
            value.type_id != lux::script::scriptSemanticTypeId(
                value.canonical_name) ||
            !validHexId(value.type_id_hex, value.type_id) ||
            value.abi_kind == LUX_SCRIPT_VK_VOID ||
            value.abi_kind > LUX_SCRIPT_VK_STRUCT_REF ||
            value.size == 0U || value.alignment == 0U ||
            (value.alignment & (value.alignment - 1U)) != 0U ||
            value.allowed_parameter_passes.empty() ||
            std::find(
                value.allowed_parameter_passes.begin(),
                value.allowed_parameter_passes.end(),
                value.default_parameter_pass) ==
                value.allowed_parameter_passes.end())
        {
            return lux::cxx::unexpected(
                EScriptAuthoringSemanticCatalogError::INVALID_ENTRY);
        }
        const auto found = std::lower_bound(
            entries_.begin(),
            entries_.end(),
            value.canonical_name,
            [](const auto& left, std::string_view right) noexcept
            {
                return left.canonical_name < right;
            }
        );
        if (found != entries_.end() &&
            found->canonical_name == value.canonical_name)
        {
            return *found == value
                ? lux::cxx::expected<
                    void,
                    EScriptAuthoringSemanticCatalogError>{}
                : lux::cxx::expected<
                    void,
                    EScriptAuthoringSemanticCatalogError>{
                    lux::cxx::unexpected(
                        EScriptAuthoringSemanticCatalogError::
                            IDENTITY_CONFLICT)};
        }
        try
        {
            entries_.insert(found, std::move(value));
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                EScriptAuthoringSemanticCatalogError::ALLOCATION_FAILURE);
        }
    }

    std::span<const ScriptAuthoringSemanticEntry>
    ScriptAuthoringSemanticCatalog::entries() const noexcept
    {
        return entries_;
    }

    const ScriptAuthoringSemanticEntry* ScriptAuthoringSemanticCatalog::find(
        std::string_view canonical_name
    ) const noexcept
    {
        const auto found = std::lower_bound(
            entries_.begin(),
            entries_.end(),
            canonical_name,
            [](const auto& left, std::string_view right) noexcept
            {
                return left.canonical_name < right;
            }
        );
        return found != entries_.end() &&
                found->canonical_name == canonical_name
            ? std::addressof(*found)
            : nullptr;
    }

    std::string ScriptAuthoringSemanticCatalog::canonicalJson() const
    {
        std::string result{
            "{\"schema\":\"lux-script-semantics\",\"version\":1,\"types\":["};
        for (std::size_t index{}; index < entries_.size(); ++index)
        {
            if (index != 0U)
                result.push_back(',');
            const auto& value = entries_[index];
            result += std::format(
                "{{\"abi_kind\":{},\"alignment\":{},"
                "\"allowed_parameter_passes\":[",
                value.abi_kind,
                value.alignment
            );
            for (std::size_t pass{};
                 pass < value.allowed_parameter_passes.size(); ++pass)
            {
                if (pass != 0U)
                    result.push_back(',');
                result += std::format(
                    "\"{}\"",
                    passName(value.allowed_parameter_passes[pass])
                );
            }
            result += std::format(
                "],\"canonical_name\":\"{}\","
                "\"default_parameter_pass\":\"{}\","
                "\"return_allowed\":{},\"size\":{},"
                "\"type_id\":\"{}\"}}",
                value.canonical_name,
                passName(value.default_parameter_pass),
                value.return_allowed ? "true" : "false",
                value.size,
                value.type_id_hex
            );
        }
        result += "]}";
        return result;
    }

    lux::cxx::expected<
        ScriptAuthoringSemanticCatalog,
        EScriptAuthoringSemanticCatalogError>
    makeBaseScriptAuthoringSemanticCatalog() noexcept
    {
        try
        {
            ScriptAuthoringSemanticCatalog result;
            for (const auto& layout :
                 lux::script::ScriptBuiltinSemanticLayouts)
            {
                auto added = result.add(entry(
                    layout.canonical_name,
                    layout.abi_kind,
                    layout.size,
                    layout.alignment,
                    {
                        lux::script::EScriptPassMode::VALUE,
                        lux::script::EScriptPassMode::CONST_REF},
                    lux::script::EScriptPassMode::VALUE,
                    true
                ));
                if (!added)
                    return lux::cxx::unexpected(added.error());
            }
            auto step = result.add(entry(
                lux::script::ScriptSemanticTypeTraits<
                    lux::simulation::SimulationStepInfo>::CanonicalName,
                LUX_SCRIPT_VK_STRUCT_REF,
                sizeof(lux::simulation::SimulationStepInfo),
                alignof(lux::simulation::SimulationStepInfo),
                {lux::script::EScriptPassMode::CONST_REF},
                lux::script::EScriptPassMode::CONST_REF,
                false
            ));
            if (!step)
                return lux::cxx::unexpected(step.error());
            auto stop = result.add(entry(
                lux::simulation::BehaviorStopReasonCanonicalName,
                LUX_SCRIPT_VK_UINT32,
                sizeof(lux::simulation::EBehaviorStopReason),
                alignof(lux::simulation::EBehaviorStopReason),
                {lux::script::EScriptPassMode::VALUE},
                lux::script::EScriptPassMode::VALUE,
                false
            ));
            if (!stop)
                return lux::cxx::unexpected(stop.error());
            return result;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                EScriptAuthoringSemanticCatalogError::ALLOCATION_FAILURE);
        }
    }
}
