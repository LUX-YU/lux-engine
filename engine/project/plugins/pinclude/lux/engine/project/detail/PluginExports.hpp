#pragma once
#include <lux/engine/project/PluginCatalog.hpp>
#include <lux/engine/dynamic_library/DynamicLibrary.hpp>

namespace lux::project::detail
{
    using engine::platform::DynamicLibrary;
    template <class Table>
    PluginResult<const Table*> exports(const DynamicLibrary& library, std::string_view plugin, const char* symbol)
    {
        using Get = const Table*() noexcept;
        const auto get = library.get_symbol<Get>(symbol);
        if (!get)
            return lux::cxx::unexpected(PluginFailure{EPluginError::MISSING_EXPORT, std::string(plugin), symbol});
        const auto* table = get();
        const bool invalid = !table || table->structure_size != sizeof(Table) || table->interface_version != 1;
        if (invalid)
            return lux::cxx::unexpected(PluginFailure{EPluginError::INVALID_EXPORT, std::string(plugin), symbol});
        if constexpr (requires {
                          table->count;
                          table->entries;
                      })
        {
            if (table->count > 65536 || (table->count && !table->entries))
                return lux::cxx::unexpected(PluginFailure{EPluginError::INVALID_EXPORT, std::string(plugin), symbol});
        }
        return table;
    }

    template <class Table, class Entry>
    PluginResult<void> copyExports(
        const DynamicLibrary& library,
        std::string_view plugin,
        const char* symbol,
        std::vector<Entry>& output,
        const std::shared_ptr<const void>& owner
    )
    {
        auto table = exports<Table>(library, plugin, symbol);
        if (!table)
            return lux::cxx::unexpected(table.error());
        if ((*table)->count)
            output.assign((*table)->entries, (*table)->entries + (*table)->count);
        if constexpr (requires(Entry& entry) { entry.code_lifetime; })
            for (auto& entry : output)
                entry.code_lifetime = owner;
        return {};
    }
}
