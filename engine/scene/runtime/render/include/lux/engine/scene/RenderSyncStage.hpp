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

        [[nodiscard]] virtual bool hasPendingChanges() const noexcept = 0;
        virtual void requestFullSync() noexcept = 0;
        [[nodiscard]] virtual ERenderSyncPrepareResult prepare(render::RenderProgramBuilder<>& builder) noexcept = 0;
        virtual void commitPrepared() noexcept = 0;
        virtual void discardPrepared() noexcept = 0;

    protected:
        RenderSyncStage() = default;
    };
} // namespace lux::scene
