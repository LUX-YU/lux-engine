#pragma once
/**
 * @file ScriptBackend.hpp
 * @brief Pluggable backend interface for ScriptRuntime.
 *
 * A backend knows how to load a particular flavour of script module: Lua source
 * file, native compiled dll exposing @ref lux_script_get_module, future WASM,
 * etc. Backends are registered with the @ref ScriptRuntime and selected by
 * filesystem extension or by explicit name.
 */

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <lux/engine/function/visibility.h>
#include <lux/engine/function/script/ScriptModule.hpp>
#include <lux/engine/function/script/ScriptResult.hpp>

namespace lux::script
{
    class LUX_FUNCTION_PUBLIC IScriptBackend
    {
    public:
        virtual ~IScriptBackend() = default;

        /** Stable identifier, e.g. "lua", "native", "wasm". */
        virtual std::string_view backendId() const noexcept = 0;

        /** File extensions handled by this backend (lower-case, no leading dot). */
        virtual std::vector<std::string> handledExtensions() const = 0;

        /**
         * @brief Try to load a module from disk.
         * @returns A complete module on success or a structured failure.
         */
        virtual ScriptResult<ScriptModulePtr> loadModule(
            const std::filesystem::path& path
        ) = 0;

        /**
         * @brief Try to load a module from an in-memory payload.
         *
         * The payload's interpretation is backend-specific (e.g. UTF-8 source
         * for Lua, shared-library bytes for native). `module_name` is a hint
         * used for diagnostics and as a default scratch name when no other
         * identifier is available; it may be empty.
         *
         * The default implementation reports `MEMORY_LOAD_UNSUPPORTED`.
         */
        virtual ScriptResult<ScriptModulePtr> loadFromMemory(
            std::span<const std::byte> /*payload*/,
            std::string_view /*module_name*/
        )
        {
            return lux::cxx::unexpected(scriptFailure(
                EScriptError::MEMORY_LOAD_UNSUPPORTED,
                "script backend does not support memory loading"
            ));
        }
    };
}
