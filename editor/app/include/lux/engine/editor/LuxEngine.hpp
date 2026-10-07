#pragma once
#include <lux/engine/editor/EditorAssembly.hpp>
#include <lux/engine/editor/EditorLayout.hpp>
#include <lux/engine/editor/FrameStatistics.hpp>
#include <lux/engine/editor/FrameworkResult.hpp>
#include <lux/engine/editor/ProjectDescription.hpp>
#include <lux/engine/editor/ProjectTransition.hpp>
#include <memory>

namespace lux::engine
{
    class EngineContext;
}
namespace lux::editor
{
    class EditorWindow;
    class EditorContext;
    struct EditorConfig final
    {
        std::string title{"LuxEngine"};
        int width{1280};
        int height{800};
        // Forwarded to the renderer/swapchain; never used as a CPU frame deadline.
        bool enable_vsync{true};
        EditorLayout layout;
        std::vector<ProjectPluginLocation> plugin_locations;
    };

    enum class EFrameStatus : std::uint8_t
    {
        RUNNING,
        EXIT_REQUESTED
    };

    class LuxEngine final
    {
    public:
        [[nodiscard]] static FrameworkResult<std::unique_ptr<LuxEngine>> create(
            EditorConfig = {},
            EditorAssembly = {}
        ) noexcept;
        ~LuxEngine() noexcept;
        LuxEngine(const LuxEngine&) = delete;
        LuxEngine& operator=(const LuxEngine&) = delete;
        LuxEngine(LuxEngine&&) = delete;
        LuxEngine& operator=(LuxEngine&&) = delete;

        // Admission only. Preparation/closure/adoption progress in frame(); failures keep the current project.
        [[nodiscard]] FrameworkResult<void> openProject(std::filesystem::path manifest_file) noexcept;
        [[nodiscard]] FrameworkResult<void> createProject(ProjectCreateRequest) noexcept;
        [[nodiscard]] FrameworkResult<void> cancelProjectTransition() noexcept;
        [[nodiscard]] ProjectTransitionStatus projectStatus() const noexcept;
        [[nodiscard]] FrameworkResult<void> closeProject() noexcept;
        [[nodiscard]] FrameworkResult<void> exec() noexcept;
        // One host iteration; callers do not drive SceneRuntime a second time.
        [[nodiscard]] FrameworkResult<EFrameStatus> frame() noexcept;
        [[nodiscard]] FrameStatistics statistics() const noexcept;
        [[nodiscard]] EditorWindow& window() noexcept;
        [[nodiscard]] engine::EngineContext& engine() noexcept;
        [[nodiscard]] const engine::EngineContext& engine() const noexcept;
        [[nodiscard]] EditorContext* context() noexcept;

    private:
        struct Impl;
        explicit LuxEngine(std::unique_ptr<Impl>) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor
