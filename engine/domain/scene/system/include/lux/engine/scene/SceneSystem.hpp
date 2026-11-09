#pragma once

#include <exec/task.hpp>
#include <lux/engine/core/execution/TaskScope.hpp>
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/ecs/EcsCommandBuffer.hpp>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/domain/scene/system/visibility.h>
#include <lux/engine/error/Error.hpp>

namespace lux::scene
{
    enum class ESceneSystemStage
    {
        BEFORE_SIMULATION,
        AFTER_SIMULATION
    };

    // SceneInstance owns the object. Systems release their resources through RAII.
    class LUX_ENGINE_SCENE_SYSTEM_PUBLIC SceneSystem : public object::LuxObject
    {
    public:
        using Result = cxx::expected<void, error::Error>;

        // Main-thread work at the selected stage; equal stages preserve assembly order.
        [[nodiscard]] virtual cxx::expected<void, error::Error> execute() noexcept = 0;
        [[nodiscard]] virtual ESceneSystemStage stage() const noexcept;
        [[nodiscard]] virtual ecs::EcsCommandProducerCapacity commandCapacity() const noexcept;

        // Disabling execution does not destroy the system or stop shared services.
        void setEnable(bool enabled) noexcept;
        [[nodiscard]] bool isEnabled() const noexcept;

    protected:
        // Recording never applies a command inline. No Writer may survive suspension or execution return.
        [[nodiscard]] cxx::expected<ecs::EcsCommandWriter, ecs::EcsCommandFailure>
        beginCommands(ecs::EEcsCommandPolicy policy = ecs::EEcsCommandPolicy::ABORT_BATCH) noexcept;
        [[nodiscard]] std::optional<ecs::Entity> resolveEntity(ecs::DeferredEntity entity) const noexcept;
        // Borrowed from SceneSession after Engine adoption; never stop or join it from a System.
        [[nodiscard]] execution::TaskScope& taskScope() const noexcept;
        [[nodiscard]] virtual exec::task<Result> initialize() noexcept;
        // Only professional resource release. Engine has already settled the Scene's tasks.
        [[nodiscard]] virtual exec::task<Result> releaseResources() noexcept;

    private:
        friend class SceneInstance;
        execution::TaskScope* tasks_ = nullptr;
        ecs::EcsCommandBuffer* commands_ = nullptr;
        std::size_t command_producer_ = SIZE_MAX;
        bool enabled_ = true;
    };
} // namespace lux::scene
