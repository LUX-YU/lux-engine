#pragma once
/**
 * @file ScriptDescriptionCodec.hpp (private)
 * @brief Compact binary encoder/decoder for `lux::rdesc::Script` headers.
 *
 * Used internally by ScriptSerDeser. NOT part of any public API.
 *
 * Wire format v2 (little-endian, 4-byte aligned; R1: per-kind body block —
 * the kind byte is the Script::Kind STABLE id, never the variant index):
 *
 *   u32  magic        = 'LSDS' (0x5344534C)
 *   u32  endian_tag   = 0x01020304
 *   u32  schema_version           (must equal Script::kSchemaVersion == 2)
 *   u8   kind                     (Script::Kind stable id)
 *   u8   reserved[3]
 *   Str  module_name
 *   u32  dependency_count
 *     Dependency[dependency_count]: { Str kind; Str id; }
 *   Provenance: { Str compiler_id; Str compiler_version;
 *                 Str source_id;   Str source_hash;     Str built_at; }
 *   ── per-kind body ──
 *   LuaSource    (1): Str entry
 *   NativeModule (3): u32 abi_version
 *                     u64 state_layout_hash
 *                     u32 state_size              (per-instance block bytes)
 *                     u32 defaults_len            (≤ state_size)
 *                       byte[defaults_len]        (state defaults prefix)
 *                     u32 function_count
 *                       Function[function_count]:
 *                         Str   name
 *                         u64   symbol_id
 *                         Str   native_symbol
 *                         u32   args_count    + Type[args_count]
 *                         u32   returns_count + Type[returns_count]
 *   CppBehavior  (6): Str behavior
 *   Unknown      (0): (no body bytes)
 *   ── end body ──
 *   u32  trailer_magic = 'LSDE' (0x4544534C)
 *
 *   Str  := u32 byte_length + UTF-8 bytes (no NUL terminator)
 *   Type := Str name + u64 type_id + u32 size + u32 align + u8 kind + u8[3] pad
 *
 * Bounds protection (any violation → decode fails with an error string):
 *   kMaxStringLen     = 64 KiB
 *   kMaxFunctionCount = 65535
 *   kMaxArgumentCount = 256 (per function, per side)
 *   kMaxDependencyCnt = 4096
 *   kMaxStateSize     = 1 MiB (per-instance state block)
 */

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <lux/engine/description/Script.hpp>
#include <lux/engine/resource/asset/codecs/visibility.h>

namespace lux::asset::detail
{
    inline constexpr std::uint32_t kScriptDescMagic   = 0x5344534Cu; // 'LSDS' (LE)
    inline constexpr std::uint32_t kScriptDescTrailer = 0x4544534Cu; // 'LSDE'
    inline constexpr std::uint32_t kEndianTag         = 0x01020304u;

    inline constexpr std::uint32_t kMaxStringLen      = 64u * 1024u;
    inline constexpr std::uint32_t kMaxFunctionCount  = 65535u;
    inline constexpr std::uint32_t kMaxArgumentCount  = 256u;
    inline constexpr std::uint32_t kMaxDependencyCnt  = 4096u;
    inline constexpr std::uint32_t kMaxStateSize      = 1024u * 1024u;

    /// Encode a Script description into a compact binary blob. Never fails
    /// (other than allocator failure, which throws std::bad_alloc).
    /// LUX_RESOURCE_PUBLIC so the asset module's own test target can link it
    /// (the MeshDescriptionCodec rationale).
    LUX_ASSET_CODECS_PUBLIC std::vector<std::byte>
    encodeScriptDescription(const lux::rdesc::Script& script);

    /// Decode a Script description. Returns false and (optionally) writes a
    /// human-readable error message into *error_out on any malformed input.
    LUX_ASSET_CODECS_PUBLIC bool
    decodeScriptDescription(std::span<const std::byte> blob,
                            lux::rdesc::Script&        out,
                            std::string*               error_out = nullptr) noexcept;
}
