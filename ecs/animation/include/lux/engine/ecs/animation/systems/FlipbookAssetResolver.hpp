#pragma once

#include <lux/engine/ecs/animation/systems/FlipbookAnimationSystem.hpp>
#include <lux/engine/ecs/systems/ISystem.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/resource/asset/AssetLoadPort.hpp>

namespace lux::asset { class AssetManager; }

namespace lux::ecs
{
    class LUX_FUNCTION_PUBLIC FlipbookAssetResolver final : public ISystem
    {
    public:
        FlipbookAssetResolver(
            lux::asset::AssetManager& manager,
            lux::asset_runtime::AssetClient client) noexcept;
        void update(const SystemUpdateContext& context) override;
        [[nodiscard]] std::span<const SystemType> runsBefore()
            const noexcept override
        {
            static constexpr SystemType kBefore[]{
                systemType<FlipbookAnimationSystem>()};
            return kBefore;
        }
    private:
        lux::asset::AssetManager* manager_{};
        lux::asset_runtime::AssetClient client_;
    };
}
