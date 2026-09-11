#pragma once

#include <lux/engine/simulation/ScriptSystem.hpp>
#include <lux/engine/simulation/scripting/ScriptSyncStep.hpp>
#include <lux/engine/simulation/scripting/cpp_static/CppStaticScriptBridge.hpp>
#include <lux/engine/simulation/scripting/lua/LuaScriptBackend.hpp>
#include <lux/engine/simulation/scripting/native_lua_tasks/visibility.h>

namespace lux::simulation::script
{
    struct NativeLuaTaskRoute final
    {
        lux::script::ScriptSymbolId lua_export{};
        lux::script::ScriptSymbolId native_export{};
    };

    struct NativeLuaTaskPlan final
    {
        lux::asset::AssetId lua_asset;
        lux::script::ScriptArtifactContentId lua_content;
        lux::asset::AssetId native_asset;
        lux::script::ScriptArtifactContentId native_content;
        const CppStaticContract* contract{};
        std::span<const NativeLuaTaskRoute> routes;
        // Ordinal order is the native program's immutable import order.
        std::span<const lux::rdesc::ScriptFunction> steps;
    };

    struct NativeLuaTaskBackendConfig final
    {
        LuaScriptBackendConfig lua;
        std::span<const CppStaticScriptPoolDescription> native_pools;
        std::span<const NativeLuaTaskPlan> plans;
        ScriptArtifactResolver artifacts;
        std::size_t instance_capacity{};
        std::size_t prepared_method_capacity{};
    };

    enum class ENativeLuaTaskBackendError : std::uint8_t
    {
        INVALID_CONFIGURATION,
        DUPLICATE_PLAN,
        INVALID_PLAN,
        LUA_BACKEND_FAILURE,
        NATIVE_BACKEND_FAILURE,
        ALLOCATION_FAILURE,
    };

    struct NativeLuaTaskBackendStats final
    {
        std::size_t active_instances{};
        std::size_t active_methods{};
        std::size_t active_companion_leases{};
        std::size_t composition_backing_bytes{};
        LuaScriptBackendStats lua;
        CppStaticScriptBackendStats native;
    };

    class LUX_ENGINE_SIMULATION_SCRIPT_NATIVE_LUA_TASKS_PUBLIC NativeLuaTaskBackend final
    {
    public:
        using CreateResult = lux::cxx::expected<NativeLuaTaskBackend, ENativeLuaTaskBackendError>;
        [[nodiscard]] static CreateResult create(NativeLuaTaskBackendConfig config) noexcept;
        ~NativeLuaTaskBackend();
        NativeLuaTaskBackend(NativeLuaTaskBackend&&) noexcept;
        NativeLuaTaskBackend& operator=(NativeLuaTaskBackend&&) noexcept;
        NativeLuaTaskBackend(const NativeLuaTaskBackend&) = delete;
        NativeLuaTaskBackend& operator=(const NativeLuaTaskBackend&) = delete;
        [[nodiscard]] ScriptBackendDescriptor descriptor() noexcept;
        [[nodiscard]] NativeLuaTaskBackendStats stats() const noexcept;
    private:
        struct Impl;
        explicit NativeLuaTaskBackend(std::unique_ptr<Impl> impl) noexcept;
        std::unique_ptr<Impl> impl_;
    };
}
