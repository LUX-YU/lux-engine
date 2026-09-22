#pragma once

#include <lux/engine/function/render/client/RenderClient.hpp>
#include <lux/engine/function/render/client/core/RenderEntityId.hpp>
#include <lux/engine/scene/render/visibility.h>

#include <cstdint>
#include <lux/engine/simulation/ecs/Entity.hpp>

namespace lux::scene
{
[[nodiscard]] constexpr render::RenderEntityId toRenderEntity(simulation::ecs::Entity entity) noexcept
{
    return static_cast<render::RenderEntityId>(simulation::ecs::entityBits(entity));
}

enum class ERenderSyncError : std::uint8_t
{
    STAGE_PREPARE_FAILURE
};
struct RenderSyncFailure final
{
    ERenderSyncError code{};
};

struct RenderSyncStatistics final
{
    std::uint64_t published{}; // Prepared immutable updates owned by RenderSystem.
    std::uint64_t forwarded{}; // Accepted by Program; not GPU completion.
    std::uint64_t backpressured{};
    std::uint32_t pending{}, high_water{};
    std::uint64_t retired_unforwarded{};
};

enum class ERenderSyncPrepareResult : std::uint8_t
{
    NO_CHANGES,
    PREPARED_NO_COMMANDS,
    PREPARED_COMMANDS,
    FAILED
};

class LUX_ENGINE_SCENE_RENDER_PUBLIC RenderSyncStage
{
  public:
    virtual ~RenderSyncStage() noexcept = default;

    // A stage owns its reactive source state. requestFullSync() only marks
    // intent and must not allocate; prepare() performs every fallible
    // allocation and leaves published state untouched.
    [[nodiscard]] virtual bool hasPendingChanges() const noexcept = 0;
    virtual void requestFullSync() noexcept = 0;
    [[nodiscard]] virtual ERenderSyncPrepareResult prepare(render::RenderProgramBuilder<> &builder) noexcept = 0;

    // commitPrepared() runs after a complete owning StateUpdate packet exists
    // (or prepare produced no wire commands). Downstream acceptance may still
    // be pending: the owner retains that packet and never extracts it twice.
    // New Registry changes then accumulate separately. It must be allocation-free,
    // noexcept and guaranteed to publish the prepared private state.
    virtual void commitPrepared() noexcept = 0;

    // discardPrepared() rolls back prepare-only private state without
    // clearing reactive, departure or full-sync source state.
    virtual void discardPrepared() noexcept = 0;

  protected:
    RenderSyncStage() = default;
};
} // namespace lux::scene
