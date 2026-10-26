#pragma once

#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/engine/scene/render/RenderCapabilityOperation.hpp>
#include <lux/engine/scene/render/visibility.h>

namespace lux::scene
{
    enum class ERenderBindingDeclarationError : std::uint8_t
    {
        INVALID_DECLARATION,
        DUPLICATE_TYPE,
        DUPLICATE_NAME
    };

    using RenderBindingRegistrationResult = cxx::expected<void, ERenderBindingDeclarationError>;

    namespace detail
    {
        // The holder address, not its pointee, identifies one EnTT subscription owner.
        using RenderBindingObject = std::unique_ptr<void, void (*)(void*)>;
    } // namespace detail

    struct RenderBindingDeclaration final
    {
        std::shared_ptr<const void> code_lifetime;
        std::string name;
        std::string role;
        std::uint32_t version = 1;
        cxx::TypeToken type;
        std::vector<cxx::TypeToken> sources;
        std::vector<cxx::TypeToken> results;
        std::vector<cxx::TypeToken> dependencies;
        std::vector<cxx::TypeToken> aggregate_dependencies;
        std::vector<render::RenderPayloadContract> inputs;
        // Optional cold selection. Empty capability names use exact payload matching.
        std::vector<std::string> capabilities;
        // Only inputs borrowed by this factory. Other payload contracts qualify capability
        // selection; dependent operations borrow their producer's already resolved input.
        std::vector<RenderInputSelection> resource_inputs;
        std::vector<system::SystemDataContract> data_inputs;
        bool each_frame = false;
        // Result records separate every active role/View instance; default components have one writer.
        bool partitioned_results = false;
        ERenderOperationScope scope = ERenderOperationScope::SCENE;
        std::uint32_t role_index = 0;
        bool view_provider = false;
        std::uint64_t (*effect_revision)(void*) noexcept {};
        exec::task<render::RenderResult<void>> (*prepare_resources)(engine::render::RenderAssets&) noexcept {};
        render::RenderResult<
            render::ConfigValue> (*effect_inputs)(void*, cxx::TypeToken, std::span<const std::byte>) noexcept {};
        render::RenderResult<void*> (*create)(RenderBindingContext&) noexcept {};
        void (*destroy)(void*) noexcept {};
        void (*connect)(
            detail::RenderBindingObject&,
            simulation::ecs::Registry&,
            std::vector<entt::scoped_connection>&
        ) noexcept {};
        SceneStageResult (*maintain)(void*, SceneStageContext&) noexcept {};
        SceneStageResult (*prepare)(void*, SceneStageContext&) noexcept {};
        SceneStageResult (*publish)(void*, SceneStageContext&, engine::render::FrameInput&) noexcept {};
    };

    class LUX_ENGINE_SCENE_RENDER_PUBLIC RenderBindingDeclarations final
    {
    public:
        // Registration is cold. Each Scene Plan copies its selected descriptions, so declaration
        // growth cannot invalidate live callbacks. Implementation code must outlive all objects.
        [[nodiscard]] RenderBindingRegistrationResult add(RenderBindingDeclaration) noexcept;
        [[nodiscard]] std::span<const RenderBindingDeclaration> declarations() const noexcept;
        [[nodiscard]] static bool
        matches(const render::RenderPayloadContract&, const render::RenderPayloadContract&) noexcept;

    private:
        std::vector<RenderBindingDeclaration> declarations_;
    };

    namespace detail
    {
        template <class T, class C, ERenderComponentChange Change>
        void renderSourceChanged(
            RenderBindingObject& object,
            simulation::ecs::Registry& registry,
            simulation::ecs::Entity entity
        ) noexcept
        {
            auto& binding = *static_cast<T*>(object.get());
            static_assert(noexcept(binding.template sourceChanged<C>(registry, entity, Change)));
            binding.template sourceChanged<C>(registry, entity, Change);
        }

