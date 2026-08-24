#include <lux/engine/ecs/script/systems/ScriptAssetRequestSystem.hpp>

#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/script/components/ScriptComponent.hpp>
#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/resource/asset/script/ScriptAsset.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>

namespace
{
    [[noreturn]] void fail(const char* message)
    {
        std::cerr << "script_asset_request_system_test: " << message << '\n';
        std::exit(1);
    }

    void expect(bool condition, const char* message)
    {
        if (!condition)
            fail(message);
    }

    std::unique_ptr<lux::asset::ScriptAsset> scriptAsset(
        lux::asset::asset_id_t id)
    {
        auto info = std::make_unique<lux::asset::AssetInfo>();
        info->id = id;
        info->type = lux::asset::EAssetType::SCRIPT;
        auto script = std::make_unique<lux::rdesc::Script>();
        script->module_name = "request.contract";
        return std::make_unique<lux::asset::ScriptAsset>(
            std::move(info),
            std::move(script),
            std::vector<std::byte>{std::byte{'x'}}
        );
    }
}

int main()
{
    lux::asset::AssetManager manager{
        lux::asset::runtimeAssetCodecCatalog()};
    const auto script_id = manager.generateUUID();
    const auto replacement_id = manager.generateUUID();
    expect(manager.registerAsset(scriptAsset(script_id)),
        "script registration failed");
    expect(manager.registerAsset(scriptAsset(replacement_id)),
        "replacement script registration failed");

    lux::ecs::World world;
    const auto active = world.createEntity();
    lux::ecs::ScriptComponent component;
    component.script = script_id;
    world.emplace<lux::ecs::ScriptComponent>(active, std::move(component));

    const auto disabled = world.createEntity();
    lux::ecs::ScriptComponent disabled_component;
    disabled_component.script = replacement_id;
    disabled_component.enabled = false;
    world.emplace<lux::ecs::ScriptComponent>(
        disabled, std::move(disabled_component));

    const auto nil = world.createEntity();
    world.emplace<lux::ecs::ScriptComponent>(
        nil, lux::ecs::ScriptComponent{});

    lux::ecs::ScriptAssetRequestSystem requester{
        manager, lux::asset_runtime::AssetClient{}};
    requester.update({world.registry(), 0.016f});
    auto& active_component = world.get<lux::ecs::ScriptComponent>(active);
    expect(active_component.instance.ref.id() == script_id &&
            manager.isReferenced(script_id),
        "enabled uncreated script did not acquire its residency ticket");
    expect(!manager.isReferenced(replacement_id) &&
            world.get<lux::ecs::ScriptComponent>(disabled)
                .instance.ref.empty() &&
            world.get<lux::ecs::ScriptComponent>(nil).instance.ref.empty(),
        "disabled or nil scripts emitted demand");

    active_component.script = replacement_id;
    requester.update({world.registry(), 0.016f});
    expect(active_component.instance.ref.id() == replacement_id &&
            !manager.isReferenced(script_id) &&
            manager.isReferenced(replacement_id),
        "script ID change did not transfer its residency ticket");

    active_component.instance.created = true;
    active_component.instance.ref.reset();
    requester.update({world.registry(), 0.016f});
    expect(active_component.instance.ref.empty(),
        "created script instance was incorrectly requested again");

    const auto before = requester.runsBefore();
    expect(before.size() == 1u && lux::ecs::sameSystemType(
            before.front(),
            lux::ecs::systemType<lux::ecs::ScriptSystem>()),
        "request system ordering no longer precedes ScriptSystem");
    return 0;
}
