#include <exception>
#include <lux/engine/editor/ProjectErrors.hpp>
#include <lux/engine/editor/detail/ProjectUiMount.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>
#include <unordered_map>
#include <utility>

namespace lux::editor::detail
{
    ProjectResult<ui::PanePlacement> ProjectUiMount::placement(const ProjectLayout& layout) noexcept
    {
        if (auto valid = validateProjectLayout(layout); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        std::unordered_map<std::string_view, std::uint32_t> panes;
        panes.reserve(layout.panes.size());
        for (std::uint32_t i = 0; i < layout.panes.size(); ++i)
        {
            panes.emplace(layout.panes[i].name, i);
        }
        ui::PanePlacement placement;
        placement.tree.nodes.reserve(layout.nodes.size());
        placement.added.reserve(layout.panes.size());
        for (std::uint32_t i = 0; i < layout.nodes.size(); ++i)
        {
            const auto& node = layout.nodes[i];
            const auto split = node.split == ELayoutSplit::LEAF         ? ui::EDockSplit::LEAF
                               : node.split == ELayoutSplit::HORIZONTAL ? ui::EDockSplit::HORIZONTAL
                                                                        : ui::EDockSplit::VERTICAL;
            placement.tree.nodes.push_back({split, node.first, node.second, node.ratio});
            for (const auto& key : node.panes)
            {
                placement.added.push_back({i, panes.find(key)->second});
            }
        }
        for (const auto& surface : layout.surfaces)
        {
            placement.tree.surfaces.push_back(
                {surface.node, {{surface.x, surface.y}, {surface.width, surface.height}}, surface.floating}
            );
        }
        return placement;
    }

    ProjectUiMount::~ProjectUiMount() noexcept
    {
        if (!unmount())
        {
            std::terminate();
        }
    }

    ProjectUiMount::ProjectUiMount(ProjectUiMount&& other) noexcept
        : root_(std::exchange(other.root_, nullptr)), handles_(std::move(other.handles_))
    {
    }

    ProjectUiMount& ProjectUiMount::operator=(ProjectUiMount&& other) noexcept
    {
        if (this != &other)
        {
            if (!unmount())
            {
                std::terminate();
            }
            root_ = std::exchange(other.root_, nullptr);
            handles_ = std::move(other.handles_);
        }
        return *this;
    }

    void ProjectUiMount::prepare(ui::Root& root, std::size_t count)
    {
        if (root_)
        {
            std::terminate();
        }
        handles_.reserve(count);
        root_ = &root;
    }

    void ProjectUiMount::arm(std::span<const ui::PaneHandle> handles) noexcept
    {
        const bool invalid_preparation = !root_ || !handles_.empty() || handles.size() > handles_.capacity();
        if (invalid_preparation)
        {
            std::terminate();
        }
        handles_.assign(handles.begin(), handles.end());
    }

    void ProjectUiMount::disarm() noexcept
    {
        root_ = nullptr;
        handles_.clear();
    }

    FrameworkResult<void> ProjectUiMount::unmount() noexcept
    {
        if (!root_)
        {
            return {};
        }
        if (handles_.empty())
        {
            disarm();
            return {};
        }
        auto commit = [this](std::span<const ui::PaneHandle>) noexcept { disarm(); };
        auto removed = root_->replacePanes(handles_, {}, commit);
        if (!removed)
        {
            return cxx::unexpected(
                error::Error{Errors::EditorCannotMountProjectWindows, {static_cast<std::uint64_t>(removed.error())}}
            );
        }
        // Root is final and this responsibility is disarmed before user destructors run.
        removed->clear();
        return {};
    }
} // namespace lux::editor::detail
