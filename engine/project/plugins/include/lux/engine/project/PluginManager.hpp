#pragma once

#include <lux/engine/project/PluginLibrary.hpp>

namespace lux::project
{
    // One immutable set of admitted runtime modules for one project opening.
    // Registrations retain the binary independently when tasks outlive this owner.
    class PluginManager final
    {
    public:
        [[nodiscard]] static PluginResult<PluginManager> create(
            PluginCatalog catalog,
            std::span<const MetadataIdentity> selected
        ) noexcept;

        PluginManager(PluginManager&&) noexcept = default;
        PluginManager& operator=(PluginManager&&) noexcept = default;
        PluginManager(const PluginManager&) = delete;
        PluginManager& operator=(const PluginManager&) = delete;

        [[nodiscard]] const PluginCatalog& catalog() const noexcept
        {
            return catalog_;
        }
        [[nodiscard]] std::span<const std::shared_ptr<const PluginLibrary>> libraries() const noexcept
        {
            return libraries_;
        }
        [[nodiscard]] const PluginLibrary* find(std::string_view id) const noexcept;

    private:
        explicit PluginManager(PluginCatalog catalog) noexcept : catalog_(std::move(catalog)) {}
        PluginCatalog catalog_;
        std::vector<std::shared_ptr<const PluginLibrary>> libraries_;
    };
}
