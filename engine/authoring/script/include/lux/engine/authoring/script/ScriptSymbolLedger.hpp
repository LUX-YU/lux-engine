#pragma once

#include <lux/engine/authoring/script/visibility.h>
#include <lux/engine/function/script/ScriptSymbol.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lux::authoring::script
{
    enum class EScriptSymbolLedgerError : std::uint8_t
    {
        INVALID_SOURCE_IDENTITY,
        INVALID_SYMBOL,
        SOURCE_NOT_FOUND,
        DESTINATION_EXISTS,
        SYMBOL_SPACE_EXHAUSTED,
        ALLOCATION_FAILURE,
    };

    struct ScriptSymbolLedgerEntry final
    {
        std::string source_identity;
        lux::script::ScriptSymbolId symbol{};

        friend bool operator==(
            const ScriptSymbolLedgerEntry&,
            const ScriptSymbolLedgerEntry&
        ) noexcept = default;
    };

    class LUX_ENGINE_AUTHORING_SCRIPT_PUBLIC ScriptSymbolLedger final
    {
      public:
        [[nodiscard]] static lux::cxx::expected<
            ScriptSymbolLedger,
            EScriptSymbolLedgerError> restore(
                std::span<const ScriptSymbolLedgerEntry> entries,
                lux::script::ScriptSymbolId next_symbol
            ) noexcept;

        [[nodiscard]] lux::cxx::expected<
            lux::script::ScriptSymbolId,
            EScriptSymbolLedgerError> assign(
                std::string_view source_identity
            ) noexcept;

        [[nodiscard]] lux::cxx::expected<void, EScriptSymbolLedgerError>
        rename(
            std::string_view old_source_identity,
            std::string_view new_source_identity
        ) noexcept;

        [[nodiscard]] lux::script::ScriptSymbolId find(
            std::string_view source_identity
        ) const noexcept;

        [[nodiscard]] std::span<const ScriptSymbolLedgerEntry> entries() const
            noexcept;
        [[nodiscard]] lux::script::ScriptSymbolId nextSymbol() const noexcept;

      private:
        std::vector<ScriptSymbolLedgerEntry> entries_;
        lux::script::ScriptSymbolId next_symbol_{1U};
    };
}
