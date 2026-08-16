#include <lux/engine/runtime/scene/script/SceneScriptRuntime.hpp>

#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/ecs/SceneServices.hpp>
#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/script/systems/ScriptRegistry.hpp>
#include <lux/engine/ecs/script/systems/ScriptSystem.hpp>
#include <lux/engine/input/ActionMapper.hpp>
#include <lux/engine/log/Log.hpp>

#include <memory>
#include <utility>

namespace lux::runtime
{
    SceneScriptRuntime::SceneScriptRuntime(
        lux::ecs::World& world,
        lux::ecs::Schedule& schedule,
        lux::ecs::SceneServices& services,
        lux::asset::AssetManager& assets) noexcept
        : world_(&world)
        , schedule_(&schedule)
        , services_(&services)
        , assets_(&assets)
    {}

    SceneScriptRuntime::~SceneScriptRuntime() noexcept
    {
        (void)stop();
    }

    bool SceneScriptRuntime::start(
        const lux::input::ActionMapper& mapper,
        const lux::input::InputActionRegistry* actions)
    {
        if (!world_ || !schedule_ || !services_ || !assets_ || system_)
            return false;

        auto script = std::make_unique<lux::ecs::ScriptSystem>(
            lux::ecs::scriptRegistry(),
            lux::ecs::ScriptContext{world_, &mapper, actions, assets_});
        lux::ecs::ScheduleBuilder builder{*schedule_, *services_};
        auto pending = builder.add(
            std::move(script),
            lux::ecs::kPhaseInput);
        if (!pending)
        {
            lux::log::error(
                "scene.script",
                "failed to stage script system: {}",
                lux::ecs::toString(pending.error()));
            return false;
        }

        const auto committed = builder.commit();
        if (!committed)
        {
            const auto& failure = committed.error();
            lux::log::error(
                "scene.script",
                "script topology rejected: {} (subject '{}', detail '{}')",
                lux::ecs::toString(failure.error),
                failure.subject,
                failure.detail);
            return false;
        }

        system_ = builder.handle(*pending);
        schedule_->get(system_)->onRuntimeStart(world_->registry());
        return true;
    }

    bool SceneScriptRuntime::stop() noexcept
    {
        if (!system_)
            return true;
        if (!schedule_)
            return false;

        const auto removed = schedule_->removeSystem(system_);
        if (!removed)
        {
            lux::log::error(
                "scene.script",
                "failed to remove script system: {}",
                lux::ecs::toString(removed.error()));
            return false;
        }
        system_ = {};
        return true;
    }

    bool SceneScriptRuntime::active() const noexcept
    {
        return static_cast<bool>(system_);
    }

    lux::ecs::ScriptSystem* SceneScriptRuntime::system() noexcept
    {
        return schedule_ ? schedule_->get(system_) : nullptr;
    }
}
