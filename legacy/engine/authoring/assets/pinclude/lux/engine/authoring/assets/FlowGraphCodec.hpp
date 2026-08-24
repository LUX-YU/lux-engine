#pragma once
/**
 * @file FlowGraphCodec.hpp (private)
 * @brief Compact binary encoder/decoder for `lux::flowforge::FlowGraph`.
 *
 * Used internally by FlowGraphSerDeser. NOT part of any public API. Mirrors
 * MaterialGraphCodec: binary at the I/O seam only; the asset holds the
 * concrete graph, never an opaque blob or JSON.
 *
 * Wire format (little-endian, no implicit padding; built on detail::ByteWriter/Reader):
 *
 *   u32  magic              = 'LFGR'
 *   u32  endian_tag         = 0x01020304
 *   u32  schema_version     (currently 3)
 *   u32  variable_count
 *     per variable: u64 id; str name; ScalarSchema type; <constant payload>
 *   u32  node_count   (ascending by STABLE node id — deterministic)
 *     per node:
 *       u64 id             (STABLE FlowGraph id — preserved on decode)
 *       u16 op             (ENodeOperation)
 *       str display_name
 *       u8 ui_placed; f32 ui_x; f32 ui_y
 *       <payload by op>:
 *         SEQUENCE                       u16 extra exec-out count
 *         ADD..LOGICAL_NOT/NEGATE/CMP_*  ScalarSchema operand_type
 *         GET_VARIABLE / SET_VARIABLE    u64 variable_id
 *         NATIVE_FUNC_CALL               str registry creator name
 *         FUNC_DEF_START                 u16 argc {ScalarSchema; str name} x argc
 *                                        u16 retc {ScalarSchema; str name} x retc
 *         FUNC_RETURN / GRAPH_FUNC_CALL  u64 func-def node id
 *         [v2] GET_FIELD / SET_FIELD     str class full_name; str field name
 *              (decode resolves through ReflectionRegistry)
 *         [v2] ON_EVENT                  u16 argc {ScalarSchema; str name} x argc
 *         (other ops: no payload)
 *   u32  link_count   (source-ordered: node id asc, out-pin ordinal asc)
 *     per link: u64 src_node; u16 src_out_ordinal; u64 dst_node; u16 dst_in_ordinal
 *   u32  constant_count
 *     per constant: u64 node; u16 in_ordinal; <constant payload>
 *   u32  trailer            = 'LFGE'
 *
 *   ScalarSchema is the frozen lux-cxx triplet: u8 kind, u8 major, u8 minor.
 *   <constant payload>: ScalarSchema followed by canonical value bytes;
 *     UTF8 uses str bytes, numeric scalars use their fixed-width raw value.
 *   Compiler-derived type hashes are never persisted.
 *
 * Pin addressing is by ORDINAL (index into Node::inPins()/outPins()), which
 * is stable per node construction; STABLE node ids come from
 * FlowGraph::next_node_id_ and are preserved through decode
 * (addNodesWithId), so id-carrying payloads (variables, func-defs) resolve
 * without a remap table.
 *
 * Decode needs the NodeRegistry: NATIVE_FUNC_CALL nodes are re-instantiated
 * through their registry creator (Node::creatorName(), stamped by
 * NodeRegistry::registerNode).
 *
 * Bounds protection (decode fails with an error string on violation):
 *   kMaxFlowGraphNodes / kMaxFlowGraphPins / kMaxFlowGraphName.
 */

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <lux/engine/authoring/flowforge/FlowGraph.hpp>
#include <lux/engine/authoring/flowforge/NodeRegistry.hpp>
#include <lux/engine/authoring/assets/visibility.h>

namespace lux::authoring::detail
{
    inline constexpr std::uint32_t kFlowGraphMagic     = 0x5247464Cu; // 'LFGR' (LE)
    inline constexpr std::uint32_t kFlowGraphTrailer   = 0x4547464Cu; // 'LFGE'
    inline constexpr std::uint32_t kFlowGraphEndianTag = 0x01020304u;
    inline constexpr std::uint32_t kFlowGraphVersion   = 3u;

    inline constexpr std::uint32_t kMaxFlowGraphNodes = 1u << 20; // defensive caps
    inline constexpr std::uint32_t kMaxFlowGraphPins  = 1u << 16;
    inline constexpr std::uint32_t kMaxFlowGraphName  = 1u << 16;

    /// Encode a FlowGraph into a compact binary blob. Returns an EMPTY vector
    /// and (optionally) writes an error when the graph contains a node the
    /// codec cannot represent (e.g. a native call constructed outside the
    /// NodeRegistry, or a non-scalar constant).
    LUX_ENGINE_AUTHORING_ASSETS_PUBLIC std::vector<std::byte>
    encodeFlowGraph(const lux::flowforge::FlowGraph& graph,
                    std::string*                     error_out = nullptr);

    /// Decode a FlowGraph. Returns false and (optionally) writes a human-
    /// readable error on any malformed input or unresolvable reference.
    LUX_ENGINE_AUTHORING_ASSETS_PUBLIC bool
    decodeFlowGraph(std::span<const std::byte>   blob,
                    lux::flowforge::FlowGraph&   out,
                    lux::flowforge::NodeRegistry& registry,
                    std::string*                 error_out = nullptr);
}
