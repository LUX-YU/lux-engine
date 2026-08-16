#include <lux/engine/ecs/render/subsystems/2d/Tilemap2DSubsystem.hpp>

#include <lux/engine/ecs/render/subsystems/2d/Tilemap2DSubsystemImplementation.hpp>

namespace lux::ecs
{
    Tilemap2DSubsystem::Tilemap2DSubsystem(TilemapRuntime* runtime)
        : impl_(std::make_unique<Tilemap2DSubsystemImplementation>(runtime))
    {}

    Tilemap2DSubsystem::~Tilemap2DSubsystem() = default;

    std::span<const std::string_view>
    Tilemap2DSubsystem::renderFeatures() const noexcept
    {
        return impl_->renderFeatures();
    }

    std::span<const RenderSubsystemType>
    Tilemap2DSubsystem::runsAfter() const noexcept
    {
        return impl_->runsAfter();
    }

    void Tilemap2DSubsystem::onAdded(const SystemSetupContext& setup)
    {
        impl_->onAdded(setup);
    }

    void Tilemap2DSubsystem::onRemoved(
        const SystemRemovalContext& context)
    {
        impl_->onRemoved(context);
    }

    void Tilemap2DSubsystem::update(RenderSubsystemContext& context)
    {
        impl_->update(context);
    }

    void Tilemap2DSubsystem::close(
        RenderSubsystemContext& context) noexcept
    {
        impl_->close(context);
    }

    std::uint32_t Tilemap2DSubsystem::residentChunks() const noexcept
    {
        return impl_->residentChunks();
    }

    std::uint32_t Tilemap2DSubsystem::freeSlots() const noexcept
    {
        return impl_->freeSlots();
    }
} // namespace lux::ecs
