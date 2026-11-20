#pragma once

#include <array>
#include <lux/cxx/compile_time/tuple_traits.hpp>
#include <lux/engine/render/extensions/CommandBinding.hpp>
#include <lux/engine/render/extensions/detail/HandlerRecord.hpp>
#include <lux/engine/render/extensions/resource/StorageMeta.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>

namespace lux::render::runtime_detail
{
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
            const auto declare = []<class Method>(std::type_identity<Method>) noexcept
            {
                if constexpr (requires { typename Method::EffectState; })
                {
                    return CommandBinding::
                        declareForEffect<typename Method::Command, typename Method::EffectState, Method::method>();
                }
                else
                {
                    return CommandBinding::declare<
                        typename Method::Command, Method::method, typename Method::Handler
                    >();
                }
            };
            return std::array<CommandBinding, sizeof...(Types)>{declare(Types{})...};
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
