#pragma once

#include <lux/engine/simulation/ScriptBindingSession.hpp>
#include <lux/engine/simulation/script_binding/lua/visibility.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace lux::simulation
{
    struct LuaComponentBinding final
    {
        std::string_view name;
        std::uint64_t component_type{};
        std::uint8_t abi_kind{LUX_SCRIPT_VK_VOID};
    };

    class LUX_ENGINE_SIMULATION_SCRIPT_BINDING_LUA_PUBLIC
        LuaScriptBindingBackend final
    {
      public:
        explicit LuaScriptBindingBackend(
            std::size_t instance_capacity,
            std::span<const LuaComponentBinding> components = {}
        ) noexcept;
        ~LuaScriptBindingBackend();

        LuaScriptBindingBackend(LuaScriptBindingBackend&&) noexcept;
        LuaScriptBindingBackend& operator=(LuaScriptBindingBackend&&) noexcept;
        LuaScriptBindingBackend(const LuaScriptBindingBackend&) = delete;
        LuaScriptBindingBackend& operator=(const LuaScriptBindingBackend&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] ScriptBackendDescriptor descriptor() noexcept;
        [[nodiscard]] std::size_t loadedInstanceCount() const noexcept;
        [[nodiscard]] std::size_t chunkLoadCount() const noexcept;
        [[nodiscard]] std::size_t preparedReferenceCount() const noexcept;
        [[nodiscard]] std::size_t cachedTracebackCount() const noexcept;

      private:
        struct State;
        std::unique_ptr<State> state_;
    };
}
