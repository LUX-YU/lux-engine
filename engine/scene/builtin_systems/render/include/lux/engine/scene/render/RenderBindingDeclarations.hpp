#pragma once

#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/engine/scene/render/RenderCapabilityOperation.hpp>
#include <unordered_map>

namespace lux::scene
{
    enum class ERenderBindingDeclarationError : std::uint8_t
    {
        INVALID_DECLARATION,
        DUPLICATE_TYPE,
        DUPLICATE_NAME,
        DUPLICATE_RESULT
    };

    using RenderBindingRegistrationResult = cxx::expected<void, ERenderBindingDeclarationError>;

    struct RenderBindingDeclaration final
    {
        std::shared_ptr<const void> code_lifetime;
        std::string name;
        std::uint32_t version = 1;
        cxx::TypeToken type;
        std::vector<cxx::TypeToken> sources;
        std::vector<cxx::TypeToken> results;
        std::vector<render::RenderPayloadContract> inputs;
        std::vector<system::SystemDataContract> data_inputs;
        ERenderUpdatePolicy policy = ERenderUpdatePolicy::STRUCTURE;
        ERenderOperationScope scope = ERenderOperationScope::SCENE;
        render::RenderResult<void*> (*create)(RenderBindingContext&) noexcept {};
        void (*destroy)(void*) noexcept {};
        void (*connect)(void*, simulation::ecs::Registry&, std::vector<entt::scoped_connection>&) noexcept {};
        SceneStageResult (*maintain)(void*, SceneStageContext&) noexcept {};
        SceneStageResult (*prepare)(void*, SceneStageContext&) noexcept {};
        SceneStageResult (*publish)(void*, SceneStageContext&, engine::render::FrameInput&) noexcept {};
        std::uint64_t (*revision)(const void*) noexcept {};
        void (*confirm_inputs)(void*, std::uint64_t) noexcept {};
    };

    class RenderBindingDeclarations final
    {
    public:
        // Registration is cold. Scene objects copy their selected descriptions, so declaration
        // growth cannot invalidate live callbacks. Implementation code must outlive all objects.
        [[nodiscard]] RenderBindingRegistrationResult add(RenderBindingDeclaration declaration) noexcept
        {
            const bool invalid_identity = declaration.version == 0 || declaration.name.empty() ||
                                          !declaration.type.isValid();
            const bool missing_entry = !declaration.create || !declaration.destroy || !declaration.connect;
            const bool invalid = invalid_identity || missing_entry;
            if (invalid)
            {
                return cxx::unexpected(ERenderBindingDeclarationError::INVALID_DECLARATION);
            }
            for (const auto& existing : declarations_)
            {
                const bool duplicate_result = std::ranges::any_of(
                    declaration.results,
                    [&](auto result) noexcept
                    { return std::ranges::find(existing.results, result) != existing.results.end(); }
                );
                if (existing.type == declaration.type)
                {
                    return cxx::unexpected(ERenderBindingDeclarationError::DUPLICATE_TYPE);
                }
                if (existing.name == declaration.name)
                {
                    return cxx::unexpected(ERenderBindingDeclarationError::DUPLICATE_NAME);
                }
                if (duplicate_result)
                {
                    return cxx::unexpected(ERenderBindingDeclarationError::DUPLICATE_RESULT);
                }
            }
            const auto index = declarations_.size();
            for (const auto source : declaration.sources)
            {
                component_bindings_[source.hash()].push_back(index);
            }
            declarations_.push_back(std::move(declaration));
            return {};
        }

        [[nodiscard]] bool supports(const render::RenderPayloadContract& input) const noexcept
        {
            return std::ranges::any_of(
                declarations_,
                [&](const auto& declaration) noexcept
                {
                    return std::ranges::any_of(
                        declaration.inputs,
                        [&](const auto& produced) noexcept { return matches(produced, input); }
                    );
                }
            );
        }

        [[nodiscard]] std::span<const RenderBindingDeclaration> declarations() const noexcept
        {
            return declarations_;
        }

        // Hash only selects a bucket; callers compare each declaration's complete source TypeToken.
        [[nodiscard]] std::span<const std::size_t> affected(cxx::TypeToken component) const noexcept
        {
            const auto found = component_bindings_.find(component.hash());
            return found == component_bindings_.end() ? std::span<const std::size_t>{} : found->second;
        }

        [[nodiscard]] static bool matches(
            const render::RenderPayloadContract& provided,
            const render::RenderPayloadContract& requested
        ) noexcept
        {
            return provided.schema == requested.schema && provided.version == requested.version &&
                   provided.cppType() == requested.cppType() && provided.use == requested.use &&
                   provided.lifetime == requested.lifetime;
        }

    private:
        std::vector<RenderBindingDeclaration> declarations_;
        std::unordered_map<std::uint64_t, std::vector<std::size_t>> component_bindings_;
    };

