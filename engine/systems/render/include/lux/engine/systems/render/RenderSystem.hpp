#pragma once

#include <lux/engine/EngineContext.hpp>
#include <lux/engine/render/capabilities/CapabilityComposition.hpp>
#include <lux/engine/scene/SceneSystem.hpp>
#include <lux/engine/systems/render/RenderCapabilityMetaRegistry.hpp>
#include <lux/engine/systems/render/visibility.h>

namespace lux::world
{
    class World;
}

namespace lux::engine::render
{
    // One externally assembled scene system. Metadata is borrowed through initialization, never queried by execute.
    class LUX_ENGINE_SYSTEM_RENDER_PUBLIC RenderSystem final : public scene::SceneSystem
    {
    public:
        using Result = lux::render::RenderResult<std::unique_ptr<RenderSystem>>;

        // Cold local construction. Engine adoption schedules remote creation before any Scene execution.
        [[nodiscard]] static Result create(
            lux::render::CapabilityComposition composition,
            lux::render::EffectOutputDesc output,
            world::World& world,
            EngineContext& context,
            const RenderCapabilityMetaRegistry& metadata
        ) noexcept;
        [[nodiscard]] world::World& world() noexcept;
        [[nodiscard]] EngineContext& context() noexcept;
        [[nodiscard]] lux::render::SceneId sceneId() const noexcept;
        [[nodiscard]] lux::render::EffectId effectId() const noexcept;
        ~RenderSystem() noexcept override;

        [[nodiscard]] cxx::expected<void, error::Error> execute() noexcept override;

    protected:
        [[nodiscard]] exec::task<SceneSystem::Result> initialize() noexcept override;
        [[nodiscard]] exec::task<SceneSystem::Result> releaseResources() noexcept override;

    private:
        friend class RenderReaction;
        struct Impl;
        explicit RenderSystem(std::unique_ptr<Impl>) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::engine::render
