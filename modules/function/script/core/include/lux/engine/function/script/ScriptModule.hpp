#pragma once
/**
 * @file ScriptModule.hpp
 * @brief Backend-agnostic interfaces for compiled script modules.
 *
 * Concrete backends (Lua, native dll, future WASM, ...) implement
 * @ref IScriptModule and produce instances of @ref ScriptFunction wrappers.
 *
 * The host (`ScriptHost`) only ever talks to these interfaces, so adding a new
 * backend never requires changes to gameplay code.
 */

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <lux/engine/function/visibility.h>
#include <lux/engine/function/script/ScriptCallFrame.hpp>
#include <lux/engine/function/script/ScriptSignature.hpp>

namespace lux::script
{
    /**
     * @brief Stable handle to a script function inside a loaded module.
     *
     * Implementations may use any internal representation (Lua registry ref,
     * native function pointer, WASM export index, ...). The host treats it as
     * an opaque token.
     */
    class LUX_FUNCTION_PUBLIC ScriptFunction
    {
    public:
        virtual ~ScriptFunction() = default;

        virtual const FunctionSignature& signature() const noexcept = 0;

        /**
         * @brief Invoke the function with the supplied call frame.
         * @return true on success, false on failure (see backend logs / lastError).
         */
        virtual bool invoke(CallFrame& frame) const = 0;
    };

    /**
     * @brief A loaded script module.
     */
    class LUX_FUNCTION_PUBLIC IScriptModule
    {
    public:
        virtual ~IScriptModule() = default;

        virtual std::string_view name() const noexcept = 0;

        /** Returns nullptr when the function is not present. */
        virtual const ScriptFunction* findFunction(std::string_view name) const = 0;
    };

    using ScriptModulePtr = std::unique_ptr<IScriptModule>;
}
