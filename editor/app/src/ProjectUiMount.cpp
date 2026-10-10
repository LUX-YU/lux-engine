#include <exception>
#include <lux/engine/editor/detail/ProjectUiMount.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>
#include <utility>

namespace lux::editor::detail
{
    ProjectUiMount::~ProjectUiMount() noexcept
    {
        clear();
    }

    ProjectUiMount::ProjectUiMount(ProjectUiMount&& other) noexcept
        : root_(std::exchange(other.root_, nullptr)), handles_(std::move(other.handles_))
    {
    }

    ProjectUiMount& ProjectUiMount::operator=(ProjectUiMount&& other) noexcept
    {
        if (this != &other)
        {
            clear();
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

    void ProjectUiMount::clear() noexcept
    {
        if (!root_)
        {
            return;
        }
        if (handles_.empty())
        {
            disarm();
            return;
        }
        auto commit = [this](std::span<const ui::PaneHandle>) noexcept { disarm(); };
        auto removed = root_->replacePanes(handles_, {}, commit);
        if (!removed)
        {
            std::terminate();
        }
        // Root is final and this responsibility is disarmed before user destructors run.
        removed->clear();
    }
} // namespace lux::editor::detail
