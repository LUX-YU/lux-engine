#pragma once

#include <array>
#include <lux/cxx/compile_time/tuple_traits.hpp>
#include <lux/engine/render/extensions/CapabilityImplementation.hpp>
#include <lux/engine/render/extensions/CommandBinding.hpp>
#include <lux/engine/render/extensions/detail/FeatureEntry.hpp>
#include <lux/engine/render/extensions/detail/HandlerRecord.hpp>
#include <lux/engine/render/extensions/features/FeatureGraphOps.hpp>
#include <lux/engine/render/extensions/resource/StorageMeta.hpp>
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
        if constexpr (sizeof...(Methods) == 0)
        {
            return std::tuple<>{};
        }
        else
        {
            return []<class... Types>(std::tuple<Types...>) noexcept
            {
                return std::tuple_cat(
                    []<class H>(std::type_identity<H>) noexcept
                    {
                        if constexpr (std::is_void_v<H>)
                        {
                            return std::tuple<>{};
                        }
                        else
                        {
                            return std::tuple{std::type_identity<H>{}};
                        }
                    }(Types{})...
                );
            }(typename cxx::remove_repeated_types<std::type_identity<typename Methods::Handler>...>::tuple_type{});
        }
    }

    template <class... Methods> auto commandBindings(std::tuple<Methods...>*) noexcept
    {
        // The enclosing implementation is the sole method declaration. Signature qualification
        // belongs to CommandBinding::declare; no second Handler::Bindings list is consulted.
        return []<class... Types>(std::tuple<Types...>) noexcept
        {
            return std::array<CommandBinding, sizeof...(Types)>{
                CommandBinding::
                    declare<typename Types::type::Command, Types::type::method, typename Types::type::Handler>()...
            };
        }(uniqueTypes(static_cast<std::tuple<Methods...>*>(nullptr)));
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
        return std::array<ResourceRegistration, sizeof...(Types)>{Types::type::factory()...};
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
            runtime_detail::featureGraphOps<Feature>(pagedInstances())
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
        auto handlers = runtime_detail::handlerRecords(
            runtime_detail::handlerTypes(static_cast<typename Implementation::Bindings*>(nullptr))
        );
        const auto bindings = runtime_detail::commandBindings(static_cast<typename Implementation::Bindings*>(nullptr));
        auto graph_features = []<class... Features>(std::tuple<Features...>, bool paged_instances) noexcept
        {
            return std::array<runtime_detail::FeatureEntry, sizeof...(Features)>{runtime_detail::FeatureEntry{
                Features::type::name,
                runtime_detail::configurationType<typename Features::type>(),
                runtime_detail::featureGraphOps<typename Features::type>(paged_instances)
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
    RenderResult<ResourceTypeId>
    RenderServer::registerResource(const typename Storage::Descriptor::Config& config) noexcept
    {
        auto value = ConfigValue::make(config);
        if (!value)
        {
            return cxx::unexpected(value.error());
        }
        return registerResource(Storage::factory(), std::move(*value));
    }

    template <class Storage>
        requires std::is_void_v<typename Storage::Descriptor::Config>
    RenderResult<ResourceTypeId> RenderServer::registerResource() noexcept
    {
        return registerResource(Storage::factory(), {});
    }

    template <class Handler, class Bindings>
    RenderResult<HandlerId> RenderServer::registerHandler(std::string_view key) noexcept
    {
        auto record = runtime_detail::handlerRecord<Handler>(key);
        const auto bindings = runtime_detail::commandBindings(static_cast<Bindings*>(nullptr));
        return registerHandler(std::move(record), bindings);
    }
} // namespace lux::render
