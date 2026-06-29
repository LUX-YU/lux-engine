#include <lux/engine/asset/PakCodec.hpp>
#include <lux/engine/asset/VirtualPath.hpp>
#include <lux/engine/asset/detail/ByteIO.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <fstream>
#include <system_error>

namespace lux::asset::detail
{
    namespace
    {
        bool fail(std::string* error_out, std::string msg)
        {
            if (error_out) *error_out = std::move(msg);
            return false;
        }

        void writeUuid(ByteWriter& w, const asset_id_t& id)
        {
            const auto span = id.as_bytes();
            w.bytes(span.data(), span.size());
        }

        bool readUuid(ByteReader& r, asset_id_t& id)
        {
            std::array<std::uint8_t, 16> bytes{};
            if (!r.bytes(bytes.data(), bytes.size()))
                return false;
            id = asset_id_t{ bytes };
            return true;
        }

        // Stream-copy @p src into @p ofs, FNV-hashing on the fly.
        bool copyFileInto(const std::filesystem::path& src, std::ofstream& ofs,
                          std::uint64_t& size_out, std::uint64_t& hash_out)
        {
            std::ifstream ifs(src, std::ios::binary);
            if (!ifs)
                return false;

            std::uint64_t hash = 14695981039346656037ull;
            std::uint64_t size = 0;
            char buf[64 * 1024];
            while (ifs)
            {
                ifs.read(buf, sizeof(buf));
                const std::streamsize n = ifs.gcount();
                if (n <= 0)
                    break;
                hash = fnv1a64(buf, static_cast<std::size_t>(n), hash);
                ofs.write(buf, n);
                size += static_cast<std::uint64_t>(n);
            }
            if (ifs.bad() || !ofs)
                return false;
            size_out = size;
            hash_out = hash;
            return true;
        }
    }

    bool writePakFile(const std::filesystem::path& out_pak,
                      std::vector<PakWriteEntry>   entries,
                      std::string_view             mount_hint,
                      std::string*                 error_out)
    {
        if (!VirtualPath::isLegalRoot(mount_hint))
            return fail(error_out,
                        std::format("illegal mount hint '{}'", mount_hint));
        if (entries.size() > kMaxPakEntries)
            return fail(error_out, "too many entries");

        // Deterministic order: entries by uuid (this IS the binary-search
        // order the reader relies on), paths by byte order.
        std::sort(entries.begin(), entries.end(),
                  [](const PakWriteEntry& a, const PakWriteEntry& b)
                  {
                      return a.id < b.id;
                  });
        for (std::size_t i = 1; i < entries.size(); ++i)
        {
            if (entries[i - 1].id == entries[i].id)
                return fail(error_out, "duplicate uuid across entries");
        }

        std::vector<const PakWriteEntry*> path_rows;
        path_rows.reserve(entries.size());
        for (const auto& e : entries)
        {
            if (e.vpath.empty())
                continue; // no PATH row (tombstone/patch shape)
            if (auto err = VirtualPath::validateRelative(e.vpath))
                return fail(error_out,
                            std::format("non-canonical vpath '{}' (err={})",
                                        e.vpath, static_cast<int>(*err)));
            path_rows.push_back(&e);
        }
        std::sort(path_rows.begin(), path_rows.end(),
                  [](const PakWriteEntry* a, const PakWriteEntry* b)
                  {
                      return a->vpath < b->vpath;
                  });
        for (std::size_t i = 1; i < path_rows.size(); ++i)
        {
            if (path_rows[i - 1]->vpath == path_rows[i]->vpath)
                return fail(error_out,
                            std::format("duplicate vpath '{}'",
                                        path_rows[i]->vpath));
        }

        // ── payload region (streamed to .tmp) ────────────────────────────
        std::filesystem::path tmp = out_pak;
        tmp += ".tmp";
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs)
            return fail(error_out, "cannot open output file");

        ofs.write(reinterpret_cast<const char*>(kPakFileMagic),
                  sizeof(kPakFileMagic));
        std::uint64_t cursor = sizeof(kPakFileMagic);

        struct Placed { std::uint64_t offset, size, hash; };
        std::vector<Placed> placed(entries.size());

        static constexpr char kZeros[kPakAlign] = {};
        for (std::size_t i = 0; i < entries.size(); ++i)
        {
            const std::uint64_t pad = (kPakAlign - cursor % kPakAlign) % kPakAlign;
            if (pad)
                ofs.write(kZeros, static_cast<std::streamsize>(pad));
            cursor += pad;

            placed[i].offset = cursor;
            if (!copyFileInto(entries[i].source_file, ofs,
                              placed[i].size, placed[i].hash))
            {
                ofs.close();
                std::error_code rm;
                std::filesystem::remove(tmp, rm);
                return fail(error_out,
                            std::format("cannot read payload '{}'",
                                        entries[i].source_file.string()));
            }
            cursor += placed[i].size;
        }
        const std::uint64_t index_offset = cursor;

        // ── index blob ───────────────────────────────────────────────────
        ByteWriter w;
        w.u32(kPakIndexMagic);
        w.u32(kPakEndianTag);
        w.u32(kPakVersion);
        w.str(mount_hint);

