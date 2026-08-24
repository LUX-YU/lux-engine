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
    class TilemapRuntime;
    class Tilemap2DSystemImplementation;

    /// Tilemap ECS-to-Canvas2D extraction. Sparse atlas and request state stay
    /// private to the compiled integration target.
    class LUX_FUNCTION_PUBLIC Tilemap2DSystem final : public ISystem
    {
    public:
        Tilemap2DSystem(
            SceneRenderBinding& render,
            TilemapRuntime* runtime);
        ~Tilemap2DSystem() override;

        Tilemap2DSystem(const Tilemap2DSystem&) = delete;
        Tilemap2DSystem& operator=(const Tilemap2DSystem&) = delete;

        [[nodiscard]] static std::span<const std::string_view>
        requiredRenderFeatures() noexcept;
        void onAdded(const SystemSetupContext& setup) override;
        void onRemoved(const SystemRemovalContext& context) override;
        void update(const SystemUpdateContext& context) override;
        void requestClose() noexcept override;
        [[nodiscard]] std::span<const Type> runsAfter() const noexcept override;

        [[nodiscard]] std::uint32_t residentChunks() const noexcept;
        [[nodiscard]] std::uint32_t freeSlots() const noexcept;

    private:
        SceneRenderBinding* render_{nullptr};
        std::unique_ptr<Tilemap2DSystemImplementation> impl_;
    };
} // namespace lux::ecs
