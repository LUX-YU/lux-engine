#pragma once

#include <lux/engine/simulation/script/ScriptBackend.hpp>
#include <lux/engine/simulation/script/lua/visibility.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace lux::simulation::script
{
    struct LuaComponentBinding final
    {
        std::string name;
        std::uint64_t component_type{};
        std::uint64_t semantic_type{};
        std::string canonical_name;
        std::uint8_t abi_kind{LUX_SCRIPT_VK_VOID};
        std::size_t size{};
        std::size_t alignment{};
    };

    enum class ELuaScriptBindingBackendError : std::uint8_t
    {
        INVALID_COMPONENT_CONTRACT,
        DUPLICATE_COMPONENT_NAME,
        ALLOCATION_FAILURE,
    };

    class LUX_ENGINE_SIMULATION_SCRIPT_LUA_PUBLIC LuaScriptBackend final
    {
      public:
        [[nodiscard]] static lux::cxx::expected<
            LuaScriptBackend,
            ELuaScriptBindingBackendError> create(
                std::size_t instance_capacity,
                std::span<const LuaComponentBinding> components = {}
            ) noexcept;
        ~LuaScriptBackend();

        LuaScriptBackend(LuaScriptBackend&&) noexcept;
        LuaScriptBackend& operator=(LuaScriptBackend&&) noexcept;
        LuaScriptBackend(const LuaScriptBackend&) = delete;
        LuaScriptBackend& operator=(const LuaScriptBackend&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] ScriptBackendDescriptor descriptor() noexcept;
        [[nodiscard]] std::size_t loadedInstanceCount() const noexcept;
        [[nodiscard]] std::size_t chunkLoadCount() const noexcept;
        [[nodiscard]] std::size_t preparedReferenceCount() const noexcept;
        [[nodiscard]] std::size_t cachedTracebackCount() const noexcept;

      private:
        struct State;
        explicit LuaScriptBackend(
            std::unique_ptr<State> state
        ) noexcept;
        std::unique_ptr<State> state_;
    };
}
