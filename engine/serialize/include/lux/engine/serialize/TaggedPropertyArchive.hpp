#pragma once
/**
 * @file TaggedPropertyArchive.hpp
 * @brief Reflection-driven structured serialization.
 *
 * Walks a `lux::meta::RefClass`'s field list. For each field, writes a
 * self-describing record:
 *
 * ```
 *   u32  name_idx        // into NameTable; 0xFFFFFFFF = end of object
 *   u8   type_id         // EArchiveType
 *   u32  size            // payload bytes — lets reader skip unknown
 *   ...                  // <size> bytes of payload
 * ```
 *
 * The reader looks up each name in the current `RefClass::fields`, dispatches
 * by type_id, and **skips the payload entirely if the field is unknown** —
 * which is the forward-compat property that lets a future scene file load
 * cleanly on older code, and an older scene file load cleanly on newer code
 * with new fields defaulting.
 *
 * Type classification is two-level:
 *   - Primitives (`bool`, `int32`, `float`, …) dispatch on `field.type.qtype.base`.
 *   - Record-typed leaves (`Eigen::Vector3f`, `std::string`, `uuids::uuid`, …)
 *     dispatch on `field.type.hash` against a pre-registered table built
 *     with the same `lux::cxx::type_hash<T>()` the rest of the engine uses.
 *   - Other reflected records (nested user structs) recurse — written as
 *     `EArchiveType::Struct` whose payload is itself a tagged-property block.
 *
 * Unknown-typed leaves currently abort the write — that surfaces "we
 * forgot to register this leaf" loudly during development. (Production
 * builds could downgrade to "log + skip".)
 */

#include <lux/engine/serialize/Archive.hpp>
#include <lux/engine/serialize/NameTable.hpp>
#include <lux/engine/serialize/visibility.h>

#include <cstdint>

namespace lux::meta
{
    struct RefClass;
    struct RefField;
} // namespace lux::meta

namespace lux::serialize
{
    /// On-wire type tag. Values are stable across releases — never renumber.
    enum class EArchiveType : std::uint8_t
    {
        Skip         = 0,    ///< Sentinel: payload is opaque, skip on read.
        Bool         = 1,
        Int32        = 2,
        UInt32       = 3,
        Int64        = 4,
        UInt64       = 5,
        Float        = 6,
        Double       = 7,
        String       = 16,   ///< Length-prefixed UTF-8.
        Vec2f        = 17,   ///< 2 × float, raw.
        Vec3f        = 18,   ///< 3 × float, raw.
        Vec4f        = 19,   ///< 4 × float, raw.
        Quatf        = 20,   ///< xyzw, 4 × float, raw.
        Mat4f        = 21,   ///< 16 × float, column-major raw.
        ArrayFloat8  = 32,   ///< Fixed 8 × float (e.g. cascade_splits).
        AssetRef     = 48,   ///< 16-byte UUID.
        Struct       = 64,   ///< Nested tagged-property block. Reader recurses.
    };

    /// Sentinel name index marking end of an object's field stream.
    inline constexpr std::uint32_t kEndOfObject = 0xFFFFFFFFu;

    // ─────────────────────────────────────────────────────────────────────
    //  Writer
    // ─────────────────────────────────────────────────────────────────────

    /// Walks `cls.fields` and writes each field as a tagged-property record.
    /// Object stream is terminated with `name_idx = kEndOfObject`.
    class LUX_ENGINE_SERIALIZE_PUBLIC TaggedPropertyWriter
    {
    public:
        TaggedPropertyWriter(ArchiveWriter& ar, NameTable& names) noexcept
            : ar_(&ar), names_(&names)
        {
        }

        /// Write `obj`'s reflected fields as a stream of tagged properties
        /// followed by `kEndOfObject`.
        void writeObject(const lux::meta::RefClass& cls, const void* obj);

    private:
        void writeField(const lux::meta::RefField& field, const void* obj);

        ArchiveWriter* ar_;
        NameTable*     names_;
    };

    // ─────────────────────────────────────────────────────────────────────
    //  Reader
    // ─────────────────────────────────────────────────────────────────────

    /// Reads a stream of tagged properties up to `kEndOfObject`, populating
    /// `obj`'s reflected fields. Unknown fields (name not found in
    /// `cls.fields`, or type mismatch) are skipped by their declared size.
    class LUX_ENGINE_SERIALIZE_PUBLIC TaggedPropertyReader
    {
    public:
        TaggedPropertyReader(ArchiveReader& ar, const NameTable& names) noexcept
            : ar_(&ar), names_(&names)
        {
        }

        void readObject(const lux::meta::RefClass& cls, void* obj);

    private:
        ArchiveReader*   ar_;
        const NameTable* names_;
    };

} // namespace lux::serialize
