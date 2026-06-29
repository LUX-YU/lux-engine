#pragma once
/**
 * @file NameTable.hpp
 * @brief String interning table for binary file headers.
 *
 * All strings used elsewhere in a `.luxscene` / `.luxpackage` / `.luxasset`
 * (component type names, field names, asset class names, etc.) are deduped
 * into this table and referenced by `u32` index. Index 0 is reserved for
 * the empty string — used as the "no name" sentinel.
 *
 * Wire format:
 * ```
 *   u32 count                      // including the index-0 empty string
 *   repeat (count - 1) times:      // index 0 is implicit
 *     u32 length
 *     bytes[length]
 * ```
 *
 * On reads we materialise `std::string` storage that owns the bytes, so
 * `std::string_view at(idx)` is safe for the lifetime of the NameTable.
 */

#include <lux/engine/serialize/visibility.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lux::serialize
{
    class ArchiveReader;
    class ArchiveWriter;

    /// String interning. Index 0 = empty string (always present).
    /// `intern` is O(1) average; `at` is O(1).
    class LUX_ENGINE_SERIALIZE_PUBLIC NameTable
    {
    public:
        NameTable();

        /// Returns the index for `s`. Empty strings always map to index 0.
        /// Idempotent: identical strings return the same index.
        [[nodiscard]] std::uint32_t intern(std::string_view s);

        /// Lookup. Out-of-range returns an empty string_view (in debug,
        /// asserts).
        [[nodiscard]] std::string_view at(std::uint32_t idx) const noexcept;

        [[nodiscard]] std::uint32_t size() const noexcept
        {
            return static_cast<std::uint32_t>(strings_.size());
        }

        /// Append the table to `ar`. Caller chooses where in the file.
        void serialize(ArchiveWriter& ar) const;

        /// Read a table from the archive at the current position.
        [[nodiscard]] static NameTable deserialize(ArchiveReader& ar);

    private:
        // strings_[0] is always the empty string. Index lookups into
        // `index_` skip index 0 (the empty entry is the implicit fallback
        // for `intern("")`).
        std::vector<std::string>                            strings_;
        std::unordered_map<std::string_view, std::uint32_t> index_;
    };

} // namespace lux::serialize
