#pragma once

#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <filesystem>

namespace lux::editor
{
    // Reusable from Launcher and Editor. Creation always opens a separate Editor process.
    class ProjectCreationPane final : public lux::ui::Pane
    {
    public:
        [[nodiscard]] static EditorResult<std::unique_ptr<ProjectCreationPane>> create(
            lux::ui::Root&,
            process::ExecutionRuntime&,
            std::filesystem::path installation
        ) noexcept;
        ~ProjectCreationPane() noexcept override;
        void requestClose() noexcept;
        [[nodiscard]] bool closed() const noexcept;

    private:
        ProjectCreationPane(lux::ui::Root&, process::ExecutionRuntime&, std::filesystem::path, EditorResult<void>&);
        void update() noexcept override;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
