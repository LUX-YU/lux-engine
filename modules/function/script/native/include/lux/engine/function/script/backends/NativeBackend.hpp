#pragma once
/**
 * @file NativeBackend.hpp
 * @brief Public factory for the native dynamic-library script backend.
 *
 * The native backend loads shared libraries that export
 * @ref LUX_SCRIPT_MODULE_ENTRY (see <lux/engine/function/script/abi/lux_script_abi.h>),
 * validates the ABI version, and exposes each function described by the
 * returned @ref lux_script_module_desc as a `lux::script::ScriptFunction`.
 *
 * It is the runtime endpoint for FlowForge-MLIR's compiled artefacts as well
 * as any hand-written native plugin. Cross-FFI calls go through
 * `lux::script::CallFrame`; the backend never needs to know which compiler
 * produced the binary.
 */

#include <lux/cxx/core/move_only_function.hpp>
#include <memory>
#include <string_view>

#include <lux/engine/function/visibility.h>
#include <lux/engine/function/script/ScriptBackend.hpp>

namespace lux::script::native_backend
{
    /**
     * @brief Host-side symbol resolver for modules that export
     *        @ref LUX_SCRIPT_BIND_HOST_ENTRY (see lux_script_abi.h).
     *
     * Maps an imported symbol name to its engine-side address, or nullptr
     * when unknown. Typical wiring resolves reflection-trampoline symbols
     * plus whatever C-ABI services the host chooses to expose. The returned
     * addresses must stay valid for as long as any bound module lives.
     */
    using HostSymbolResolver =
        lux::cxx::move_only_function<void*(std::string_view symbol)>;

    /**
     * @brief Construct a fresh native backend instance.
     *
     * Register one backend instance with `ScriptRuntime::registerBackend()` to
     * make it discoverable by extension. The runtime owns every module returned
     * by that backend.
     * The default extension list is `{"dll", "so", "dylib"}`. Memory images
     * and extension-independent sources use the explicit `"native"` backend id.
     *
     * `resolver` feeds modules that export LUX_SCRIPT_BIND_HOST_ENTRY —
     * binding runs once at load, before any function is exposed, and a
     * module with unresolved imports is rejected. Without a resolver, such
     * modules fail to load (import-free modules are unaffected).
     */
    LUX_FUNCTION_PUBLIC std::unique_ptr<IScriptBackend> create(HostSymbolResolver resolver = {});
}
