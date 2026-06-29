#pragma once
/**
 * @file NativeBackend.hpp
 * @brief Public factory for the native dynamic-library script backend.
 *
 * The native backend loads shared libraries that export
 * @ref LUX_SCRIPT_MODULE_ENTRY (see <lux/engine/script/abi/lux_script_abi.h>),
 * validates the ABI version, and exposes each function described by the
 * returned @ref lux_script_module_desc as a `lux::script::ScriptFunction`.
 *
 * It is the runtime endpoint for FlowForge-MLIR's compiled artefacts as well
 * as any hand-written native plugin. Cross-FFI calls go through
 * `lux::script::CallFrame`; the backend never needs to know which compiler
 * produced the binary.
 */

#include <memory>

#include <lux/engine/core/visibility.h>
#include <lux/engine/script/ScriptBackend.hpp>

namespace lux::script::native_backend
{
    /**
     * @brief Construct a fresh native backend instance.
     *
     * Each backend instance owns its loaded modules. Register it with
     * `ScriptHost::registerBackend()` to make it discoverable by extension.
     * The default extension list is `{"dll", "so", "dylib"}`; consumers that
     * want a single canonical extension (e.g. `.luxscript`) can wrap the
     * returned backend or register the module manually via the host API.
     */
    LUX_CORE_PUBLIC std::unique_ptr<IScriptBackend> create();
}
