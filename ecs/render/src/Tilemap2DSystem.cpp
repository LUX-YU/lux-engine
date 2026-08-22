#include <lux/engine/ecs/render/systems/2d/Tilemap2DSystem.hpp>

#include <lux/engine/ecs/render/systems/2d/Tilemap2DSystemImplementation.hpp>
#include <lux/engine/ecs/render/systems/RenderSystem.hpp>

#include <array>

namespace lux::ecs
{
    Tilemap2DSystem::Tilemap2DSystem(
        SceneRenderBinding& render,
        TilemapRuntime* runtime)
        : render_(&render),
          impl_(std::make_unique<Tilemap2DSystemImplementation>(runtime))
    {}

    Tilemap2DSystem::~Tilemap2DSystem() = default;

    std::span<const std::string_view>
    Tilemap2DSystem::requiredRenderFeatures() noexcept
    {
        static constexpr std::array<std::string_view, 1u> features{
            "Canvas2D"};
        return features;
    }

    void Tilemap2DSystem::onAdded(const SystemSetupContext& setup)
    {
        impl_->onAdded(setup);
    }

    void Tilemap2DSystem::onRemoved(
        const SystemRemovalContext& context)
    {
        impl_->onRemoved(context);
    }

    void Tilemap2DSystem::update(const SystemUpdateContext& context)
    {
        impl_->update(context.registry(), *render_);
    }

    void Tilemap2DSystem::requestClose() noexcept
    {
        impl_->releaseOwned(*render_);
    }

    std::span<const ISystem::Type>
    Tilemap2DSystem::runsAfter() const noexcept
    {
        static constexpr std::array<Type, 1u> dependencies{
            systemType<RenderSystem>()};
        return dependencies;
    }

    std::uint32_t Tilemap2DSystem::residentChunks() const noexcept
    {
        return impl_->residentChunks();
    }

    std::uint32_t Tilemap2DSystem::freeSlots() const noexcept
    {
        return impl_->freeSlots();
    }
} // namespace lux::ecs
