#pragma once

#include <array>
#include <lux/cxx/compile_time/tuple_traits.hpp>
#include <lux/engine/render/extensions/CapabilityRegistry.hpp>
#include <lux/engine/render/extensions/CommandBinding.hpp>
#include <lux/engine/render/extensions/detail/FeatureEntry.hpp>
#include <lux/engine/render/extensions/detail/HandlerRecord.hpp>
#include <lux/engine/render/extensions/features/FeatureGraphOps.hpp>
#include <lux/engine/render/extensions/storage/StorageFactory.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>

namespace lux::render::runtime_detail
{
    template <class Declaration> constexpr cxx::type_info configurationType() noexcept
    {
        if constexpr (std::is_void_v<typename Declaration::Config>)
        {
            return {};
        }
        else
        {
            return cxx::make_basic_type_info<typename Declaration::Config>();
        }
    }

    template <class Declaration> RenderResult<ConfigValue> makeDefaultConfiguration() noexcept
    {
        if constexpr (std::is_void_v<typename Declaration::Config>)
        {
            return ConfigValue{};
        }
        else
        {
            return ConfigValue::make(typename Declaration::Config{});
        }
    }

    // Use type_identity so lux-cxx's tuple utilities never construct a Storage or Handler.
    template <class... Types> constexpr auto uniqueTypes(std::tuple<Types...>*) noexcept
    {
        if constexpr (sizeof...(Types) == 0)
        {
            return std::tuple<>{};
        }
        else
        {
            return typename cxx::remove_repeated_types<std::type_identity<Types>...>::tuple_type{};
        }
    }

    template <class... Methods> constexpr auto handlerTypes(std::tuple<Methods...>*) noexcept
    {
        return uniqueTypes(static_cast<std::tuple<typename Methods::Handler...>*>(nullptr));
    }

    template <class... Methods> auto commandBindings(std::tuple<Methods...>*) noexcept
    {
        // The enclosing implementation is the sole method declaration. Signature qualification
        // belongs to CommandBinding::declare; no second Handler::Bindings list is consulted.
        return []<class... Types>(std::tuple<Types...>) noexcept
        {
            return std::array<CommandBinding, sizeof...(Types)>{CommandBinding::declare<
                typename Types::type::Command,
                Types::type::method,
                typename Types::type::Handler,
                Types::type::storage_dependency>()...};
        }(uniqueTypes(static_cast<std::tuple<Methods...>*>(nullptr)));
    }

    template <class Method> FrameDataCall frameCall() noexcept
    {
        auto call = FrameDataCall::declare<typename Method::Data, Method::method>();
        if constexpr (std::is_lvalue_reference_v<typename Method::Target>)
        {
            call.storage_dependency = Method::dependency;
        }
        else if constexpr (std::same_as<typename Method::Target, FrameDrawTarget>)
        {
            call.graph_feature = Method::dependency;
            call.contribution_key = Method::contribution.view();
            call.pass_key = Method::pass.view();
        }
        return call;
    }

    template <class... Methods> auto frameCalls(std::tuple<Methods...>*) noexcept
    {
        return std::array<FrameDataCall, sizeof...(Methods)>{frameCall<Methods>()...};
    }

    // The same typed metadata construction serves builtin and external registration.
    // It neither constructs a Handler/Storage nor creates a second graph-factory description.
    template <class Declaration> CapabilityImplementation makeImplementation() noexcept
    {
        auto result = Declaration::implementation();
        result.config_type = configurationType<Declaration>();
        if constexpr (std::is_void_v<typename Declaration::Config> ||
                      std::is_nothrow_default_constructible_v<typename Declaration::Config>)
        {
            result.make_default_configuration = &makeDefaultConfiguration<Declaration>;
        }
        const auto frames = frameCalls(static_cast<typename Declaration::FrameBindings*>(nullptr));
        result.frame_calls.assign(frames.begin(), frames.end());
        return result;
    }

