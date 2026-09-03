#pragma once

#include <lux/engine/function/script/lua/LuaVm.hpp>
#include <lux/engine/function/script/lua/ScriptAbilityLua.hpp>
#include <lux/engine/function/script/ScriptEvent.hpp>
#include <lux/engine/simulation/scripting/ScriptBackend.hpp>
#include <lux/engine/simulation/scripting/lua/visibility.h>

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

    struct LuaRecordMarshaller final
    {
        std::uint64_t semantic_type{};
        std::string canonical_name;
        std::size_t size{};
        std::size_t alignment{};
        void* context{};
        bool (*push)(
            void* context,
            void* lua_state,
            const void* value
        ) noexcept{};
    };

    enum class ELuaScriptBindingBackendError : std::uint8_t
    {
        INVALID_CAPACITY,
        INVALID_COMPONENT_CONTRACT,
        DUPLICATE_COMPONENT_NAME,
        INVALID_RECORD_MARSHALLER,
        DUPLICATE_RECORD_MARSHALLER,
        INVALID_ABILITY_CONTRIBUTION,
        DUPLICATE_ABILITY_CONTRACT,
        DUPLICATE_ABILITY_NAME,
        DUPLICATE_ABILITY_METHOD,
        UNSUPPORTED_ABILITY_TYPE,
        ABILITY_REGISTRATION_FAILURE,
        INVALID_EVENT_SOURCE,
        DUPLICATE_EVENT_SOURCE,
        UNSUPPORTED_EVENT_PAYLOAD,
        EVENT_REGISTRATION_FAILURE,
        VM_CONFIGURATION_FAILURE,
        ALLOCATION_FAILURE,
    };

    struct LuaScriptBackendConfig final
    {
        std::size_t instance_capacity{};
        std::size_t prepared_call_capacity{};
        std::size_t continuation_capacity{};
        std::size_t execution_depth_capacity{};
        std::size_t ability_method_capacity{};
        std::span<const LuaComponentBinding> components;
        std::span<const LuaRecordMarshaller> record_marshallers;
        std::span<const lux::script::lua::ScriptAbilityLuaContribution> abilities;
        lux::script::lua::ELuaExecutionPolicy execution_policy{
            lux::script::lua::ELuaExecutionPolicy::DEFAULT
        };
        std::size_t event_source_capacity{1U};
        std::span<const lux::script::ScriptEventSourceDescription> events;
    };

    class LUX_ENGINE_SIMULATION_SCRIPT_LUA_PUBLIC LuaScriptBackend final
    {
      public:
        [[nodiscard]] static lux::cxx::expected<
            LuaScriptBackend,
            ELuaScriptBindingBackendError> create(
                LuaScriptBackendConfig config
            ) noexcept;
        ~LuaScriptBackend();

        LuaScriptBackend(LuaScriptBackend&&) noexcept;
        LuaScriptBackend& operator=(LuaScriptBackend&&) noexcept;
        LuaScriptBackend(const LuaScriptBackend&) = delete;
        LuaScriptBackend& operator=(const LuaScriptBackend&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] lux::script::lua::LuaRuntimeInfo runtimeInfo() const noexcept;
        [[nodiscard]] ScriptBackendDescriptor descriptor() noexcept;
      private:
        struct State;
        explicit LuaScriptBackend(
            std::unique_ptr<State> state
        ) noexcept;
        std::unique_ptr<State> state_;
    };
}
