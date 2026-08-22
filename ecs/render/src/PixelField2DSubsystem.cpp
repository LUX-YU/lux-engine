#include <lux/engine/ecs/render/subsystems/2d/PixelField2DSubsystem.hpp>

#include <lux/engine/ecs/render/subsystems/2d/PixelField2DSubsystemImplementation.hpp>

namespace lux::ecs
{
    PixelField2DSubsystem::PixelField2DSubsystem(PixelFieldRuntime* runtime)
        : impl_(std::make_unique<PixelField2DSubsystemImplementation>(runtime))
    {}

    PixelField2DSubsystem::~PixelField2DSubsystem() = default;

    std::span<const std::string_view>
    PixelField2DSubsystem::requiredFeatures() const noexcept
    {
        return impl_->requiredFeatures();
    }

    void PixelField2DSubsystem::onAdded(const SystemSetupContext& setup)
    {
        impl_->onAdded(setup);
    }

    void PixelField2DSubsystem::onRemoved(
        const SystemRemovalContext& context)
    {
        impl_->onRemoved(context);
    }

    void PixelField2DSubsystem::extract(RenderSubsystemContext& context)
    {
        impl_->extract(context);
    }

    void PixelField2DSubsystem::close(
        RenderSubsystemContext& context) noexcept
    {
        impl_->close(context);
    }

    std::uint32_t PixelField2DSubsystem::residentChunks() const noexcept
    {
        return impl_->residentChunks();
    }

    std::uint32_t PixelField2DSubsystem::freeSlots() const noexcept
    {
        return impl_->freeSlots();
    }

    std::uint64_t PixelField2DSubsystem::slotProtocolErrors() const noexcept
    {
        return impl_->slotProtocolErrors();
    }
} // namespace lux::ecs