        {
            w.u32(kPakSectionEntb);
            w.u64(static_cast<std::uint64_t>(entries.size()) * kPakEntryBytes);
            for (std::size_t i = 0; i < entries.size(); ++i)
            {
                writeUuid(w, entries[i].id);
                w.u64(placed[i].offset);
                w.u64(placed[i].size);
                w.u64(placed[i].size); // uncompressed == size (v1: NONE only)
                w.u32(entries[i].asset_magic);
                w.u8(kPakCompressionNone);
                w.u8(0);               // flags: v1 writer emits no tombstones
                w.u8(0); w.u8(0);      // pad[2]
                w.u64(placed[i].hash);
            }
        }
        {
            ByteWriter pw;
            for (const auto* e : path_rows)
            {
                pw.str(e->vpath);
                writeUuid(pw, e->id);
            }
            auto path_bytes = std::move(pw).take();
            w.u32(kPakSectionPath);
            w.u64(static_cast<std::uint64_t>(path_bytes.size()));
            w.bytes(path_bytes.data(), path_bytes.size());
        }
        w.u32(kPakIndexTrailer);
        const auto index = std::move(w).take();

        ofs.write(reinterpret_cast<const char*>(index.data()),
                  static_cast<std::streamsize>(index.size()));

        // ── footer ───────────────────────────────────────────────────────
        PakFooter footer{};
        footer.magic        = kPakFooterMagic;
        footer.endian_tag   = kPakEndianTag;
        footer.version      = kPakVersion;
        footer.flags        = 0;
        footer.index_offset = index_offset;
        footer.index_size   = static_cast<std::uint64_t>(index.size());
        footer.index_hash   = fnv1a64(index.data(), index.size());
        footer.hash_algo    = kPakHashFnv1a64;
        std::memset(footer.key_id, 0, sizeof(footer.key_id));
        footer.trailer      = kPakFooterTrailer;
        ofs.write(reinterpret_cast<const char*>(&footer), sizeof(footer));

        // Flush + close BEFORE checking — buffered failures (disk full)
        // only surface at flush time (same discipline as exportAsLuxAsset).
        ofs.flush();
        ofs.close();
        if (!ofs.good())
        {
            std::error_code rm;
            std::filesystem::remove(tmp, rm);
            return fail(error_out, "write failed (flush)");
        }

