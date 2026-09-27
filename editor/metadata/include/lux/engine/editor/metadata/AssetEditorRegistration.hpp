#pragma once

#include <lux/engine/editor/metadata/PaneRegistration.hpp>
#include <lux/engine/resource/identity/AssetId.hpp>

namespace lux::editor
{
    struct ProjectAssetEntry;

    struct AssetEditorRegistration final
    {
        lux::ui::PaneTypeId type;
        bool (*accepts)(const ProjectAssetEntry&) noexcept {};
        PaneRegistration::CreateResult (*open)(PaneManager&, asset::AssetId) noexcept {};
        std::shared_ptr<const void> code_lifetime;

        [[nodiscard]] bool valid() const noexcept
        {
            return type.isValid() && accepts && open;
        }
    };

    // A synchronous value query; it never borrows the tool's model or loading request.
    struct AssetEditorQuery final
    {
        asset::AssetId asset;
        bool matches{};
    };
}
