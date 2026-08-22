#pragma once

#include <lux/engine/ecs/script/systems/ScriptSystem.hpp>
#include <lux/engine/ecs/systems/ISystem.hpp>
#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/runtime/assets/AssetLoadService.hpp>

namespace lux::runtime
{
    class ScriptAssetRequestSystem final : public lux::ecs::ISystem
    {
    public:
        ScriptAssetRequestSystem(
            lux::asset::AssetManager& manager,
            lux::asset_runtime::AssetClient client
        ) noexcept;

        void update(const lux::ecs::SystemUpdateContext& context) override;

        [[nodiscard]] std::span<const lux::ecs::SystemType> runsBefore()
            const noexcept override
        {
            static constexpr lux::ecs::SystemType kBefore[]{
                lux::ecs::systemType<lux::ecs::ScriptSystem>()};
            return kBefore;
        }

    private:
        lux::asset::AssetManager* manager_{};
        lux::asset_runtime::AssetClient client_;
    };
}
