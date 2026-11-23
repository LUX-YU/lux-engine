#pragma once

#include <lux/cxx/compile_time/type_info.hpp>
#include <lux/engine/function/graph/GraphTypes.hpp>
#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <memory>
#include <type_traits>
#include <utility>

namespace lux::render
{
    // Synchronous borrow during logical CreateEffect. One state may be created for each live node.
    class EffectInitializationContext
    {
    public:
        virtual ~EffectInitializationContext() noexcept = default;
        [[nodiscard]] virtual graph::NodeId nodeId() const noexcept = 0;

        // Publishes one immutable value for the device builds of this logical node.
        [[nodiscard]] virtual RenderResult<void> setConfiguration(ConfigValue) noexcept = 0;

        template <class T> requires requires(T&& value) { ConfigValue::make(std::forward<T>(value)); }
        [[nodiscard]] RenderResult<void> setConfiguration(T&& value) noexcept
        {
            auto configuration = ConfigValue::make(std::forward<T>(value));
            if (!configuration) return cxx::unexpected(configuration.error());
            return setConfiguration(std::move(*configuration));
        }

        template <class State, class... Args>
            requires std::is_nothrow_constructible_v<State, Args...> && std::is_nothrow_destructible_v<State>
        [[nodiscard]] RenderResult<void> initializeState(Args&&... args) noexcept
        {
            auto value = std::make_unique<State>(std::forward<Args>(args)...);
            const auto destroy = [](void* object) noexcept { delete static_cast<State*>(object); };
            auto adopted = adoptState(value.get(), destroy, cxx::make_basic_type_info<State>(), sizeof(State));
            if (!adopted) return adopted;
            (void)value.release();
            return {};
        }

    private:
        // State owns everything it needs after initialization. Ownership transfers only on success.
        [[nodiscard]] virtual RenderResult<void> adoptState(
            void* value, void (*destroy)(void*) noexcept, cxx::basic_type_info type, std::size_t bytes
        ) noexcept = 0;
    };
}
