#pragma once

#include <lux/engine/function/script/ScriptSymbol.hpp>

#include <cstdint>
#include <string>

namespace lux::script
{
    // Source/import suggestions only. These are not runtime endpoint identities or binding authority.
    enum class EScriptBindingHintKind : std::uint8_t { HOOK, EVENT };

    struct ScriptBindingHintTarget final
    {
        EScriptBindingHintKind kind{EScriptBindingHintKind::HOOK};
        std::string qualified_name;
        friend bool operator==(const ScriptBindingHintTarget&, const ScriptBindingHintTarget&) noexcept = default;
    };

    struct ScriptBindingHint final
    {
        ScriptSymbolId symbol{};
        ScriptBindingHintTarget target;
    };
}
