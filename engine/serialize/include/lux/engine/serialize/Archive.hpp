#pragma once
/**
 * @file Archive.hpp
 * @brief Byte-level POD / string / UUID I/O over a contiguous buffer.
 *
 * The serialization stack has three layers:
 *
 *   - `Archive{Reader,Writer}` (this file) — byte-level primitives. POD writes
 *     are little-endian, strings are length-prefixed (`u32 + utf-8`, no null
 *     terminator), UUIDs are 16 raw bytes. The writer appends to a
 *     caller-owned `std::vector<std::byte>`; the reader walks a span. No
 *     allocations during read.
 *
 *   - `NameTable` — string interning. Written once at file head, referenced
 *     by `u32` index everywhere else.
 *
 *   - `TaggedPropertyArchive` — reflection-driven structured layer. Walks
 *     `RefClass::fields` and writes each as `(name_idx, type_id, size, value)`
 *     so unknown fields can be skipped on read for forward compatibility.
 *
 * Endianness contract: all files written by this stack are LE on disk. Reads
 * on a LE host (x86, ARM little-mode — everything Lux currently targets)
 * are direct `memcpy`. A future BE host would need byte-swap wrappers around
 * the `writePod`/`readPod` paths; flagged for now but not implemented.
 */

#include <lux/engine/serialize/visibility.h>

#include <uuid.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace lux::serialize
{

    /// Little-endian POD writer to a caller-owned byte buffer.
    /// Strings: `u32` length prefix + UTF-8 bytes (no null terminator).
    /// UUIDs: 16 raw bytes in big-endian field order (matches
    /// `uuids::uuid::as_bytes()`).
    class LUX_ENGINE_SERIALIZE_PUBLIC ArchiveWriter
    {
    public:
        explicit ArchiveWriter(std::vector<std::byte>& sink) noexcept
            : sink_(&sink)
        {
        }

        /// Append `n` raw bytes from `p`.
        void writeBytes(const void* p, std::size_t n);

        /// Append a trivially-copyable POD. On big-endian hosts this would
        /// need byte-swap; not implemented (Lux targets LE-only).
        template <class T>
        void writePod(const T& v)
        {
            static_assert(std::is_trivially_copyable_v<T>,
                          "ArchiveWriter::writePod: T must be trivially_copyable");
            writeBytes(&v, sizeof(T));
        }

        /// Append a string: `u32` byte length then the UTF-8 bytes.
        void writeString(std::string_view s);

        /// Append a UUID as its 16 raw bytes.
        void writeUuid(const uuids::uuid& u);

        /// Reserve a `u32` placeholder at the current write position;
        /// returns the byte offset where the placeholder lives. Use
        /// `patchU32At` to fill in the real value later — the idiom for
        /// writing `[length][payload]` when the length isn't known up
        /// front.
        std::size_t reserveU32();

        /// Patch a previously-reserved `u32` at `offset`. `offset` must
        /// come from `reserveU32()` (or any other `u32`-aligned write
        /// position the caller is sure about).
        void patchU32At(std::size_t offset, std::uint32_t value);

        /// Current byte length of the sink.
        [[nodiscard]] std::size_t tell() const noexcept
        {
            return sink_->size();
        }

    private:
        std::vector<std::byte>* sink_;
    };

    /// Little-endian POD reader over a span. Throws `std::out_of_range`
    /// on reads past `end_`.
    class LUX_ENGINE_SERIALIZE_PUBLIC ArchiveReader
    {
    public:
        ArchiveReader(const std::byte* data, std::size_t size) noexcept
            : begin_(data), cur_(data), end_(data + size)
        {
        }

        /// Read `n` bytes into `p`.
        void readBytes(void* p, std::size_t n);

        /// Read a trivially-copyable POD.
        template <class T>
        T readPod()
        {
            static_assert(std::is_trivially_copyable_v<T>,
                          "ArchiveReader::readPod: T must be trivially_copyable");
            T v;
            readBytes(&v, sizeof(T));
            return v;
        }

        /// Read a length-prefixed string.
        std::string readString();

        /// Read a 16-byte UUID.
        uuids::uuid readUuid();

        /// Skip `n` bytes. Used by tagged-property reads to drop unknown
        /// fields by their declared size.
        void skip(std::size_t n);

        [[nodiscard]] std::size_t tell() const noexcept
        {
            return static_cast<std::size_t>(cur_ - begin_);
        }
        [[nodiscard]] std::size_t remaining() const noexcept
        {
            return static_cast<std::size_t>(end_ - cur_);
        }
        [[nodiscard]] bool eof() const noexcept
        {
            return cur_ >= end_;
        }

    private:
        const std::byte* begin_;
        const std::byte* cur_;
        const std::byte* end_;
    };

} // namespace lux::serialize
