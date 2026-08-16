#pragma once
/**
 * @file ScriptHost.hpp
 * @brief Central script dispatcher used by the engine runtime.
 *
 * Backends are registered with the host. The host
 * owns loaded modules, hands out stable handles, and routes `invoke()` calls
 * through the appropriate backend.
 *
 * This is the only API gameplay code should consume; it knows nothing about
 * MLIR, Lua internals, or the dynamic library loader.
 */

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <lux/engine/function/visibility.h>
#include <lux/engine/function/script/ScriptBackend.hpp>
#include <lux/engine/function/script/ScriptModule.hpp>

namespace lux::script
{
    using ModuleHandle = std::uint32_t;
    inline constexpr ModuleHandle kInvalidModule = 0;

    /** Lightweight reference returned by ScriptHost::findFunction. */
    struct ScriptFunctionHandle
    {
        ModuleHandle           module = kInvalidModule;
        const ScriptFunction*  fn     = nullptr;

        explicit operator bool() const noexcept { return module != kInvalidModule && fn != nullptr; }
    };

    class ScriptHostImpl;

    class LUX_FUNCTION_PUBLIC ScriptHost
    {
    public:
        ScriptHost();
        ~ScriptHost();

        ScriptHost(const ScriptHost&)            = delete;
        ScriptHost& operator=(const ScriptHost&) = delete;

        /** Register a backend. The host takes ownership. */
        void registerBackend(std::unique_ptr<IScriptBackend> backend);

        /** Lookup a backend by id. */
        IScriptBackend* findBackend(std::string_view id) const;

        /**
         * @brief Load a module via the backend matching the file extension.
         * @returns kInvalidModule on failure.
         */
        ModuleHandle loadModule(const std::filesystem::path& path);

        /** Load via an explicit backend id. */
        ModuleHandle loadModule(std::string_view backend_id, const std::filesystem::path& path);

        /**
         * @brief Load a module from an in-memory payload.
         *
         * The payload's interpretation is dictated by the backend (UTF-8 Lua
         * source, native shared-library bytes, ...). `module_name` is used
         * both as the registered module name (for findFunction-by-name) and
         * as a backend hint. Returns kInvalidModule if the chosen backend
         * does not support in-memory loading or fails to parse.
         */
        ModuleHandle loadModuleFromMemory(
            std::string_view           backend_id,
            std::span<const std::byte> payload,
            std::string_view           module_name
        );

        /** Drop a previously loaded module. Safe to call with kInvalidModule. */
        void unloadModule(ModuleHandle handle);

        const IScriptModule* module(ModuleHandle handle) const;

        ScriptFunctionHandle findFunction(ModuleHandle handle, std::string_view fn_name) const;

        ScriptFunctionHandle findFunction(std::string_view module_name, std::string_view fn_name) const;

        bool invoke(const ScriptFunctionHandle& fn, CallFrame& frame) const;

        /** Last error message produced on the calling thread. */
        std::string lastError() const;

    private:
        std::unique_ptr<ScriptHostImpl> impl_;
    };
}
