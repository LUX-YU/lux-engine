#pragma once

#include <concepts>
#include <exec/task.hpp>
#include <lux/engine/process/asset_loading/AssetRead.hpp>
#include <lux/engine/render/capabilities/RenderCapability.hpp>
#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/scene/RenderAssetInput.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>
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
    class RenderAssets;
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
        std::span<const render::RenderPayloadContract> contracts;
        engine::render::RenderAssets& assets;
        RenderAssetInput& asset_input;
        process::asset_loading::AssetReadPort source;
        render::SceneId scene;
        render::CapabilitySelection implementation;
        const RenderSystemConfiguration& configuration;
        // One Scene input revision, not a second resource identity or lifetime owner.
        std::uint64_t& input_revision;
        std::uint64_t& adopted_input_revision;
        void* lookup_context;
        void* (*lookup)(void*, cxx::TypeToken, std::string_view) noexcept;
        void* effect_input_object = nullptr;
        render::RenderResult<
            render::ConfigValue> (*effect_inputs)(void*, cxx::TypeToken, std::span<const std::byte>) noexcept = nullptr;
        std::uint64_t (*effect_revision)(void*) noexcept = nullptr;
        std::string_view role;

        template <class T> [[nodiscard]] T* find(std::string_view selected_role = {}) const noexcept
        {
            return static_cast<T*>(lookup(lookup_context, cxx::typeToken<T>(), selected_role));
        }
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
