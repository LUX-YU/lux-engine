#pragma once

#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/editor/scene/RunTypes.hpp>
#include <lux/engine/editor/sessions/ContentStamp.hpp>
#include <lux/engine/ui/Attachment.hpp>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace lux::ui
{
    class Root;
}
namespace lux::services
{
    class ServiceRegistry;
    class ServiceScope;
    struct ServiceDescriptor;
} // namespace lux::services
namespace lux::editor::desktop
{
    class UiRegistry;
}
namespace lux::editor::sessions
{
    class SessionStore;
}
namespace lux::editor::scene
{
    class RunStore;
    struct ProjectionEnvironment;
    enum class ESceneTool : std::uint8_t;

    // Owning observations; no live Registry borrow, preparation pointer or mutable Run record escapes.
    struct RunPresentationInfo final
    {
        struct StepResult final
        {
            StepTicket ticket;
            lux::scene::SceneStepStatus status;
        };
        StartRunId start;
        sessions::ContentStamp source;
        std::optional<RunId> run;
        std::vector<lux::ui::PaneHandle> views;
        std::vector<StepResult> steps;
        std::optional<EditorFailure> failure;
        bool preparing{};
        bool stopping{};
    };

    // Workbench use case: owns preparations, window associations and unacknowledged UI results.
    // RunStore retains Run and retirement responsibility; this class never drives SceneRuntime.
    // Root and composition scopes outlive settled() and the last use of this fixed-address object.
    class ScenePlayback final
    {
    public:
        ScenePlayback(
            std::shared_ptr<sessions::SessionStore>,
            std::shared_ptr<RunStore>,
            std::shared_ptr<const ProjectionEnvironment>,
            lux::ui::Root&,
            desktop::UiRegistry&,
            services::ServiceRegistry&,
            services::ServiceScope&
        );
        ~ScenePlayback();
        ScenePlayback(const ScenePlayback&) = delete;
        ScenePlayback& operator=(const ScenePlayback&) = delete;
        ScenePlayback(ScenePlayback&&) = delete;
        ScenePlayback& operator=(ScenePlayback&&) = delete;

        [[nodiscard]] EditorResult<StartRunId> play(sessions::ContentStamp);
        [[nodiscard]] EditorResult<void> step(RunId);
        [[nodiscard]] EditorResult<void> stop(RunId);
        [[nodiscard]] EditorResult<lux::ui::PaneHandle> showTool(lux::ui::PaneHandle, ESceneTool);
        // Review can delay adoption without dropping a ready preparation or stopping accepted work.
        [[nodiscard]] EditorResult<void> update(bool accept_prepared = true);
        [[nodiscard]] EditorResult<void> requestClose() noexcept;
        [[nodiscard]] bool settled() const noexcept;
        [[nodiscard]] EditorResult<std::vector<RunPresentationInfo>> reports() const;
        [[nodiscard]] EditorResult<void> acknowledgeFailure(StartRunId);
        [[nodiscard]] EditorResult<void> acknowledgeStep(StepTicket);
        // Called after the original Root/Session close transaction commits, never while preparing it.
        [[nodiscard]] EditorResult<void> forgetViews(std::span<const lux::ui::PaneHandle>) noexcept;
        [[nodiscard]] EditorResult<void> requestStop(RunId) noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
    extern const services::ServiceDescriptor kScenePlaybackService;
} // namespace lux::editor::scene
