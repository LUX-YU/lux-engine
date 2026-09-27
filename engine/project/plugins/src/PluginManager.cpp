#include <lux/engine/project/PluginManager.hpp>

#include <algorithm>
#include <exception>

namespace lux::project
{
    PluginResult<PluginManager> PluginManager::create(
        PluginCatalog catalog,
        std::span<const MetadataIdentity> selected
    ) noexcept
    {
        try
        {
            PluginManager candidate(std::move(catalog));
            std::vector<const PluginDescription*> descriptions;
            // Resolve the entire selection before opening any module.
            for (std::size_t index{}; index < selected.size(); ++index)
            {
                const auto& identity = selected[index];
                const bool duplicate = std::ranges::any_of(selected.first(index), [&](const auto& previous) {
                    return previous.id == identity.id;
                });
                if (duplicate)
                    return lux::cxx::unexpected(PluginFailure{EPluginError::DUPLICATE_IDENTITY, identity.id});
                const auto* description = candidate.catalog_.find(identity.id);
                if (!description)
                    return lux::cxx::unexpected(PluginFailure{EPluginError::MISSING_DEPENDENCY, identity.id});
                if (description->identity.version != identity.version)
                    return lux::cxx::unexpected(PluginFailure{EPluginError::DEPENDENCY_VERSION_MISMATCH, identity.id});
                auto order = candidate.catalog_.loadOrder(identity.id);
                if (!order)
                    return lux::cxx::unexpected(order.error());
                for (const auto* entry : *order)
                    if (std::ranges::find(descriptions, entry) == descriptions.end())
                        descriptions.push_back(entry);
            }
            candidate.libraries_.reserve(descriptions.size());
            for (const auto* description : descriptions)
            {
                auto library = PluginLibrary::load(*description, candidate.libraries_);
                if (!library)
                    return lux::cxx::unexpected(library.error());
                candidate.libraries_.push_back(std::move(*library));
            }
            return candidate;
        }
        catch (const std::exception& error)
        {
            return lux::cxx::unexpected(PluginFailure{EPluginError::LIBRARY_LOAD_FAILURE, {}, {}, error.what()});
        }
    }

    const PluginLibrary* PluginManager::find(std::string_view id) const noexcept
    {
        const auto found =
            std::ranges::find_if(libraries_, [&](const auto& library) { return library->identity().id == id; });
        return found == libraries_.end() ? nullptr : found->get();
    }
}
