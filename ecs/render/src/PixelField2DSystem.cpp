#include <lux/engine/ecs/render/systems/2d/PixelField2DSystem.hpp>

#include <lux/engine/ecs/render/systems/2d/PixelField2DSystemImplementation.hpp>
#include <lux/engine/ecs/render/systems/RenderSystem.hpp>

#include <array>

namespace lux::ecs
{
    PixelField2DSystem::PixelField2DSystem(
        SceneRenderBinding& render,
        PixelFieldRuntime* runtime)
        : render_(&render),
          impl_(std::make_unique<PixelField2DSystemImplementation>(runtime))
    {}

    PixelField2DSystem::~PixelField2DSystem() = default;

    std::span<const std::string_view>
    PixelField2DSystem::requiredRenderFeatures() noexcept
    {
        static constexpr std::array<std::string_view, 1u> features{
            "Canvas2D"};
        return features;
    }

    void PixelField2DSystem::onAdded(const SystemSetupContext& setup)
    {
        impl_->onAdded(setup);
    }

    void PixelField2DSystem::onRemoved(
        const SystemRemovalContext& context)
    {
        impl_->onRemoved(context);
    }

    void PixelField2DSystem::update(const SystemUpdateContext& context)
    {
        impl_->update(context.registry(), *render_);
    }

    void PixelField2DSystem::requestClose() noexcept
    {
        impl_->releaseOwned(*render_);
    }

    std::span<const ISystem::Type>
    PixelField2DSystem::runsAfter() const noexcept
    {
        static constexpr std::array<Type, 1u> dependencies{
            systemType<RenderSystem>()};
        return dependencies;
    }

    std::uint32_t PixelField2DSystem::residentChunks() const noexcept
    {
        return impl_->residentChunks();
    }

    std::uint32_t PixelField2DSystem::freeSlots() const noexcept
    {
        return impl_->freeSlots();
    }

    std::uint64_t PixelField2DSystem::slotProtocolErrors() const noexcept
    {
        return impl_->slotProtocolErrors();
    }
} // namespace lux::ecs
