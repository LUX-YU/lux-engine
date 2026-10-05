#pragma once
#include <filesystem>
#include <lux/engine/editor/EditorError.hpp>
#include <memory>

namespace lux::process
{
    class ExecutionRuntime;
}
namespace lux::services
{
    struct ServiceDescriptor;
}

namespace lux::editor
{
    // Blocking OS launch. The caller schedules it through ExecutionRuntime.
    [[nodiscard]] EditorResult<void> launchEditor(
        const std::filesystem::path& installation,
        const std::filesystem::path& project_file
    ) noexcept;

    extern const services::ServiceDescriptor kProjectLaunchingService;

    // One accepted process launch and its owning result. No window or project data is owned here.
    class ProjectLaunching final
    {
    public:
        ProjectLaunching(process::ExecutionRuntime&, std::filesystem::path installation);
        ~ProjectLaunching();
        ProjectLaunching(const ProjectLaunching&) = delete;
        ProjectLaunching& operator=(const ProjectLaunching&) = delete;
        ProjectLaunching(ProjectLaunching&&) = delete;
        ProjectLaunching& operator=(ProjectLaunching&&) = delete;
        [[nodiscard]] EditorResult<void> request(std::filesystem::path project_file);
        [[nodiscard]] bool pending() const noexcept;
        [[nodiscard]] const EditorResult<void>* result() const noexcept;
        [[nodiscard]] EditorResult<void> acknowledge();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor
