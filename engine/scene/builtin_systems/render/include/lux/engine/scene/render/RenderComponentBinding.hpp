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
        void* (*lookup)(void*, cxx::TypeToken, std::string_view, render::ViewId) noexcept;
        void (*visit)(void*, cxx::TypeToken, render::ViewId, void*, void (*)(void*, void*) noexcept) noexcept;
        // A View provider calls bind_view after accepting its native View, at a Scene structural safe point.
        // Before removing that View, repeat finish_view until COMPLETE; it settles the selected VIEW operations.
        // Borrow these callbacks with lookup_context, not this temporary construction aggregate.
        render::RenderResult<void> (*bind_view)(void*, render::ViewId, simulation::ecs::Entity) noexcept;
        SceneStageResult (*finish_view)(void*, render::ViewId, SceneStageContext&) noexcept;
        void* effect_input_object = nullptr;
        render::RenderResult<
            render::ConfigValue> (*effect_inputs)(void*, cxx::TypeToken, std::span<const std::byte>) noexcept = nullptr;
        std::uint64_t (*effect_revision)(void*) noexcept = nullptr;
        std::string_view role;
        std::uint32_t role_index = 0;
        render::ViewId view;
        simulation::ecs::Entity view_entity = simulation::ecs::NullEntity;

        template <class T> [[nodiscard]] T* find(std::string_view selected_role = {}) const noexcept
        {
            if (selected_role.empty())
            {
                if (auto* same_role = lookup(lookup_context, cxx::typeToken<T>(), role, view))
                {
                    return static_cast<T*>(same_role);
                }
            }
            return static_cast<T*>(lookup(lookup_context, cxx::typeToken<T>(), selected_role, view));
        }

        // Cold aggregation of explicitly declared multi-role dependencies; no new directory or ownership.
        template <class T, class F> void forEach(F&& function) const noexcept
        {
            static_assert(noexcept(function(std::declval<T&>())));
            visit(
                lookup_context,
                cxx::typeToken<T>(),
                view,
                const_cast<void*>(static_cast<const void*>(std::addressof(function))),
                +[](void* callback, void* object) noexcept
                { (*static_cast<std::remove_reference_t<F>*>(callback))(*static_cast<T*>(object)); }
            );
        }
    };

    // Empty function objects can use the same declaration without allocating one object per Scene/View.
    // Anything retaining dependencies must provide its explicit fallible factory.
    template <class T>
    concept StatelessRenderBinding =
        std::is_empty_v<T> && std::is_trivially_destructible_v<T> && std::is_nothrow_default_constructible_v<T>;

    template <class T>
    concept RenderComponentBinding = requires {
        typename T::SourceComponents;
        typename T::ResultComponents;
        { T::name } -> std::convertible_to<std::string_view>;
        { T::inputs() } noexcept -> std::same_as<std::span<const render::RenderPayloadContract>>;
        requires std::is_nothrow_destructible_v<T>;
    } && (StatelessRenderBinding<T> || requires(RenderBindingContext& context) {
                                         {
                                             T::create(context)
                                         } noexcept -> std::same_as<render::RenderResult<std::unique_ptr<T>>>;
                                     });
} // namespace lux::scene
