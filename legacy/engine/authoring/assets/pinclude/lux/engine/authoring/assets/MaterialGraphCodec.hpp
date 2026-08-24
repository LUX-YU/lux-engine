#pragma once
/**
 * @file MaterialGraphCodec.hpp (private)
 * @brief Compact binary encoder/decoder for `lux::rdesc::MaterialGraph`.
 *
 * Authoring-only implementation detail used to persist the editable graph in a
 * tagged payload. Cooked runtime material codecs never include this header.
 *
 * Wire format (little-endian, no implicit padding; built on detail::ByteWriter/Reader):
 *
 *   u32  magic              = 'LMGR'
 *   u32  endian_tag         = 0x01020304
 *   u32  schema_version     (currently 2; v1 decodes leniently — see below)
 *   u8   shading_model      (EMaterialShadingModel)
 *   u8   alpha_mode         (EAlphaMode)   |
 *   f32  alpha_cutoff                      | render_state
 *   u8   double_sided       (0/1)          |
 *   u32  texture_slot_count;  { str name } x count
 *   u32  param_slot_count;     { str name; u8 type; f32[4] dflt } x count
 *   u32  node_count;  (ascending by id — deterministic)
 *     per node:
 *       u64 id
 *       u16 kind             (EMatNodeKind)
 *       str name
 *       [v2+] u8 ui_placed; f32 ui_x; f32 ui_y   (editor canvas position —
 *             lives INSIDE the node record because decode remaps node ids)
 *       <payload by kind>    (see decode)
 *       u32 input_pin_count
 *         per input pin:
 *           u8 has_source
 *           if has_source: u64 src_node; u32 src_pin
 *           else:          f32[4] constant
 *   u32  trailer            = 'LMGE'
 *
 * Versioning: decode accepts 1..kMatGraphVersion. v1 blobs carry no node
 * positions — every node decodes ui_placed=false and the editor's grid
 * layout places them (GraphLayout's documented lenient-decode case); the
 * next save writes v2. Encode always writes the current version.
 *
 * Enums are encoded as their underlying integer (numeric, not string): a graph
 * is re-bakeable, so an enum reorder is handled by re-baking (ABI-don't-care).
 *
 * Bounds protection (decode fails with an error string on violation):
 *   kMaxMatGraphNodes / kMaxMatGraphSlots — defensive caps.
 */

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <lux/engine/authoring/assets/material/MaterialGraph.hpp>
#include <lux/engine/authoring/assets/visibility.h>

namespace lux::authoring::detail
{
    inline constexpr std::uint32_t kMatGraphMagic     = 0x52474D4Cu; // 'LMGR' (LE)
    inline constexpr std::uint32_t kMatGraphTrailer   = 0x45474D4Cu; // 'LMGE'
    inline constexpr std::uint32_t kMatGraphEndianTag = 0x01020304u;
    inline constexpr std::uint32_t kMatGraphVersion   = 2u;

    inline constexpr std::uint32_t kMaxMatGraphNodes = 1u << 20; // ~1M, defensive
    inline constexpr std::uint32_t kMaxMatGraphSlots = 1u << 16;
    inline constexpr std::uint32_t kMaxMatGraphName  = 1u << 16; // per-string length cap

    /// Encode a MaterialGraph into a compact binary blob. Never fails (other than
    /// std::bad_alloc). The codec stays private to the Authoring component.
    LUX_ENGINE_AUTHORING_ASSETS_PUBLIC std::vector<std::byte>
    encodeMaterialGraph(const lux::rdesc::MaterialGraph& graph);

    /// Decode a MaterialGraph. Returns false and (optionally) writes a human-
    /// readable error into *error_out on any malformed input.
    LUX_ENGINE_AUTHORING_ASSETS_PUBLIC bool
    decodeMaterialGraph(std::span<const std::byte> blob,
                        lux::rdesc::MaterialGraph& out,
                        std::string*               error_out = nullptr) noexcept;
}
