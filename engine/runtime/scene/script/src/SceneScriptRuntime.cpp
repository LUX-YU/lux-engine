#include <lux/engine/runtime/scene/script/SceneScriptRuntime.hpp>
#include <lux/engine/runtime/scene/script/ScriptAssetRequestSystem.hpp>

#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/ecs/SceneServices.hpp>
#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/script/systems/ScriptRegistry.hpp>
#include <lux/engine/ecs/script/systems/ScriptBehavior.hpp>
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
        lux::asset::AssetManager& assets,
        lux::asset_runtime::AssetClient asset_client) noexcept
        : world_(&world)
        , schedule_(&schedule)
        , services_(&services)
        , assets_(&assets)
        , asset_client_(std::move(asset_client))
    {}

    SceneScriptRuntime::~SceneScriptRuntime() noexcept
    {
        (void)stop();
    }

    bool SceneScriptRuntime::addBackend(
        std::unique_ptr<lux::ecs::IScriptBackend> backend
    )
    {
        if (!backend || active())
            return false;
        for (const auto& current : backends_)
            if (current->kind() == backend->kind())
                return false;
        backends_.push_back(std::move(backend));
        return true;
    }

    bool SceneScriptRuntime::start(
        const lux::input::ActionMapper& mapper,
        const lux::input::InputActionRegistry* actions)
    {
        if (!world_ || !schedule_ || !services_ || !assets_ ||
            !asset_client_ || request_system_ || system_)
            return false;

        auto request = std::make_unique<ScriptAssetRequestSystem>(
            *assets_,
            asset_client_
        );
        std::vector<lux::ecs::IScriptBackend*> backend_views;
        backend_views.reserve(backends_.size());
        for (const auto& backend : backends_)
            backend_views.push_back(backend.get());
        auto script = std::make_unique<lux::ecs::ScriptSystem>(
            lux::ecs::scriptRegistry(),
            lux::ecs::ScriptContext{world_, &mapper, actions, assets_},
            std::move(backend_views));
        lux::ecs::ScheduleBuilder builder{*schedule_, *services_};
        auto pending_request = builder.add(
            std::move(request),
            lux::ecs::kPhaseInput);
        if (!pending_request)
        {
            lux::log::error(
                "scene.script",
                "failed to stage script asset request system: {}",
                lux::ecs::toString(pending_request.error()));
            return false;
        }
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

        request_system_ = builder.handle(*pending_request);
        system_ = builder.handle(*pending);
        schedule_->get(system_)->onRuntimeStart(world_->registry());
        return true;
    }

    bool SceneScriptRuntime::stop() noexcept
    {
        if (!system_ && !request_system_)
            return true;
        if (!schedule_)
            return false;

        if (system_)
        {
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
        }

        if (request_system_)
        {
            const auto request_removed =
                schedule_->removeSystem(request_system_);
            if (!request_removed)
            {
                lux::log::error(
                    "scene.script",
                    "failed to remove script asset request system: {}",
                    lux::ecs::toString(request_removed.error()));
                return false;
            }
            request_system_ = {};
        }
        for (const auto& backend : backends_)
            backend->resetSession();
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
