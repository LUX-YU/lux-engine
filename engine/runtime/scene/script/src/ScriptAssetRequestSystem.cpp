#include <lux/engine/runtime/scene/script/ScriptAssetRequestSystem.hpp>

#include <lux/engine/ecs/script/components/ScriptComponent.hpp>
#include <lux/engine/resource/asset/script/ScriptAsset.hpp>

#include <utility>

namespace lux::runtime
{
    ScriptAssetRequestSystem::ScriptAssetRequestSystem(
        lux::asset::AssetManager& manager,
        lux::asset_runtime::AssetClient client) noexcept
        : manager_(&manager)
        , client_(std::move(client))
    {}

    void ScriptAssetRequestSystem::update(
        const lux::ecs::SystemUpdateContext& context)
    {
        context.registry().view<lux::ecs::ScriptComponent>().each(
            [this](lux::ecs::ScriptComponent& script)
            {
                if (!script.enabled || script.instance.created ||
                    script.script.is_nil())
                {
                    return;
                }
                if (script.instance.ref.id() != script.script)
                    script.instance.ref = manager_->acquire(script.script);

                const auto* asset = manager_->fetchAssetAs<
                    lux::asset::ScriptAsset>(script.script);
                if (asset == nullptr || asset->data() == nullptr)
                    static_cast<void>(client_.request(script.script));
            });
    }
}
