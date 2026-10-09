#pragma once

#include <lux/engine/ContextExtensions.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/resource/asset/storage/AssetVfs.hpp>
#include <lux/engine/scene/SceneRuntime.hpp>

namespace lux::engine
{
    class RenderContext;

    namespace detail
    {
        struct EngineRenderAccess;
    }

    // Application facilities. Hosts decide execution, scene time and explicit shutdown order.
    class EngineContext final
    {
    public:
        using VCreateFailure = std::variant<process::EExecutionError, scene::SceneRuntimeFailure>;
        using CreateResult = lux::cxx::expected<std::unique_ptr<EngineContext>, VCreateFailure>;
        [[nodiscard]] static CreateResult create(
            process::ExecutionRuntimeConfig,
            task::TaskExecutorConfig,
            ContextExtensions::Composition = {}
        ) noexcept;
        ~EngineContext();
        EngineContext(const EngineContext&) = delete;
        EngineContext& operator=(const EngineContext&) = delete;

        [[nodiscard]] process::ExecutionRuntime& execution() noexcept
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

        [[nodiscard]] RenderContext* renderContext() noexcept
        {
            return rendering_.get();
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
        friend struct detail::EngineRenderAccess;
        EngineContext(process::ExecutionRuntime&&, ContextExtensions::Composition&&);
        using RenderOwner = std::unique_ptr<RenderContext, void (*)(RenderContext*) noexcept>;
        asset::AssetVfs assets_;
        process::ExecutionRuntime execution_;
        RenderOwner rendering_{nullptr, nullptr};
        std::unique_ptr<scene::SceneRuntime> scenes_;
        ContextExtensions extensions_;
    };
} // namespace lux::engine
