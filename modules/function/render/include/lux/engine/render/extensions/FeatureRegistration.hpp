#pragma once

#include <array>
#include <lux/cxx/compile_time/tuple_traits.hpp>
#include <lux/engine/render/extensions/CommandBinding.hpp>
#include <lux/engine/render/extensions/detail/HandlerRecord.hpp>
#include <lux/engine/render/extensions/features/FeatureGraphOps.hpp>
#include <lux/engine/render/extensions/storage/StorageFactory.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>

namespace lux::render::runtime_detail
{
    template <class Feature> const CapabilitySupport* featureCapabilities() noexcept
    {
        if constexpr (requires { Feature::capabilities(); })
        {
            return &Feature::capabilities();
        }
        else
        {
            return nullptr; // A private composition dependency need not be selectable by an application.
        }
    }

    template <class Feature> constexpr cxx::TypeToken featureConfigType() noexcept
    {
        if constexpr (std::is_void_v<typename Feature::Config>)
        {
            return {};
        }
        else
        {
            return cxx::typeToken<typename Feature::Config>();
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
        // A Handler has one declared method mapping. Features can select or repeat those methods,
        // but cannot silently assign another member function to the same registered implementation.
        static_assert((cxx::tuple_has_type_v<typename Methods::Handler::Bindings, Methods> && ...));
        return []<class... Types>(std::tuple<Types...>) noexcept
        {
            return std::array<CommandBinding, sizeof...(Types)>{
                CommandBinding::
                    declare<typename Types::type::Command, Types::type::method, typename Types::type::Handler>()...
            };
        }(uniqueTypes(static_cast<std::tuple<Methods...>*>(nullptr)));
    }

    template <class Handler> HandlerRecord handlerRecord(std::string_view key) noexcept
    {
        // Construction is explicit, not inferred from whichever constructors happen to exist.
        static_assert(std::same_as<decltype(Handler::create), Handler*(ResourceRegistry&) noexcept>);
        static_assert(std::is_nothrow_destructible_v<Handler>);
        return {
            cxx::typeToken<Handler>(),
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
    // External declarations use the same tuple shape as private built-ins. Only this route
    // qualifies a whole dependency set before publication. No implementation instance is constructed here.
    template <class Feature> RenderResult<FeatureId> RenderServer::registerFeature() noexcept
    {
        auto factories = runtime_detail::storageFactories(
            runtime_detail::uniqueTypes(static_cast<typename Feature::Storages*>(nullptr))
        );
        auto handlers = runtime_detail::handlerRecords(
            runtime_detail::handlerTypes(static_cast<typename Feature::Commands*>(nullptr))
        );
        const auto bindings = runtime_detail::commandBindings(static_cast<typename Feature::Commands*>(nullptr));
        return registerFeature(
            Feature::name.name(),
            runtime_detail::featureConfigType<Feature>(),
            Feature::graph(pagedInstances()),
            runtime_detail::featureCapabilities<Feature>(),
            factories,
            handlers,
            bindings
        );
    }

    template <class Storage>
        requires(!std::is_void_v<typename Storage::Descriptor::Config>)
    RenderResult<StorageTypeId> RenderServer::registerStorage(const typename Storage::Descriptor::Config& config
    ) noexcept
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

    template <class Handler> RenderResult<HandlerId> RenderServer::registerHandler(std::string_view key) noexcept
    {
        auto record = runtime_detail::handlerRecord<Handler>(key);
        const auto bindings = runtime_detail::commandBindings(static_cast<typename Handler::Bindings*>(nullptr));
        return registerHandler(std::move(record), bindings);
    }
} // namespace lux::render
