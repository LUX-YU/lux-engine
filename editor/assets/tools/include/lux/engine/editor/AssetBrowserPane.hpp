#pragma once

#include <lux/engine/ui/Pane.hpp>
#include <memory>

namespace lux::editor
{
    class ProjectAssets;

    class AssetBrowserPane final : public ui::Pane
    {
    public:
        AssetBrowserPane(ProjectAssets&, std::string title);
        ~AssetBrowserPane() noexcept;

    private:
        void refresh() noexcept;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor
