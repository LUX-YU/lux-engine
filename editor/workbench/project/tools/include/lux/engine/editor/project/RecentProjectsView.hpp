#pragma once

#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <filesystem>

namespace lux::editor
{
    class RecentProjects;
}
namespace lux::editor::project
{
    class RecentProjectsView final : public lux::ui::Pane
    {
    public:
        object::TSignal<std::filesystem::path> openRequested{*this};
        RecentProjectsView(object::ObjectDispatcherRef, lux::ui::PaneId, RecentProjects&);
        ~RecentProjectsView() noexcept override;
        RecentProjectsView(const RecentProjectsView&) = delete;
        RecentProjectsView& operator=(const RecentProjectsView&) = delete;
        RecentProjectsView(RecentProjectsView&&) = delete;
        RecentProjectsView& operator=(RecentProjectsView&&) = delete;
        void showFailure(EditorFailure);

    private:
        void update() noexcept override;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
