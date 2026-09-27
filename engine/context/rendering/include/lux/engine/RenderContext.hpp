#pragma once

#include <lux/engine/render/RendererConfig.hpp>

namespace lux::process
{
    class ExecutionRuntime;
    enum class EExecutionError : std::uint8_t;
}
namespace lux::render
{
    class RenderRuntime;
}
namespace lux::scene
{
    class RenderResources;
}
namespace lux::engine
{
    class EngineContext;

    class RenderContext final
    {
    public:
        using VFailure = std::variant<render::RendererFailure, process::EExecutionError>;
        using Result = lux::cxx::expected<void, VFailure>;

        ~RenderContext() noexcept;
        [[nodiscard]] render::RenderRuntime& runtime() noexcept;
        [[nodiscard]] scene::RenderResources& resources() noexcept;
        // Cold owner-thread assembly. Only transport/resource completions run while waiting.
        [[nodiscard]] Result registerFeatures(std::vector<render::RenderFeatureRegistration>) noexcept;

    private:
        friend Result initializeRendering(EngineContext&, std::span<const char* const>) noexcept;
        struct Impl;
        explicit RenderContext(std::unique_ptr<Impl>) noexcept;
        using CreateResult = lux::cxx::expected<std::unique_ptr<RenderContext>, VFailure>;
        [[nodiscard]] static CreateResult create(process::ExecutionRuntime&, render::RendererConfig) noexcept;
        std::unique_ptr<Impl> impl_;
    };
}
