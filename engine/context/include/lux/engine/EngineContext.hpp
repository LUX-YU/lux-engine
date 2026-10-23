#pragma once

#include <lux/engine/ContextExtensions.hpp>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/resource/asset/storage/AssetVfs.hpp>
#include <lux/engine/scene/SceneRuntime.hpp>

namespace lux::engine
{
    // Application facilities. Hosts decide execution, scene time and explicit shutdown order.
    class EngineContext final
    {
    public:
        using VCreateFailure = std::variant<execution::EExecutionError, scene::SceneRuntimeFailure>;
        using CreateResult = lux::cxx::expected<std::unique_ptr<EngineContext>, VCreateFailure>;
        [[nodiscard]] static CreateResult create(
            execution::ExecutionRuntimeConfig execution_config,
            std::size_t scene_task_capacity,
            ContextExtensions::Composition extensions = {}
        ) noexcept;
        ~EngineContext();
        EngineContext(const EngineContext&) = delete;
        EngineContext& operator=(const EngineContext&) = delete;

        [[nodiscard]] execution::ExecutionRuntime& execution() noexcept
        {
            return execution_;
        }

        [[nodiscard]] scene::SceneRuntime& sceneRuntime() noexcept
        {
            return *scenes_;
        }

        [[nodiscard]] const scene::SceneRuntime& sceneRuntime() const noexcept
        {
            return *scenes_;
        }

        [[nodiscard]] asset::AssetVfs& assets() noexcept
        {
            return assets_;
        }

        [[nodiscard]] ContextExtensions& extensions() noexcept
        {
            return extensions_;
        }

        [[nodiscard]] const ContextExtensions& extensions() const noexcept
        {
            return extensions_;
        }

    private:
        EngineContext(execution::ExecutionRuntime&&, ContextExtensions::Composition&&);
        asset::AssetVfs assets_;
        execution::ExecutionRuntime execution_;
        std::unique_ptr<scene::SceneRuntime> scenes_;
        ContextExtensions extensions_;
    };
} // namespace lux::engine