    template <class Handler> HandlerRecord handlerRecord(std::string_view key) noexcept
    {
        // Construction is explicit, not inferred from whichever constructors happen to exist.
        static_assert(std::same_as<decltype(Handler::create), Handler*(ResourceRegistry&) noexcept>);
        static_assert(std::is_nothrow_destructible_v<Handler>);
        return {
            cxx::make_basic_type_info<Handler>(),
            std::string(Handler::name),
            std::string(key),
            [](ResourceRegistry& resources) noexcept -> void* { return Handler::create(resources); },
            {nullptr, [](void* object) noexcept { delete static_cast<Handler*>(object); }}
        };
    }

    template <class... Types> auto handlerRecords(std::tuple<Types...>, std::string_view key = {}) noexcept
    {
        return std::array<HandlerRecord, sizeof...(Types)>{handlerRecord<typename Types::type>(key)...};
    }

    template <class... Types> auto storageFactories(std::tuple<Types...>) noexcept
    {
        return std::array<StorageFactory, sizeof...(Types)>{Types::type::factory()...};
    }
} // namespace lux::render::runtime_detail

namespace lux::render
{
    // Graph factories are independent of capability methods and professional Storage installation.
    template <class Feature> RenderResult<FeatureId> RenderServer::registerFeature() noexcept
    {
        return registerFeature(runtime_detail::FeatureEntry{
            Feature::name,
            runtime_detail::configurationType<Feature>(),
            Feature::graph(pagedInstances())
        });
    }

    // One external publication boundary for all declared graph factories, Storage factories and methods.
    // Constructing these values has no GPU/business side effects; all qualification precedes adoption.
    template <class Implementation>
    RenderResult<CapabilityImplementationId> RenderServer::registerCapabilityImplementation() noexcept
    {
        auto factories = runtime_detail::storageFactories(
            runtime_detail::uniqueTypes(static_cast<typename Implementation::Storages*>(nullptr))
        );
        using Methods = decltype(std::tuple_cat(
            std::declval<typename Implementation::Bindings>(),
            std::declval<typename Implementation::FrameBindings>()
        ));
        auto handlers = runtime_detail::handlerRecords(runtime_detail::handlerTypes(static_cast<Methods*>(nullptr)));
        const auto bindings = runtime_detail::commandBindings(static_cast<typename Implementation::Bindings*>(nullptr));
        auto graph_features = []<class... Features>(std::tuple<Features...>, bool paged_instances) noexcept
        {
            return std::array<runtime_detail::FeatureEntry, sizeof...(Features)>{runtime_detail::FeatureEntry{
                Features::type::name,
                runtime_detail::configurationType<typename Features::type>(),
                Features::type::graph(paged_instances)
            }...};
        }(runtime_detail::uniqueTypes(static_cast<typename Implementation::Features*>(nullptr)), pagedInstances());
        auto implementation = runtime_detail::makeImplementation<Implementation>();
        return registerCapabilityImplementation(
            describeCapability<typename Implementation::Capability>(),
            std::move(implementation),
            graph_features,
            factories,
            handlers,
            bindings
        );
    }

    template <class Storage>
        requires(!std::is_void_v<typename Storage::Descriptor::Config>)
    RenderResult<StorageTypeId>
    RenderServer::registerStorage(const typename Storage::Descriptor::Config& config) noexcept
    {
        auto value = ConfigValue::make(config);
        if (!value)
        {
            return cxx::unexpected(value.error());
        }
        return registerStorage(Storage::factory(), std::move(*value));
    }

    template <class Storage>
        requires std::is_void_v<typename Storage::Descriptor::Config>
    RenderResult<StorageTypeId> RenderServer::registerStorage() noexcept
    {
        return registerStorage(Storage::factory(), {});
    }

    template <class Handler, class Bindings>
    RenderResult<HandlerId> RenderServer::registerHandler(std::string_view key) noexcept
    {
        auto record = runtime_detail::handlerRecord<Handler>(key);
        const auto bindings = runtime_detail::commandBindings(static_cast<Bindings*>(nullptr));
        return registerHandler(std::move(record), bindings);
    }
} // namespace lux::render