        template <class T, class C>
        void connectRenderSource(
            RenderBindingObject& object,
            simulation::ecs::Registry& registry,
            std::vector<entt::scoped_connection>& connections
        ) noexcept
        {
            connections.emplace_back(
                registry.on_construct<C>()
                    .template connect<&renderSourceChanged<T, C, ERenderComponentChange::CONSTRUCT>>(object)
            );
            connections.emplace_back(
                registry.on_update<C>().template connect<&renderSourceChanged<T, C, ERenderComponentChange::UPDATE>>(
                    object
                )
            );
            connections.emplace_back(
                registry.on_destroy<C>().template connect<&renderSourceChanged<T, C, ERenderComponentChange::DESTROY>>(
                    object
                )
            );
            for (const auto entity : registry.view<C>())
            {
                renderSourceChanged<T, C, ERenderComponentChange::CONSTRUCT>(object, registry, entity);
            }
        }

        template <class T, class C>
        void renderResultDestroyed(
            RenderBindingObject& object,
            simulation::ecs::Registry& registry,
            simulation::ecs::Entity entity
        ) noexcept
        {
            auto& binding = *static_cast<T*>(object.get());
            static_assert(noexcept(binding.template resultDestroyed<C>(registry, entity)));
            binding.template resultDestroyed<C>(registry, entity);
        }

        template <class T, class... C>
        void
        connectRenderSources(
            RenderBindingObject& object,
            simulation::ecs::Registry& registry,
            std::vector<entt::scoped_connection>& connections,
            std::type_identity<std::tuple<C...>>
        ) noexcept
        {
            (connectRenderSource<T, C>(object, registry, connections), ...);
        }

        template <class T, class... C>
        void
        connectRenderResults(
            RenderBindingObject& object,
            simulation::ecs::Registry& registry,
            std::vector<entt::scoped_connection>& connections,
            std::type_identity<std::tuple<C...>>
        ) noexcept
        {
            (connections.emplace_back(registry.on_destroy<C>().template connect<&renderResultDestroyed<T, C>>(object)),
             ...);
        }

        template <class... C> auto renderSourceTypes(std::type_identity<std::tuple<C...>>) noexcept
        {
            return std::vector<cxx::TypeToken>{cxx::typeToken<C>()...};
        }

