#include <lux/engine/scene/SceneSystem.hpp>

namespace lux::scene
{
    ESceneSystemStage SceneSystem::stage() const noexcept
    {
        return ESceneSystemStage::AFTER_SIMULATION;
    }

    ecs::EcsCommandProducerCapacity SceneSystem::commandCapacity() const noexcept
    {
        return {0, 0};
    }

    cxx::expected<ecs::EcsCommandWriter, ecs::EcsCommandFailure>
    SceneSystem::beginCommands(ecs::EEcsCommandPolicy policy) noexcept
    {
        if (!commands_ || command_producer_ == SIZE_MAX)
        {
            return cxx::unexpected(ecs::EcsCommandFailure{ecs::EEcsCommandError::INVALID_PRODUCER});
        }
        return commands_->begin(command_producer_, policy);
    }

    std::optional<ecs::Entity> SceneSystem::resolveEntity(ecs::DeferredEntity entity) const noexcept
    {
        if (!commands_ || entity.producer != command_producer_)
        {
            return std::nullopt;
        }
        return commands_->resolve(entity);
    }

    void SceneSystem::setEnable(bool enabled) noexcept
    {
        enabled_ = enabled;
    }

    bool SceneSystem::isEnabled() const noexcept
    {
        return enabled_;
    }

    execution::TaskScope& SceneSystem::taskScope() const noexcept
    {
        if (!tasks_)
        {
            std::terminate(); // Task submission requires Engine adoption.
        }
        return *tasks_;
    }

    exec::task<SceneSystem::Result> SceneSystem::initialize() noexcept
    {
        co_return Result{};
    }

    exec::task<SceneSystem::Result> SceneSystem::releaseResources() noexcept
    {
        co_return Result{};
    }

} // namespace lux::scene