    namespace detail
    {
        template <class T, class C, ERenderComponentChange Change>
        void renderSourceChanged(
            T& binding,
            simulation::ecs::Registry& registry,
            simulation::ecs::Entity entity
        ) noexcept
        {
            static_assert(noexcept(binding.template sourceChanged<C>(registry, entity, Change)));
            binding.template sourceChanged<C>(registry, entity, Change);
        }

        template <class T, class C>
        void connectRenderSource(
            T& binding,
            simulation::ecs::Registry& registry,
            std::vector<entt::scoped_connection>& connections
        ) noexcept
        {
            connections.emplace_back(
                registry.on_construct<C>()
                    .template connect<&renderSourceChanged<T, C, ERenderComponentChange::CONSTRUCT>>(binding)
            );
            connections.emplace_back(
                registry.on_update<C>().template connect<&renderSourceChanged<T, C, ERenderComponentChange::UPDATE>>(
                    binding
                )
            );
            connections.emplace_back(
                registry.on_destroy<C>().template connect<&renderSourceChanged<T, C, ERenderComponentChange::DESTROY>>(
                    binding
                )
            );
            for (const auto entity : registry.view<C>())
            {
                renderSourceChanged<T, C, ERenderComponentChange::CONSTRUCT>(binding, registry, entity);
            }
        }

        template <class T, class C>
        void renderResultDestroyed(
            T& binding,
            simulation::ecs::Registry& registry,
            simulation::ecs::Entity entity
        ) noexcept
        {
            static_assert(noexcept(binding.template resultDestroyed<C>(registry, entity)));
            binding.template resultDestroyed<C>(registry, entity);
        }

        template <class T, class... C>
        void connectRenderSources(
            T& binding,
            simulation::ecs::Registry& registry,
            std::vector<entt::scoped_connection>& connections,
            std::type_identity<std::tuple<C...>>
        ) noexcept
        {
            (connectRenderSource<T, C>(binding, registry, connections), ...);
        }

        template <class T, class... C>
        void connectRenderResults(
            T& binding,
            simulation::ecs::Registry& registry,
            std::vector<entt::scoped_connection>& connections,
            std::type_identity<std::tuple<C...>>
        ) noexcept
        {
            (connections.emplace_back(registry.on_destroy<C>().template connect<&renderResultDestroyed<T, C>>(binding)),
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
            if constexpr (requires { T::data_inputs; })
            {
                result.data_inputs.assign(std::begin(T::data_inputs), std::end(T::data_inputs));
            }
            result.create = +[](RenderBindingContext& context) noexcept -> render::RenderResult<void*>
            {
                auto made = T::create(context);
                if (!made)
                {
                    return cxx::unexpected(made.error());
                }
                return made->release();
            };
            result.destroy = +[](void* object) noexcept { delete static_cast<T*>(object); };
            result.connect =
                +[](void* object, simulation::ecs::Registry& registry, std::vector<entt::scoped_connection>& connections
                 ) noexcept
            {
                auto& binding = *static_cast<T*>(object);
                connectRenderResults(
                    binding,
                    registry,
                    connections,
                    std::type_identity<typename T::ResultComponents>{}
                );
                connectRenderSources(
                    binding,
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
            if constexpr (requires(const T& object) {
                              { object.revision() } noexcept -> std::same_as<std::uint64_t>;
                          })
            {
                result.revision =
                    +[](const void* object) noexcept { return static_cast<const T*>(object)->revision(); };
            }
            if constexpr (requires(T& object, std::uint64_t revision) {
                              { object.confirmInputs(revision) } noexcept -> std::same_as<void>;
                          })
            {
                result.confirm_inputs = +[](void* object, std::uint64_t revision) noexcept
                { static_cast<T*>(object)->confirmInputs(revision); };
            }
            return result;
        }
    } // namespace detail

    template <RenderComponentBinding T>
    [[nodiscard]] RenderBindingRegistrationResult registerComponentBinding(
        RenderBindingDeclarations& declarations,
        std::shared_ptr<const void> code_lifetime = {}
    ) noexcept
    {
        auto declaration = detail::renderBindingDeclaration<T>();
        declaration.code_lifetime = std::move(code_lifetime);
        return declarations.add(std::move(declaration));
    }

    template <RenderCapabilityOperation T>
    [[nodiscard]] RenderBindingRegistrationResult registerCapabilityOperation(
        RenderBindingDeclarations& declarations,
        std::shared_ptr<const void> code_lifetime = {}
    ) noexcept
    {
        auto declaration = detail::renderBindingDeclaration<T>();
        declaration.code_lifetime = std::move(code_lifetime);
        declaration.policy = T::policy;
        declaration.scope = T::scope;
        return declarations.add(std::move(declaration));
    }
} // namespace lux::scene
