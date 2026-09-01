#pragma once

#include <lux/engine/function/render/client/RenderClient.hpp>
#include <lux/engine/scene/runtime/render/visibility.h>

#include <cstdint>

namespace lux::scene
{
    enum class ERenderSyncPrepareResult : std::uint8_t
    {
        NO_CHANGES,
        PREPARED_NO_COMMANDS,
        PREPARED_COMMANDS,
        FAILED
    };

    class LUX_ENGINE_SCENE_RUNTIME_RENDER_PUBLIC RenderSyncStage
    {
    public:
        virtual ~RenderSyncStage() noexcept = default;

        // A stage owns its reactive source state. requestFullSync() only marks
        // intent and must not allocate; prepare() performs every fallible
        // allocation and leaves published state untouched.
        [[nodiscard]] virtual bool hasPendingChanges() const noexcept = 0;
        virtual void requestFullSync() noexcept = 0;
        [[nodiscard]] virtual ERenderSyncPrepareResult prepare(render::RenderProgramBuilder<>& builder) noexcept = 0;

        // commitPrepared() runs only after the StateUpdate packet was accepted
        // (or prepare produced no wire commands). It must be allocation-free,
        // noexcept and guaranteed to publish the prepared private state.
        virtual void commitPrepared() noexcept = 0;

        // discardPrepared() rolls back prepare-only private state without
        // clearing reactive, departure or full-sync source state.
        virtual void discardPrepared() noexcept = 0;

    protected:
        RenderSyncStage() = default;
    };
} // namespace lux::scene
