#pragma once

#include <lux/engine/ecs/render/IRenderSubsystem.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace lux::ecs
{
    class PixelFieldRuntime;
    class PixelField2DSubsystemImplementation;

    /// Pixel field ECS-to-Canvas2D extraction. The retained atlas, request and
    /// retirement machinery is private to the compiled integration target.
    class LUX_FUNCTION_PUBLIC PixelField2DSubsystem final
        : public IRenderSubsystem
    {
    public:
        explicit PixelField2DSubsystem(PixelFieldRuntime* runtime);
        ~PixelField2DSubsystem() override;

        PixelField2DSubsystem(const PixelField2DSubsystem&) = delete;
        PixelField2DSubsystem& operator=(const PixelField2DSubsystem&) = delete;

        [[nodiscard]] std::span<const std::string_view>
        renderFeatures() const noexcept override;
        [[nodiscard]] std::span<const RenderSubsystemType>
        runsAfter() const noexcept override;
        void onAdded(const SystemSetupContext& setup) override;
        void onRemoved(const SystemRemovalContext& context) override;
        void update(RenderSubsystemContext& context) override;
        void close(RenderSubsystemContext& context) noexcept override;

        [[nodiscard]] std::uint32_t residentChunks() const noexcept;
        [[nodiscard]] std::uint32_t freeSlots() const noexcept;
        [[nodiscard]] std::uint64_t slotProtocolErrors() const noexcept;

    private:
        std::unique_ptr<PixelField2DSubsystemImplementation> impl_;
    };
} // namespace lux::ecs
