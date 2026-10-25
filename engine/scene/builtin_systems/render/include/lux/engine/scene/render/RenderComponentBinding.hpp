#pragma once

#include <concepts>
#include <lux/engine/render/capabilities/RenderCapability.hpp>
#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/scene/SceneSystem.hpp>
#include <lux/engine/simulation/ecs/Registry.hpp>
#include <memory>
#include <tuple>

namespace lux::execution
{
    class ExecutionRuntime;
}

namespace lux::engine::render
{
    class RenderRequester;
    struct FrameInput;
} // namespace lux::engine::render

namespace lux::scene
{
    enum class ERenderComponentChange : std::uint8_t
    {
        CONSTRUCT,
        UPDATE,
        DESTROY
    };

    // Borrowed only during construction. Implementations retain their precise dependencies,
    // never this temporary aggregate or a reference into the installation response.
    struct RenderBindingContext final
    {
        simulation::ecs::Registry& registry;
        engine::render::RenderRequester& client;
        execution::ExecutionRuntime& execution;
        std::span<const render::CapabilitiesInstalled::Input> inputs;
    };

    template <class T>
    concept RenderComponentBinding = requires(RenderBindingContext& context) {
        typename T::SourceComponents;
        typename T::ResultComponents;
        { T::name } -> std::convertible_to<std::string_view>;
        { T::inputs() } noexcept -> std::same_as<std::span<const render::RenderPayloadContract>>;
        { T::create(context) } noexcept -> std::same_as<render::RenderResult<std::unique_ptr<T>>>;
        requires std::is_nothrow_destructible_v<T>;
    };
} // namespace lux::scene
