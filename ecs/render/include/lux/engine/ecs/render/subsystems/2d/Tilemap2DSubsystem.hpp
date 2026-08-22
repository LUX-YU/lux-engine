#pragma once

#include <lux/engine/ecs/render/RenderStage.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace lux::ecs
{
    class TilemapRuntime;
    class Tilemap2DSubsystemImplementation;

    /// Tilemap ECS-to-Canvas2D extraction. Sparse atlas and request state stay
    /// private to the compiled integration target.
    class LUX_FUNCTION_PUBLIC Tilemap2DSubsystem final
        : public RenderStage
    {
    public:
        explicit Tilemap2DSubsystem(TilemapRuntime* runtime);
        ~Tilemap2DSubsystem() override;

        Tilemap2DSubsystem(const Tilemap2DSubsystem&) = delete;
        Tilemap2DSubsystem& operator=(const Tilemap2DSubsystem&) = delete;

        [[nodiscard]] std::span<const std::string_view>
        requiredFeatures() const noexcept override;
        void onAdded(const SystemSetupContext& setup) override;
        void onRemoved(const SystemRemovalContext& context) override;
        void extract(RenderSubsystemContext& context) override;
        void close(RenderSubsystemContext& context) noexcept override;

        [[nodiscard]] std::uint32_t residentChunks() const noexcept;
        [[nodiscard]] std::uint32_t freeSlots() const noexcept;

    private:
        std::unique_ptr<Tilemap2DSubsystemImplementation> impl_;
    };
} // namespace lux::ecs
