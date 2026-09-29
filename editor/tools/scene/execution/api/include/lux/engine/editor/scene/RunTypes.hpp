#pragma once

#include <lux/engine/editor/sessions/ContentStamp.hpp>
#include <lux/engine/editor/scene/SceneEditError.hpp>
#include <lux/engine/scene/SceneRuntime.hpp>
#include <lux/engine/scene/ScenePackage.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/function/render/client/core/RenderSceneId.hpp>

namespace lux::editor::scene
{
    struct StartRunId final
    {
        std::uint64_t domain{}, serial{};
        friend auto operator<=>(StartRunId, StartRunId) = default;
    };
    struct RunId final
    {
        std::uint64_t domain{};
        std::uint32_t slot{}, generation{};
        [[nodiscard]] bool valid() const noexcept
        {
            return domain && generation;
        }
        friend auto operator<=>(RunId, RunId) = default;
    };

    enum class ERunState : std::uint8_t
    {
        RUNNING,
        PAUSED,
        STOPPING,
        STOPPED,
        FAILED
    };
    enum class ERunError : std::uint8_t
    {
        INVALID_ID,
        WRONG_THREAD,
        BUSY,
        CAPACITY,
        INVALID_CONFIGURATION,
        CANCELLED,
        NOT_READY,
        STOPPED
    };
    struct RunFailure final
    {
        using VCause = std::variant<
            ERunError,
            SceneEditError,
            lux::scene::SceneRuntimeFailure,
            lux::scene::ScenePackageFailure,
            process::EExecutionError,
            editing::EditFailure>;
        VCause cause{ERunError::INVALID_ID};
    };
    template <class T> using RunResult = lux::cxx::expected<T, RunFailure>;

    struct RunConfiguration final
    {
        std::chrono::nanoseconds fixed_step{std::chrono::milliseconds(16)};
        lux::system::SystemInstanceId viewport;
        friend bool operator==(const RunConfiguration&, const RunConfiguration&) = default;
    };
    struct RunProvenance final
    {
        sessions::ContentStamp content;
        RunConfiguration configuration;
    };
    struct RunInfo final
    {
        RunId id;
        RunProvenance provenance;
        lux::scene::SceneInstanceId instance;
        ERunState state{ERunState::RUNNING};
        bool pause_pending{};
        std::uint64_t structure_revision{};
        render::RenderSceneId render_scene;
        double coordinate_page_size{};
        std::uint64_t published_updates{}, forwarded_updates{}, retired_updates{}, backpressure_count{};
        std::uint32_t pending_updates{}, update_high_water{};
        std::size_t retained_resources{};
        lux::scene::SceneDriveSnapshot progress;
        RunResult<void> result;
    };
    struct StepTicket final
    {
        RunId run;
        lux::scene::SceneStepTicket step;
        friend auto operator<=>(const StepTicket&, const StepTicket&) = default;
    };
    struct StopTicket final
    {
        RunId run;
        lux::scene::InstanceRetirement retirement;
        [[nodiscard]] bool complete() const noexcept
        {
            return retirement.complete();
        }
    };
}
