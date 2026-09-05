#pragma once
#include <lux/engine/core/semantic/SemanticType.hpp>
#include <filesystem>
#include <fstream>
#include <span>

namespace lux::script
{
    template <class T> [[nodiscard]] consteval lux::semantic::Layout scriptSemanticLayout() noexcept
    {
        using Traits = lux::semantic::TypeTraits<T>;
        return {lux::semantic::typeId(Traits::CanonicalName), Traits::CanonicalName,
            Traits::AbiKind, Traits::Size, Traits::Alignment};
    }

    // Tool input only, generated from the target's canonical C++ facts. Not a runtime type registry.
    [[nodiscard]] inline bool writeScriptSemanticSchema(
        const std::filesystem::path& path, std::span<const lux::semantic::Layout> additions = {}) noexcept
    {
        try
        {
            std::ofstream output(path, std::ios::binary);
            if (!output) return false;
            output << "{\"schema\":\"lux-script-semantic\",\"version\":1,\"types\":[";
            bool first = true;
            for (const auto values : {std::span<const lux::semantic::Layout>{lux::semantic::BuiltinLayouts}, additions})
            {
                for (const auto& type : values)
                {
                    const bool invalid = type.canonical_name.empty() ||
                        type.canonical_name.find_first_of("\"\\\r\n\t") != std::string_view::npos ||
                        type.type_id != lux::semantic::typeId(type.canonical_name) || type.size == 0U ||
                        type.alignment == 0U || (type.alignment & (type.alignment - 1U)) != 0U;
                    if (invalid) return false;
                    output << (first ? "" : ",") << "{\"canonical_name\":\"" << type.canonical_name
                        << "\",\"type_id\":" << type.type_id << ",\"abi_kind\":" << unsigned(type.abi_kind)
                        << ",\"size\":" << type.size << ",\"alignment\":" << type.alignment << "}";
                    first = false;
                }
            }
            output << "]}\n";
            return static_cast<bool>(output);
        }
        catch (...) { return false; }
    }
}
