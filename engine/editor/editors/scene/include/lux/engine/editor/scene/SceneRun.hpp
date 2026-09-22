#pragma once

#include <chrono>
#include <lux/engine/editor/DocumentRequests.hpp>
#include <lux/engine/editor/scene/visibility.h>
#include <lux/engine/function/render/client/core/RenderSceneId.hpp>

namespace lux::editor::scene
{
struct RunId final
{
    DocumentHandle document;
    std::uint64_t serial{};
    friend bool operator==(RunId, RunId) = default;
};

enum class ERunState : std::uint8_t
{
    IDLE,
    PREPARING,
    RUNNING,
    PAUSED,
    STOPPING,
    FINISHED,
    FAILED
};

enum class ERunPhase : std::uint8_t
{
    NONE,
    STARTUP,
    SIMULATION,
    RESOURCES,
    STABLE,
    PUBLICATION,
    MAINTENANCE,
    DERIVATION
};

struct RunCompletedPhases final
{
    std::uint64_t simulation{}, stable{}, publication{};
};

struct RunStatus final
{
    RunId id;
    ERunState state{ERunState::IDLE};
    bool pause_pending{};
    editing::StateId captured_state;
    std::uint64_t steps{};
    std::chrono::nanoseconds elapsed{};
    // steps/elapsed are the actual clock, including a step whose systems
    // failed. These are the last independently successful phases, not an
    // atomic frame.
    RunCompletedPhases completed;
    ERunPhase failed_phase{ERunPhase::NONE};
    lux::render::RenderSceneId render_scene;
    std::uint64_t published_updates{}, forwarded_updates{}, backpressure_count{};
    // Prepared but not accepted by Runtime; released with the CPU producer.
    // Accepted Program/GPU retirement is independently owned by Runtime.
    std::uint64_t retired_updates{};
    std::uint32_t pending_updates{}, update_high_water{};
    std::size_t retained_resources{};
    std::chrono::nanoseconds simulation_work{}, publication_wait{}, longest_advance{};
    EditorResult<void> result;
};

namespace detail
{
class SceneRun;
struct SceneRunSlot final
{
    editing::HistoryId owner;
};
} // namespace detail

} // namespace lux::editor::scene
