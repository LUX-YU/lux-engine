#pragma once
/**
 * @file Script.hpp
 * @brief Pure-POD description of a script module (no payload, no I/O).
 *
 * `lux::rdesc::Script` is the engine's *resource description* of a script
 * module. It carries everything an editor / runtime needs to know about
 * the artefact *without* loading code: kind, ABI version, function table,
 * resource dependencies, and provenance.
 *
 * The compiled binary / source bytes are NOT stored here — they belong to
 * `lux::asset::ScriptAsset::payload()`. The `.luxasset` codec lives in the
 * asset layer (see `pinclude/.../ScriptDescriptionCodec.hpp`).
 *
 * The runtime contract for `Kind::NativeLibrary` is fixed by
 * `<lux/engine/script/abi/lux_script_abi.h>`: once loaded, the library
 * exports `lux_script_get_module` and `ScriptHost` dispatches calls
 * through `lux_script_call_frame`.
 */

#include <cstdint>
#include <string>
#include <vector>

#include <lux/engine/script/abi/lux_script_abi.h>

namespace lux::rdesc
{
    /**
     * @brief Description of a script module.
     */
    class Script
    {
    public:
        /// Bump on any non-additive change to the description schema.
        static constexpr std::uint32_t kSchemaVersion = 1u;

        /**
         * @brief Backend / payload format selector.
         *
         * The integer values are part of the on-disk binary codec and must
         * not be reordered. Reserve gaps for future additions.
         */
        enum class Kind : std::uint8_t
        {
            Unknown       = 0,
            LuaSource     = 1,   ///< payload = UTF-8 Lua source
            LuaBytecode   = 2,   ///< payload = LuaJIT bytecode
            NativeLibrary = 3,   ///< payload = .dll/.so/.dylib bytes; abi_version must match
            PythonSource  = 4,   ///< payload = UTF-8 Python source (reserved)
        };

        /// Description of a single value slot (param / return).
        struct Type
        {
            std::string   name;
            std::uint64_t type_id  = 0;
            std::uint32_t size     = 0;
            std::uint32_t align    = 0;
            std::uint8_t  kind     = LUX_SCRIPT_VK_VOID;
        };

        /// One entry in the module's function table.
        struct Function
        {
            std::string         name;
            std::uint64_t       symbol_id = 0;
            std::vector<Type>   args;
            std::vector<Type>   returns;
            std::string         native_symbol;
        };

        /// A resource the artefact depends on at runtime.
        struct Dependency
        {
            std::string kind;
            std::string id;
        };

        /// Provenance — which compiler produced this artefact, with what inputs.
        struct Provenance
        {
            std::string compiler_id;
            std::string compiler_version;
            std::string source_id;
            std::string source_hash;
            std::string built_at;
        };

        std::uint32_t            schema_version = kSchemaVersion;
        Kind                     kind           = Kind::Unknown;
        /// Only meaningful for Kind::NativeLibrary; 0 otherwise.
        std::uint32_t            abi_version    = 0;
        std::string              module_name;
        std::vector<Function>    functions;
        std::vector<Dependency>  dependencies;
        Provenance               provenance;
    };
}
