#pragma once

#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/engine/simulation/ecs/Registry.hpp>
#include <lux/engine/simulation/ecs/schema/visibility.h>

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

namespace lux::simulation::ecs
{
    // Providers may specialize this contract for a reviewed allocating move (for
    // example MSVC deque). Such a move may allocate, but must not reject business
    // input, invoke business callbacks, or throw a recoverable domain exception.
    template <class Component>
    inline constexpr bool componentInstallHasNoBusinessFailure = std::is_nothrow_move_constructible_v<Component>;

    // Owns one decoded value, independently of Registry storage. Installation consumes it.
    // Normal format/reference/business rejection must finish before installation.
    class LUX_ENGINE_SIMULATION_ECS_SCHEMA_PUBLIC DecodedComponent final
    {
      public:
        DecodedComponent(DecodedComponent &&other) noexcept;
        DecodedComponent &operator=(DecodedComponent &&other) noexcept;
        DecodedComponent(const DecodedComponent &) = delete;
        DecodedComponent &operator=(const DecodedComponent &) = delete;
        ~DecodedComponent();

        [[nodiscard]] lux::cxx::TypeToken type() const noexcept;
        // Accounts for the inline component allocation, not provider-owned dynamic storage.
        [[nodiscard]] std::size_t accountedBytes() const noexcept;
        void installInto(Registry &registry, Entity entity) &&;

        template <class Component>
            requires componentInstallHasNoBusinessFailure<Component> && std::is_move_constructible_v<Component> &&
                     std::is_nothrow_destructible_v<Component>
        [[nodiscard]] static DecodedComponent own(Component value, std::shared_ptr<const void> code)
        {
            return DecodedComponent(
                lux::cxx::typeToken<Component>(), sizeof(Component), new Component(std::move(value)),
                [](void *value) noexcept { delete static_cast<Component *>(value); },
                [](Registry &registry, Entity entity, void *value)
                { registry.emplace<Component>(entity, std::move(*static_cast<Component *>(value))); }, std::move(code));
        }

      private:
        using Destroy = void (*)(void *) noexcept;
        using Install = void (*)(Registry &, Entity, void *);
        DecodedComponent(lux::cxx::TypeToken type, std::size_t bytes, void *value, Destroy destroy, Install install,
                         std::shared_ptr<const void> code) noexcept;
        void reset() noexcept;

        lux::cxx::TypeToken type_;
        std::size_t bytes_;
        void *value_;
        Destroy destroy_;
        Install install_;
        std::shared_ptr<const void> code_;
    };
} // namespace lux::simulation::ecs