        template <RenderComponentBinding T> RenderBindingDeclaration renderBindingDeclaration() noexcept
        {
            RenderBindingDeclaration result;
            result.name = T::name;
            if constexpr (requires { T::version; })
            {
                result.version = T::version;
            }
            result.type = cxx::typeToken<T>();
            result.sources = renderSourceTypes(std::type_identity<typename T::SourceComponents>{});
            result.results = renderSourceTypes(std::type_identity<typename T::ResultComponents>{});
            const auto inputs = T::inputs();
            result.inputs.assign(inputs.begin(), inputs.end());
            if constexpr (requires { T::resourceInputs(); })
            {
                const auto inputs = T::resourceInputs();
                result.resource_inputs.assign(inputs.begin(), inputs.end());
            }
            if constexpr (requires { T::data_inputs; })
            {
                result.data_inputs.assign(std::begin(T::data_inputs), std::end(T::data_inputs));
            }
            result.create = +[](RenderBindingContext& context) noexcept -> render::RenderResult<void*>
            {
                if constexpr (requires { T::create(context); })
                {
                    auto made = T::create(context);
                    if (!made)
                    {
                        return cxx::unexpected(made.error());
                    }
                    return made->release();
                }
                else
                {
                    static T function;
                    return &function;
                }
            };
            result.destroy = +[](void* object) noexcept
            {
                if constexpr (requires(RenderBindingContext& context) { T::create(context); })
                {
                    delete static_cast<T*>(object);
                }
            };
            result.connect = +[](RenderBindingObject& object,
                                 simulation::ecs::Registry& registry,
                                 std::vector<entt::scoped_connection>& connections) noexcept
            {
                connectRenderResults<T>(
                    object,
                    registry,
                    connections,
                    std::type_identity<typename T::ResultComponents>{}
                );
                connectRenderSources<T>(
                    object,
                    registry,
                    connections,
                    std::type_identity<typename T::SourceComponents>{}
                );
            };
            if constexpr (requires(T& object, SceneStageContext& context) {
                              { object.maintain(context) } noexcept -> std::same_as<SceneStageResult>;
                          })
            {
                result.maintain = +[](void* object, SceneStageContext& context) noexcept
                { return static_cast<T*>(object)->maintain(context); };
            }
            if constexpr (requires(T& object, SceneStageContext& context) {
                              { object.prepare(context) } noexcept -> std::same_as<SceneStageResult>;
                          })
            {
                result.prepare = +[](void* object, SceneStageContext& context) noexcept
                { return static_cast<T*>(object)->prepare(context); };
            }
            if constexpr (requires(T& object, SceneStageContext& context, engine::render::FrameInput& frame) {
                              { object.publish(context, frame) } noexcept -> std::same_as<SceneStageResult>;
                          })
            {
                result.publish = +[](void* object, SceneStageContext& context, engine::render::FrameInput& frame
                                  ) noexcept { return static_cast<T*>(object)->publish(context, frame); };
            }
            if constexpr (requires { T::scope; })
            {
                result.scope = T::scope;
            }
            if constexpr (requires { T::partitioned_results; })
            {
                result.partitioned_results = T::partitioned_results;
            }
            if constexpr (requires { T::each_frame; })
            {
                result.each_frame = T::each_frame;
            }
            if constexpr (requires { T::view_provider; })
            {
                result.view_provider = T::view_provider;
            }
            if constexpr (requires { typename T::Dependencies; })
            {
                result.dependencies = renderSourceTypes(std::type_identity<typename T::Dependencies>{});
            }
            if constexpr (requires { typename T::AggregateDependencies; })
            {
                result.aggregate_dependencies =
                    renderSourceTypes(std::type_identity<typename T::AggregateDependencies>{});
            }
            if constexpr (requires(engine::render::RenderAssets& assets) { T::prepareResources(assets); })
            {
                result.prepare_resources = &T::prepareResources;
            }
            if constexpr (requires(T& object, cxx::TypeToken type, std::span<const std::byte> bytes) {
                              {
                                  object.effectInputs(type, bytes)
                              } noexcept -> std::same_as<render::RenderResult<render::ConfigValue>>;
                          })
            {
                result.effect_inputs = +[](void* object, cxx::TypeToken type, std::span<const std::byte> bytes) noexcept
                { return static_cast<T*>(object)->effectInputs(type, bytes); };
            }
            if constexpr (requires(T& object) {
                              { object.effectRevision() } noexcept -> std::same_as<std::uint64_t>;
                          })
            {
                result.effect_revision =
                    +[](void* object) noexcept { return static_cast<T*>(object)->effectRevision(); };
            }
            return result;
        }
    } // namespace detail

    template <RenderComponentBinding T>
    [[nodiscard]] RenderBindingRegistrationResult registerComponentBinding(
        RenderBindingDeclarations& declarations,
        std::shared_ptr<const void> code_lifetime = {},
        std::string_view role = {},
        std::span<const RenderInputSelection> resource_inputs = {},
        std::span<const std::string_view> capabilities = {}
    ) noexcept
    {
        auto declaration = detail::renderBindingDeclaration<T>();
        declaration.code_lifetime = std::move(code_lifetime);
        declaration.role = role.empty() ? declaration.name : role;
        if (!resource_inputs.empty())
        {
            declaration.resource_inputs.assign(resource_inputs.begin(), resource_inputs.end());
        }
        declaration.capabilities.assign(capabilities.begin(), capabilities.end());
        return declarations.add(std::move(declaration));
    }

    template <RenderCapabilityOperation T>
    [[nodiscard]] RenderBindingRegistrationResult registerCapabilityOperation(
        RenderBindingDeclarations& declarations,
        std::shared_ptr<const void> code_lifetime = {},
        std::string_view role = {},
        std::span<const RenderInputSelection> resource_inputs = {},
        std::span<const std::string_view> capabilities = {}
    ) noexcept
    {
        auto declaration = detail::renderBindingDeclaration<T>();
        declaration.code_lifetime = std::move(code_lifetime);
        declaration.role = role.empty() ? declaration.name : role;
        if (!resource_inputs.empty())
        {
            declaration.resource_inputs.assign(resource_inputs.begin(), resource_inputs.end());
        }
        declaration.capabilities.assign(capabilities.begin(), capabilities.end());
        declaration.scope = T::scope;
        return declarations.add(std::move(declaration));
    }
} // namespace lux::scene
