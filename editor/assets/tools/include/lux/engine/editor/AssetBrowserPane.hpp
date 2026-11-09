#pragma once

#include <lux/engine/editor/AssetImportEditor.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <memory>

namespace lux::editor
{
    class ProjectAssets;

    class AssetBrowserPane final : public ui::Pane
    {
    public:
        AssetBrowserPane(ProjectAssets&, std::vector<AssetImportEditor>, std::string title);
        ~AssetBrowserPane() noexcept;

    private:
        void refresh() noexcept;
        void selectImporter() noexcept;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor
