#include "ProjectCatalogAdapter.hpp"
#include <lux/engine/editor/ui/InspectorInteraction.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>

namespace lux::editor::ui
{
    project::ProjectCatalogAccess InspectorInteraction::catalogAccess() const noexcept
    {
        return projectCatalogAccess(catalog_);
    }
    project::ProjectCatalogAccess projectCatalogAccess(const ProjectStorage* storage) noexcept
    {
        if (!storage)
            return {};
        return {
            storage,
            +[](const void* owner) -> project::ProjectQueryResult<project::ProjectCatalogVersion> {
                const auto& value = *static_cast<const ProjectStorage*>(owner);
                const auto reference = value.reference({});
                return project::ProjectCatalogVersion{reference.project_instance, reference.catalog_revision};
            },
            +[](const void* owner) -> project::ProjectQueryResult<project::ProjectCatalog> {
                const auto& value = *static_cast<const ProjectStorage*>(owner);
                const auto reference = value.reference({});
                const auto rows = value.catalog();
                return project::ProjectCatalog{
                    {reference.project_instance, reference.catalog_revision},
                    value.manifest().name,
                    {rows.begin(), rows.end()}
                };
            },
            +[](const void* owner, AssetReference reference, std::uint32_t magic
             ) -> project::ProjectQueryResult<asset::AssetId> {
                auto result = static_cast<const ProjectStorage*>(owner)->resolveReference(reference, magic);
                if (!result)
                {
                    if (const auto* cause = std::any_cast<EAssetReferenceError>(&result.error().cause))
                        return lux::cxx::unexpected(project::VProjectQueryFailure{*cause});
                    return lux::cxx::unexpected(project::VProjectQueryFailure{project::EProjectQueryError::IO});
                }
                return *result;
            }
        };
    }
}