        std::error_code mv;
        std::filesystem::rename(tmp, out_pak, mv);
        if (mv)
        {
            std::error_code rm;
            std::filesystem::remove(tmp, rm);
            return fail(error_out, "atomic rename failed");
        }
        return true;
    }

    bool readPakIndex(std::istream&  is,
                      std::uint64_t  file_size,
                      PakIndex&      out,
                      std::string*   error_out)
    {
        out = PakIndex{};

        if (file_size < sizeof(kPakFileMagic) + sizeof(PakFooter))
            return fail(error_out, "file too small for a pak");

        // File magic.
        std::uint8_t magic[sizeof(kPakFileMagic)]{};
        is.clear();
        is.seekg(0, std::ios::beg);
        if (!is.read(reinterpret_cast<char*>(magic), sizeof(magic))
            || std::memcmp(magic, kPakFileMagic, sizeof(magic)) != 0)
        {
            return fail(error_out, "bad pak file magic");
        }

        // Footer.
        PakFooter footer{};
        is.clear();
        is.seekg(static_cast<std::streamoff>(file_size - sizeof(PakFooter)),
                 std::ios::beg);
        if (!is.read(reinterpret_cast<char*>(&footer), sizeof(footer)))
            return fail(error_out, "cannot read footer");
        if (footer.magic != kPakFooterMagic || footer.trailer != kPakFooterTrailer)
            return fail(error_out, "bad footer magic/trailer");
        if (footer.endian_tag != kPakEndianTag)
            return fail(error_out, "endian mismatch");
        if (footer.version != kPakVersion)
            return fail(error_out, "unsupported pak version");
        if (footer.flags != 0)
            return fail(error_out, "unsupported footer flags (encrypted index?)");
        if (footer.hash_algo != kPakHashFnv1a64)
            return fail(error_out, "unknown index hash algorithm");

        // Index bounds + bytes + hash.
        if (footer.index_size == 0 || footer.index_size > kMaxPakIndexSize)
            return fail(error_out, "index size out of range");
        if (footer.index_offset < sizeof(kPakFileMagic)
            || footer.index_offset + footer.index_size
                   > file_size - sizeof(PakFooter))
        {
            return fail(error_out, "index region out of bounds");
        }
        std::vector<std::byte> index(static_cast<std::size_t>(footer.index_size));
        is.clear();
        is.seekg(static_cast<std::streamoff>(footer.index_offset), std::ios::beg);
        if (!is.read(reinterpret_cast<char*>(index.data()),
                     static_cast<std::streamsize>(index.size())))
        {
            return fail(error_out, "cannot read index");
        }
        if (fnv1a64(index.data(), index.size()) != footer.index_hash)
            return fail(error_out, "index hash mismatch (corrupt index)");

        // Parse the index blob.
        std::string err;
        ByteReader r(std::span<const std::byte>{ index }, &err);
        std::uint32_t u{};
        if (!r.u32(u) || u != kPakIndexMagic)
            return fail(error_out, "bad index magic");
        if (!r.u32(u) || u != kPakEndianTag)
            return fail(error_out, "bad index endian tag");
        if (!r.u32(u) || u != kPakVersion)
            return fail(error_out, "bad index version");
        if (!r.str(out.mount_hint, VirtualPath::kMaxLength))
            return fail(error_out, "cannot read mount hint");
        if (!VirtualPath::isLegalRoot(out.mount_hint))
            return fail(error_out,
                        std::format("illegal mount hint '{}'", out.mount_hint));

        bool seen_entb = false, seen_path = false;
        while (true)
        {
            std::uint32_t tag{};
            if (!r.u32(tag))
                return fail(error_out, "index truncated before trailer");
            if (tag == kPakIndexTrailer)
                break;

            std::uint64_t size{};
            if (!r.u64(size))
                return fail(error_out, "section size truncated");
            if (size > r.remaining())
                return fail(error_out, "section size out of bounds");

            if (tag == kPakSectionEntb)
            {
                if (seen_entb)
                    return fail(error_out, "duplicate ENTB section");
                seen_entb = true;
                if (size % kPakEntryBytes != 0)
                    return fail(error_out, "ENTB size not a multiple of 56");
                const std::uint64_t count = size / kPakEntryBytes;
                if (count > kMaxPakEntries)
                    return fail(error_out, "too many entries");

                out.entries.resize(static_cast<std::size_t>(count));
                for (auto& e : out.entries)
                {
                    if (!readUuid(r, e.id)) break;
                    r.u64(e.offset);
                    r.u64(e.size);
                    r.u64(e.uncompressed_size);
                    r.u32(e.asset_magic);
                    r.u8(e.compression);
                    r.u8(e.flags);
                    std::uint8_t pad0{}, pad1{};
                    r.u8(pad0); r.u8(pad1);
                    if (!r.u64(e.content_hash))
                        break;
                }
                if (!r.ok())
                    return fail(error_out, "ENTB truncated");

                for (std::size_t i = 0; i < out.entries.size(); ++i)
                {
                    const auto& e = out.entries[i];
                    if (i > 0 && !(out.entries[i - 1].id < e.id))
                        return fail(error_out,
                                    "ENTB not strictly uuid-ascending");
                    if (e.id.is_nil())
                        return fail(error_out, "nil uuid entry");
                    // Payload must live inside [magic, index) — tombstones
                    // carry size 0 and are exempt from the offset check.
                    if (e.size > 0
                        && (e.offset < sizeof(kPakFileMagic)
                            || e.offset + e.size > footer.index_offset))
                    {
                        return fail(error_out, "entry payload out of bounds");
                    }
                    if (e.compression == kPakCompressionNone
                        && e.uncompressed_size != e.size)
                    {
                        return fail(error_out,
                                    "uncompressed_size mismatch for NONE entry");
                    }
                    // Unknown e.compression is deliberately NOT rejected here:
                    // it fails that single entry at open() time.
                }
            }
            else if (tag == kPakSectionPath)
            {
                if (seen_path)
                    return fail(error_out, "duplicate PATH section");
                if (!seen_entb)
                    return fail(error_out, "PATH section before ENTB");
                seen_path = true;

                const std::size_t section_end =
                    static_cast<std::size_t>(index.size() - r.remaining()
                                             + size);
                while (index.size() - r.remaining() < section_end)
                {
                    PakPathRow row;
                    if (!r.str(row.vpath, VirtualPath::kMaxLength))
                        return fail(error_out, "PATH row truncated");
                    if (!readUuid(r, row.id))
                        return fail(error_out, "PATH uuid truncated");
                    if (auto verr = VirtualPath::validateRelative(row.vpath))
                        return fail(error_out,
                                    std::format("non-canonical vpath '{}'",
                                                row.vpath));
                    if (!out.paths.empty()
                        && !(out.paths.back().vpath < row.vpath))
                    {
                        return fail(error_out,
                                    "PATH not strictly byte-ascending");
                    }
                    const auto it = std::lower_bound(
                        out.entries.begin(), out.entries.end(), row.id,
                        [](const PakEntry& e, const asset_id_t& id)
                        {
                            return e.id < id;
                        });
                    if (it == out.entries.end() || !(it->id == row.id))
                        return fail(error_out,
                                    std::format("PATH row '{}' references an "
                                                "unknown uuid", row.vpath));
                    out.paths.push_back(std::move(row));
                }
                if (index.size() - r.remaining() != section_end)
                    return fail(error_out, "PATH section size mismatch");
            }
            else
            {
                // Unknown section: skip (forward compatibility — 'META',
                // 'ALIA', 'SIGN' are reserved seams).
                std::vector<std::byte> skip(static_cast<std::size_t>(size));
                if (size > 0 && !r.bytes(skip.data(), skip.size()))
                    return fail(error_out, "unknown section truncated");
            }
        }

        if (!seen_entb)
            return fail(error_out, "missing ENTB section");
        return true;
    }
}
