#include <lux/engine/authoring/ScriptSymbolLedger.hpp>

#include <algorithm>
#include <limits>
#include <new>

namespace lux::authoring
{
    namespace
    {
        [[nodiscard]] bool validEntries(
            std::span<const ScriptSymbolLedgerEntry> entries,
            lux::script::ScriptSymbolId next_symbol
        ) noexcept
        {
            if (next_symbol == lux::script::InvalidScriptSymbolId)
                return false;
            for (std::size_t index{}; index < entries.size(); ++index)
            {
                const auto& entry = entries[index];
                if (entry.source_identity.empty() ||
                    entry.symbol == lux::script::InvalidScriptSymbolId ||
                    entry.symbol >= next_symbol)
                {
                    return false;
                }
                for (std::size_t previous{}; previous < index; ++previous)
                {
                    if (entries[previous].source_identity ==
                            entry.source_identity ||
                        entries[previous].symbol == entry.symbol)
                    {
                        return false;
                    }
                }
            }
            return true;
        }
    }

    lux::cxx::expected<ScriptSymbolLedger, EScriptSymbolLedgerError>
    ScriptSymbolLedger::restore(
        std::span<const ScriptSymbolLedgerEntry> entries,
        lux::script::ScriptSymbolId next_symbol
    ) noexcept
    {
        if (!validEntries(entries, next_symbol))
        {
            return lux::cxx::unexpected(
                EScriptSymbolLedgerError::INVALID_SYMBOL
            );
        }
        try
        {
            ScriptSymbolLedger result;
            result.entries_.assign(entries.begin(), entries.end());
            result.next_symbol_ = next_symbol;
            return result;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                EScriptSymbolLedgerError::ALLOCATION_FAILURE
            );
        }
    }

    lux::cxx::expected<
        lux::script::ScriptSymbolId,
        EScriptSymbolLedgerError> ScriptSymbolLedger::assign(
            std::string_view source_identity
        ) noexcept
    {
        if (source_identity.empty())
        {
            return lux::cxx::unexpected(
                EScriptSymbolLedgerError::INVALID_SOURCE_IDENTITY
            );
        }
        const auto existing = find(source_identity);
        if (existing != lux::script::InvalidScriptSymbolId)
            return existing;
        if (next_symbol_ == std::numeric_limits<
                lux::script::ScriptSymbolId>::max())
        {
            return lux::cxx::unexpected(
                EScriptSymbolLedgerError::SYMBOL_SPACE_EXHAUSTED
            );
        }
        try
        {
            const auto assigned = next_symbol_++;
            entries_.push_back({std::string(source_identity), assigned});
            return assigned;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                EScriptSymbolLedgerError::ALLOCATION_FAILURE
            );
        }
    }

    lux::cxx::expected<void, EScriptSymbolLedgerError>
    ScriptSymbolLedger::rename(
        std::string_view old_source_identity,
        std::string_view new_source_identity
    ) noexcept
    {
        if (old_source_identity.empty() || new_source_identity.empty())
        {
            return lux::cxx::unexpected(
                EScriptSymbolLedgerError::INVALID_SOURCE_IDENTITY
            );
        }
        const auto old_entry = std::find_if(
            entries_.begin(),
            entries_.end(),
            [&](const auto& entry) noexcept
            {
                return entry.source_identity == old_source_identity;
            }
        );
        if (old_entry == entries_.end())
        {
            return lux::cxx::unexpected(
                EScriptSymbolLedgerError::SOURCE_NOT_FOUND
            );
        }
        if (std::any_of(
                entries_.begin(),
                entries_.end(),
                [&](const auto& entry) noexcept
                {
                    return entry.source_identity == new_source_identity;
                }
            ))
        {
            return lux::cxx::unexpected(
                EScriptSymbolLedgerError::DESTINATION_EXISTS
            );
        }
        try
        {
            old_entry->source_identity = new_source_identity;
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                EScriptSymbolLedgerError::ALLOCATION_FAILURE
            );
        }
    }

    lux::script::ScriptSymbolId ScriptSymbolLedger::find(
        std::string_view source_identity
    ) const noexcept
    {
        const auto found = std::find_if(
            entries_.begin(),
            entries_.end(),
            [&](const auto& entry) noexcept
            {
                return entry.source_identity == source_identity;
            }
        );
        return found == entries_.end()
            ? lux::script::InvalidScriptSymbolId
            : found->symbol;
    }

    std::span<const ScriptSymbolLedgerEntry> ScriptSymbolLedger::entries() const
        noexcept
    {
        return entries_;
    }

    lux::script::ScriptSymbolId ScriptSymbolLedger::nextSymbol() const noexcept
    {
        return next_symbol_;
    }
}
