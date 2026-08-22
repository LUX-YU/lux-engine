#pragma once

#include <lux/engine/ecs/systems/ISystem.hpp>
#include <lux/engine/ecs/render/SceneRenderBinding.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace lux::ecs
{
    class PixelFieldRuntime;
    class PixelField2DSystemImplementation;

    /// Pixel field ECS-to-Canvas2D extraction. The retained atlas, request and
    /// retirement machinery is private to the compiled integration target.
    class LUX_FUNCTION_PUBLIC PixelField2DSystem final : public ISystem
    {
    public:
        PixelField2DSystem(
            SceneRenderBinding& render,
            PixelFieldRuntime* runtime);
        ~PixelField2DSystem() override;

        PixelField2DSystem(const PixelField2DSystem&) = delete;
        PixelField2DSystem& operator=(const PixelField2DSystem&) = delete;

        [[nodiscard]] static std::span<const std::string_view>
        requiredRenderFeatures() noexcept;
        void onAdded(const SystemSetupContext& setup) override;
        void onRemoved(const SystemRemovalContext& context) override;
        void update(const SystemUpdateContext& context) override;
        void requestClose() noexcept override;
        [[nodiscard]] std::span<const Type> runsAfter() const noexcept override;

        [[nodiscard]] std::uint32_t residentChunks() const noexcept;
        [[nodiscard]] std::uint32_t freeSlots() const noexcept;
        [[nodiscard]] std::uint64_t slotProtocolErrors() const noexcept;

    private:
        SceneRenderBinding* render_{nullptr};
        std::unique_ptr<PixelField2DSystemImplementation> impl_;
    };
} // namespace lux::ecs
