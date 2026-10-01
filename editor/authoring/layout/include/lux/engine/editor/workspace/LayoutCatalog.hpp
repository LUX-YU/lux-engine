#pragma once
#include <lux/engine/editor/workspace/WorkspaceValues.hpp>

namespace lux::editor::workspace
{
    struct CatalogVersion final
    {
        std::string digest;
    };
    struct LayoutSummary final
    {
        LayoutId id;
        std::string label;
        std::string version;
    };
    struct CatalogDiagnostic final
    {
        std::string file;
        WorkspaceFailure failure;
    };
    struct LayoutCatalog final
    {
        CatalogVersion version;
        std::vector<LayoutSummary> layouts;
        std::vector<CatalogDiagnostic> diagnostics;
        [[nodiscard]] bool complete() const noexcept
        {
            return diagnostics.empty();
        }
    };
}
