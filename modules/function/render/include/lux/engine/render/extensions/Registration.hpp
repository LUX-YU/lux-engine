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
    template <class Declaration> constexpr cxx::basic_type_info configurationType() noexcept
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

} // namespace lux::render::runtime_detail

namespace lux::render
{
    // Graph factories are independent of capability methods and professional Storage installation.
    template <class Feature> RenderResult<FeatureId> RenderServer::registerFeature() noexcept
    {
        return registerFeature(runtime_detail::FeatureEntry{
            Feature::name,
            runtime_detail::configurationType<Feature>(),
            FeatureGraphOps::make<Feature>()
        });
    }

    // Reference an already registered module closure. No factory/callback payload is resubmitted.
    template <class Implementation>
    RenderResult<CapabilityImplementationId> RenderServer::registerCapabilityImplementation() noexcept
    {
        auto capability = capabilityId(cxx::make_basic_type_info<typename Implementation::Capability>());
        if (!capability) return cxx::unexpected(capability.error());
        auto implementation = Implementation::implementation();
        implementation.features.clear();
        implementation.command_bindings.clear();
        RenderResult<void> resolved;
        std::apply([&](auto... feature) noexcept
        {
            ([&]() noexcept
            {
                if (!resolved) return;
                using Feature = typename decltype(feature)::type;
                auto id = featureId(Feature::name.name());
                if (!id) resolved = cxx::unexpected(id.error());
                else implementation.features.push_back(*id);
            }(), ...);
        }, runtime_detail::uniqueTypes(static_cast<typename Implementation::Features*>(nullptr)));
        std::apply([&](auto... method) noexcept
        {
            ([&]() noexcept
            {
                if (!resolved) return;
                using Method = typename decltype(method)::type;
                HandlerId handler;
                if constexpr (!std::is_void_v<typename Method::Handler>)
                {
                    auto id = handlerId(cxx::make_basic_type_info<typename Method::Handler>());
                    if (!id)
                    {
                        resolved = cxx::unexpected(id.error());
                        return;
                    }
                    handler = *id;
                }
                auto location = commandBinding(
                    commandDescriptor<typename Method::Command>(),
                    cxx::make_basic_type_info<Method>(),
                    handler
                );
                if (!location) resolved = cxx::unexpected(location.error());
                else implementation.command_bindings.push_back(*location);
            }(), ...);
        }, runtime_detail::uniqueTypes(static_cast<typename Implementation::Bindings*>(nullptr)));
        if (!resolved) return cxx::unexpected(resolved.error());
        return addCapabilityImplementation(*capability, implementation);
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
