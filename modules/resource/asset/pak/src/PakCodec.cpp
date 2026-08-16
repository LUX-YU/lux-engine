#include <lux/engine/resource/asset/PakCodec.hpp>
#include <lux/engine/resource/asset/VirtualPath.hpp>
#include <lux/engine/core/serialization/ByteIO.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <system_error>
#include <unordered_set>

#include <lux/engine/platform/FormatCompat.h>

namespace lux::asset::detail
{
    using lux::core::serialization::ByteReader;
    using lux::core::serialization::ByteWriter;

    namespace
    {
        constexpr std::size_t kHeaderBytes = 256u;
        constexpr std::size_t kPageHeaderBytes = 24u;
        constexpr std::size_t kPagePayloadBytes = kPakPageSize - kPageHeaderBytes;
        constexpr std::size_t kMaximumPathBytes = 2048u;

        struct PageNode final
        {
            PakPage page{};
            std::uint64_t offset{0u};
            lux::cxx::algorithm::Sha256Digest digest;
            asset_id_t maximum_id{};
            std::string maximum_path;
        };

        struct TreeRoot final
        {
            std::uint64_t offset{0u};
            lux::cxx::algorithm::Sha256Digest digest;
        };

        bool fail(std::string* error_out, std::string message)
        {
            if (error_out)
                *error_out = std::move(message);
            return false;
        }

        void writeUuid(ByteWriter& writer, const asset_id_t& id)
        {
            const auto bytes = id.as_bytes();
            writer.bytes(bytes.data(), bytes.size());
        }

        bool readUuid(ByteReader& reader, asset_id_t& id)
        {
            std::array<std::uint8_t, 16> bytes{};
            if (!reader.bytes(bytes.data(), bytes.size()))
                return false;
            id = asset_id_t{bytes};
            return true;
        }

        void writeDigest(
            ByteWriter& writer,
            const lux::cxx::algorithm::Sha256Digest& digest)
        {
            writer.bytes(digest.data(), digest.size());
        }

        bool readDigest(
            ByteReader& reader,
            lux::cxx::algorithm::Sha256Digest& digest)
        {
            return reader.bytes(digest.data(), digest.size());
        }

        void writePageHeader(ByteWriter& writer, const PakPageHeader& header)
        {
            writer.u32(header.magic);
            writer.u16(header.version);
            writer.u8(static_cast<std::uint8_t>(header.kind));
            writer.u8(header.level);
            writer.u16(header.count);
            writer.u16(header.reserved);
            writer.u32(header.used_bytes);
            writer.u64(header.next_leaf_offset);
        }

        bool readPageHeader(ByteReader& reader, PakPageHeader& header)
        {
            std::uint8_t kind = 0u;
            if (!reader.u32(header.magic)
                || !reader.u16(header.version)
                || !reader.u8(kind)
                || !reader.u8(header.level)
                || !reader.u16(header.count)
                || !reader.u16(header.reserved)
                || !reader.u32(header.used_bytes)
                || !reader.u64(header.next_leaf_offset))
            {
                return false;
            }
            if (kind > static_cast<std::uint8_t>(EPakPageKind::PATH_INTERNAL))
                return false;
            header.kind = static_cast<EPakPageKind>(kind);
            return true;
        }

        PakPage makePage(
            EPakPageKind kind,
            std::uint8_t level,
            std::uint16_t count,
            std::uint64_t next_leaf_offset,
            std::vector<std::byte> payload)
        {
            PakPage page{};
            PakPageHeader header{
                kPakPageMagic,
                static_cast<std::uint16_t>(kPakVersion),
                kind,
                level,
                count,
                0u,
                static_cast<std::uint32_t>(kPageHeaderBytes + payload.size()),
                next_leaf_offset};
            ByteWriter writer;
            writer.reserve(header.used_bytes);
            writePageHeader(writer, header);
            if (!payload.empty())
                writer.bytes(payload.data(), payload.size());
            auto bytes = std::move(writer).take();
            std::memcpy(page.data(), bytes.data(), bytes.size());
            return page;
        }

        std::vector<std::byte> encodeEntryRows(
            std::span<const PakEntry> rows)
        {
            ByteWriter writer;
            for (const auto& row : rows)
            {
                writeUuid(writer, row.id);
                writer.u64(row.offset);
                writer.u64(row.size);
                writer.u64(row.uncompressed_size);
                writer.u32(row.asset_magic);
                writer.u8(row.compression);
                writer.u8(row.flags);
                writer.u16(0u);
                writeDigest(writer, row.content_digest);
                writer.u16(static_cast<std::uint16_t>(row.vpath.size()));
                if (!row.vpath.empty())
                    writer.bytes(row.vpath.data(), row.vpath.size());
            }
            return std::move(writer).take();
        }

        std::vector<std::byte> encodeEntryChildren(
            std::span<const PageNode> children)
        {
            ByteWriter writer;
            for (const auto& child : children)
            {
                writeUuid(writer, child.maximum_id);
                writer.u64(child.offset);
                writeDigest(writer, child.digest);
            }
            return std::move(writer).take();
        }

        std::vector<std::byte> encodePathRows(
            std::span<const PakPathRow> rows)
        {
            ByteWriter writer;
            for (const auto& row : rows)
            {
                writer.u16(static_cast<std::uint16_t>(row.vpath.size()));
                if (!row.vpath.empty())
                    writer.bytes(row.vpath.data(), row.vpath.size());
                writeUuid(writer, row.id);
            }
            return std::move(writer).take();
        }

        std::vector<std::byte> encodePathChildren(
            std::span<const PageNode> children)
        {
            ByteWriter writer;
            for (const auto& child : children)
            {
                writer.u16(
                    static_cast<std::uint16_t>(child.maximum_path.size()));
                if (!child.maximum_path.empty())
                {
                    writer.bytes(
                        child.maximum_path.data(),
                        child.maximum_path.size());
                }
                writer.u64(child.offset);
                writeDigest(writer, child.digest);
            }
            return std::move(writer).take();
        }

        void finishPage(PageNode& node)
        {
            node.digest = lux::cxx::algorithm::Sha256::hash(
                std::span<const std::byte>{node.page.data(), node.page.size()});
        }

        bool appendEntryTree(
            const std::vector<PakEntry>& entries,
            std::uint64_t& next_offset,
            std::vector<PageNode>& pages,
            TreeRoot& root,
            std::string* error_out)
        {
            std::vector<std::pair<std::size_t, std::size_t>> chunks;
            for (std::size_t begin = 0u; begin < entries.size();)
            {
                std::size_t end = begin;
                std::size_t bytes = 0u;
                while (end < entries.size())
                {
                    const auto row_bytes = 82u + entries[end].vpath.size();
                    if (row_bytes > kPagePayloadBytes)
                        return fail(error_out, "Pak entry path does not fit an index page");
                    if (end != begin && bytes + row_bytes > kPagePayloadBytes)
                        break;
                    bytes += row_bytes;
                    ++end;
                }
                chunks.emplace_back(begin, end);
                begin = end;
            }
            if (chunks.empty())
                chunks.emplace_back(0u, 0u);

            std::vector<PageNode> level;
            level.reserve(chunks.size());
            for (const auto [begin, end] : chunks)
            {
                PageNode node;
                node.offset = next_offset;
                next_offset += kPakPageSize;
                if (end > begin)
                    node.maximum_id = entries[end - 1u].id;
                level.push_back(std::move(node));
            }
            for (std::size_t i = 0u; i < level.size(); ++i)
            {
                const auto [begin, end] = chunks[i];
                const auto next = i + 1u < level.size()
                    ? level[i + 1u].offset
                    : 0u;
                level[i].page = makePage(
                    EPakPageKind::ENTRY_LEAF,
                    0u,
                    static_cast<std::uint16_t>(end - begin),
                    next,
                    encodeEntryRows(std::span<const PakEntry>{entries}.subspan(
                        begin, end - begin)));
                finishPage(level[i]);
                pages.push_back(level[i]);
            }

            std::uint8_t tree_level = 1u;
            while (level.size() > 1u)
            {
                constexpr std::size_t children_per_page =
                    kPagePayloadBytes / 56u;
                std::vector<PageNode> parent;
                for (std::size_t begin = 0u; begin < level.size();)
                {
                    const auto count = std::min(
                        children_per_page, level.size() - begin);
                    PageNode node;
                    node.offset = next_offset;
                    next_offset += kPakPageSize;
                    node.maximum_id = level[begin + count - 1u].maximum_id;
                    node.page = makePage(
                        EPakPageKind::ENTRY_INTERNAL,
                        tree_level,
                        static_cast<std::uint16_t>(count),
                        0u,
                        encodeEntryChildren(
                            std::span<const PageNode>{level}.subspan(
                                begin, count)));
                    finishPage(node);
                    pages.push_back(node);
                    parent.push_back(std::move(node));
                    begin += count;
                }
                level = std::move(parent);
                ++tree_level;
            }
            root = TreeRoot{level.front().offset, level.front().digest};
            return true;
        }

        bool appendPathTree(
            const std::vector<PakPathRow>& rows,
            std::uint64_t& next_offset,
            std::vector<PageNode>& pages,
            TreeRoot& root,
            std::string* error_out)
        {
            std::vector<std::pair<std::size_t, std::size_t>> chunks;
            for (std::size_t begin = 0u; begin < rows.size();)
            {
                std::size_t end = begin;
                std::size_t bytes = 0u;
                while (end < rows.size())
                {
                    const auto row_bytes = 18u + rows[end].vpath.size();
                    if (row_bytes > kPagePayloadBytes)
                        return fail(error_out, "Pak path does not fit an index page");
                    if (end != begin && bytes + row_bytes > kPagePayloadBytes)
                        break;
                    bytes += row_bytes;
                    ++end;
                }
                chunks.emplace_back(begin, end);
                begin = end;
            }
            if (chunks.empty())
                chunks.emplace_back(0u, 0u);

            std::vector<PageNode> level;
            level.reserve(chunks.size());
            for (const auto [begin, end] : chunks)
            {
                PageNode node;
                node.offset = next_offset;
                next_offset += kPakPageSize;
                if (end > begin)
                    node.maximum_path = rows[end - 1u].vpath;
                level.push_back(std::move(node));
            }
            for (std::size_t i = 0u; i < level.size(); ++i)
            {
                const auto [begin, end] = chunks[i];
                const auto next = i + 1u < level.size()
                    ? level[i + 1u].offset
                    : 0u;
                level[i].page = makePage(
                    EPakPageKind::PATH_LEAF,
                    0u,
                    static_cast<std::uint16_t>(end - begin),
                    next,
                    encodePathRows(std::span<const PakPathRow>{rows}.subspan(
                        begin, end - begin)));
                finishPage(level[i]);
                pages.push_back(level[i]);
            }

            std::uint8_t tree_level = 1u;
            while (level.size() > 1u)
            {
                std::vector<PageNode> parent;
                for (std::size_t begin = 0u; begin < level.size();)
                {
                    std::size_t count = 0u;
                    std::size_t bytes = 0u;
                    while (begin + count < level.size())
                    {
                        const auto row_bytes =
                            42u + level[begin + count].maximum_path.size();
                        if (row_bytes > kPagePayloadBytes)
                            return fail(error_out, "Pak path key does not fit an internal page");
                        if (count != 0u && bytes + row_bytes > kPagePayloadBytes)
                            break;
                        bytes += row_bytes;
                        ++count;
                    }
                    PageNode node;
                    node.offset = next_offset;
                    next_offset += kPakPageSize;
                    node.maximum_path =
                        level[begin + count - 1u].maximum_path;
                    node.page = makePage(
                        EPakPageKind::PATH_INTERNAL,
                        tree_level,
                        static_cast<std::uint16_t>(count),
                        0u,
                        encodePathChildren(
                            std::span<const PageNode>{level}.subspan(
                                begin, count)));
                    finishPage(node);
                    pages.push_back(node);
                    parent.push_back(std::move(node));
                    begin += count;
                }
                level = std::move(parent);
                ++tree_level;
            }
            root = TreeRoot{level.front().offset, level.front().digest};
            return true;
        }

        std::array<std::byte, kHeaderBytes> encodeHeader(const PakHeader& header)
        {
            ByteWriter writer;
            writer.reserve(kHeaderBytes);
            writer.bytes(header.magic, sizeof(header.magic));
            writer.u32(header.endian_tag);
            writer.u32(header.version);
            writer.u32(header.page_size);
            writer.u32(header.flags);
            writer.u64(header.entry_root_offset);
            writer.u64(header.path_root_offset);
            writer.u64(header.entry_count);
            writer.u64(header.path_count);
            writer.u64(header.index_page_count);
            writer.u64(header.payload_end);
            writer.u32(header.mount_hint_size);
            writer.bytes(header.mount_hint, sizeof(header.mount_hint));
            writeDigest(writer, header.entry_root_digest);
            writeDigest(writer, header.path_root_digest);
            writer.bytes(header.reserved, sizeof(header.reserved));
            auto bytes = std::move(writer).take();
            std::array<std::byte, kHeaderBytes> output{};
            std::memcpy(output.data(), bytes.data(), bytes.size());
            return output;
        }

        bool copyPayload(
            const PakWriteEntry& input,
            std::ofstream& output,
            std::uint64_t& size,
            lux::cxx::algorithm::Sha256Digest& digest)
        {
            lux::cxx::algorithm::Sha256 hasher;
            size = 0u;
            if (!input.source_bytes.empty())
            {
                const auto bytes = input.source_bytes.view();
                output.write(
                    reinterpret_cast<const char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
                if (!output)
                    return false;
                hasher.update(bytes);
                size = bytes.size();
            }
            else
            {
                std::ifstream source(input.source_file, std::ios::binary);
                if (!source)
                    return false;
                std::array<std::byte, 64u * 1024u> buffer{};
                while (source)
                {
                    source.read(
                        reinterpret_cast<char*>(buffer.data()),
                        static_cast<std::streamsize>(buffer.size()));
                    const auto count = source.gcount();
                    if (count <= 0)
                        break;
                    const auto bytes = std::span<const std::byte>{
                        buffer.data(), static_cast<std::size_t>(count)};
                    output.write(
                        reinterpret_cast<const char*>(bytes.data()), count);
                    if (!output)
                        return false;
                    hasher.update(bytes);
                    size += static_cast<std::uint64_t>(count);
                }
                if (source.bad())
                    return false;
            }
            digest = hasher.digest();
            return true;
        }

        bool decodePagePrelude(
            const PakPage& page,
            EPakPageKind expected,
            PakPageHeader& header,
            ByteReader& reader,
            std::string* error_out)
        {
            ByteReader prelude(
                std::span<const std::byte>{page.data(), page.size()}, error_out);
            if (!readPageHeader(prelude, header))
                return fail(error_out, "invalid Pak index page header");
            if (header.magic != kPakPageMagic
                || header.version != kPakVersion
                || header.kind != expected
                || header.used_bytes < kPageHeaderBytes
                || header.used_bytes > kPakPageSize)
            {
                return fail(error_out, "invalid Pak index page contract");
            }
            reader = ByteReader(
                std::span<const std::byte>{page.data(), header.used_bytes}
                    .subspan(kPageHeaderBytes),
                error_out);
            return true;
        }
    }

    bool writePakFile(
        const std::filesystem::path& out_pak,
        std::vector<PakWriteEntry> entries,
        std::string_view mount_hint,
        std::string* error_out)
    {
        if (!VirtualPath::isLegalRoot(mount_hint))
            return fail(error_out, lux::format("illegal mount hint '{}'", mount_hint));
        if (mount_hint.size() > kPakMountHintBytes)
            return fail(error_out, "Pak mount hint is too long");
        if (entries.size() > kMaxPakEntries)
            return fail(error_out, "too many Pak entries");

        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b)
        {
            return a.id < b.id;
        });
        for (std::size_t i = 0u; i < entries.size(); ++i)
        {
            if (i != 0u && entries[i - 1u].id == entries[i].id)
                return fail(error_out, "duplicate Pak entry UUID");
            if (entries[i].vpath.size() > kMaximumPathBytes
                || entries[i].vpath.size() > std::numeric_limits<std::uint16_t>::max())
            {
                return fail(error_out, "Pak virtual path is too long");
            }
            if (!entries[i].vpath.empty())
            {
                if (auto path_error = VirtualPath::validateRelative(entries[i].vpath))
                {
                    return fail(
                        error_out,
                        lux::format(
                            "non-canonical vpath '{}' (err={})",
                            entries[i].vpath,
                            static_cast<int>(*path_error)));
                }
            }
        }

        std::vector<PakPathRow> path_rows;
        path_rows.reserve(entries.size());
        for (const auto& entry : entries)
        {
            if (!entry.vpath.empty())
                path_rows.push_back(PakPathRow{entry.vpath, entry.id});
        }
        std::sort(path_rows.begin(), path_rows.end(), [](const auto& a, const auto& b)
        {
            return a.vpath < b.vpath;
        });
        for (std::size_t i = 1u; i < path_rows.size(); ++i)
        {
            if (path_rows[i - 1u].vpath == path_rows[i].vpath)
                return fail(error_out, "duplicate Pak virtual path");
        }

        std::error_code ec;
        std::filesystem::create_directories(out_pak.parent_path(), ec);
        if (ec)
            return fail(error_out, "cannot create Pak output directory");
        auto temporary = out_pak;
        temporary += ".writing";
        std::filesystem::remove(temporary, ec);
        ec.clear();

        std::ofstream output(
            temporary,
            std::ios::binary | std::ios::trunc);
        if (!output)
            return fail(error_out, "cannot create temporary Pak");
        std::array<std::byte, kHeaderBytes> empty_header{};
        output.write(
            reinterpret_cast<const char*>(empty_header.data()),
            static_cast<std::streamsize>(empty_header.size()));

        std::vector<PakEntry> cooked_entries;
        cooked_entries.reserve(entries.size());
        for (const auto& input : entries)
        {
            auto cursor = static_cast<std::uint64_t>(output.tellp());
            const auto aligned = (cursor + kPakPayloadAlign - 1u)
                & ~(kPakPayloadAlign - 1u);
            std::array<std::byte, kPakPayloadAlign> padding{};
            if (aligned != cursor)
            {
                output.write(
                    reinterpret_cast<const char*>(padding.data()),
                    static_cast<std::streamsize>(aligned - cursor));
            }
            PakEntry entry;
            entry.id = input.id;
            entry.offset = aligned;
            entry.asset_magic = input.asset_magic;
            entry.compression = kPakCompressionNone;
            entry.vpath = input.vpath;
            if (!copyPayload(
                    input,
                    output,
                    entry.size,
                    entry.content_digest))
            {
                output.close();
                std::filesystem::remove(temporary, ec);
                return fail(error_out, "cannot read or write Pak payload");
            }
            entry.uncompressed_size = entry.size;
            cooked_entries.push_back(std::move(entry));
        }

        const auto payload_end = static_cast<std::uint64_t>(output.tellp());
        auto next_page_offset = payload_end;
        const auto remainder = next_page_offset % kPakPageSize;
        if (remainder != 0u)
        {
            const auto count = kPakPageSize - remainder;
            std::vector<std::byte> padding(count);
            output.write(
                reinterpret_cast<const char*>(padding.data()),
                static_cast<std::streamsize>(padding.size()));
            next_page_offset += count;
        }

        std::vector<PageNode> pages;
        TreeRoot entry_root;
        TreeRoot path_root;
        if (!appendEntryTree(
                cooked_entries,
                next_page_offset,
                pages,
                entry_root,
                error_out)
            || !appendPathTree(
                path_rows,
                next_page_offset,
                pages,
                path_root,
                error_out))
        {
            output.close();
            std::filesystem::remove(temporary, ec);
            return false;
        }
        for (const auto& page : pages)
        {
            output.seekp(static_cast<std::streamoff>(page.offset), std::ios::beg);
            output.write(
                reinterpret_cast<const char*>(page.page.data()),
                static_cast<std::streamsize>(page.page.size()));
        }

        PakHeader header{};
        std::copy(std::begin(kPakFileMagic), std::end(kPakFileMagic), header.magic);
        header.endian_tag = kPakEndianTag;
        header.version = kPakVersion;
        header.page_size = kPakPageSize;
        header.entry_root_offset = entry_root.offset;
        header.path_root_offset = path_root.offset;
        header.entry_count = cooked_entries.size();
        header.path_count = path_rows.size();
        header.index_page_count = pages.size();
        header.payload_end = payload_end;
        header.mount_hint_size = static_cast<std::uint32_t>(mount_hint.size());
        std::memcpy(header.mount_hint, mount_hint.data(), mount_hint.size());
        header.entry_root_digest = entry_root.digest;
        header.path_root_digest = path_root.digest;
        const auto header_bytes = encodeHeader(header);
        output.seekp(0, std::ios::beg);
        output.write(
            reinterpret_cast<const char*>(header_bytes.data()),
            static_cast<std::streamsize>(header_bytes.size()));
        output.flush();
        if (!output)
        {
            output.close();
            std::filesystem::remove(temporary, ec);
            return fail(error_out, "cannot finalize Pak");
        }
        output.close();

        std::filesystem::rename(temporary, out_pak, ec);
        if (ec)
        {
            std::error_code remove_ec;
            std::filesystem::remove(out_pak, remove_ec);
            ec.clear();
            std::filesystem::rename(temporary, out_pak, ec);
        }
        if (ec)
        {
            std::filesystem::remove(temporary, ec);
            return fail(error_out, "cannot atomically publish Pak");
        }
        return true;
    }

    bool readPakHeader(
        std::istream& stream,
        std::uint64_t file_size,
        PakHeader& output,
        std::string* error_out)
    {
        output = {};
        if (file_size < kHeaderBytes + 2u * kPakPageSize)
            return fail(error_out, "Pak is too small");
        std::array<std::byte, kHeaderBytes> bytes{};
        stream.clear();
        stream.seekg(0, std::ios::beg);
        if (!stream.read(
                reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size())))
        {
            return fail(error_out, "cannot read Pak header");
        }
        ByteReader reader(bytes, error_out);
        if (!reader.bytes(output.magic, sizeof(output.magic))
            || !reader.u32(output.endian_tag)
            || !reader.u32(output.version)
            || !reader.u32(output.page_size)
            || !reader.u32(output.flags)
            || !reader.u64(output.entry_root_offset)
            || !reader.u64(output.path_root_offset)
            || !reader.u64(output.entry_count)
            || !reader.u64(output.path_count)
            || !reader.u64(output.index_page_count)
            || !reader.u64(output.payload_end)
            || !reader.u32(output.mount_hint_size)
            || !reader.bytes(output.mount_hint, sizeof(output.mount_hint))
            || !readDigest(reader, output.entry_root_digest)
            || !readDigest(reader, output.path_root_digest)
            || !reader.bytes(output.reserved, sizeof(output.reserved)))
        {
            return fail(error_out, "truncated Pak header");
        }
        if (!std::equal(
                std::begin(output.magic),
                std::end(output.magic),
                std::begin(kPakFileMagic))
            || output.endian_tag != kPakEndianTag
            || output.version != kPakVersion
            || output.page_size != kPakPageSize
            || output.mount_hint_size > kPakMountHintBytes
            || output.entry_count > kMaxPakEntries
            || output.path_count > output.entry_count
            || output.index_page_count < 2u
            || output.payload_end < kHeaderBytes
            || output.entry_root_offset < output.payload_end
            || output.path_root_offset < output.payload_end
            || output.entry_root_offset > file_size - kPakPageSize
            || output.path_root_offset > file_size - kPakPageSize)
        {
            return fail(error_out, "invalid Pak v2 header contract");
        }
        return true;
    }

    bool readPakPage(
        std::istream& stream,
        std::uint64_t file_size,
        std::uint64_t offset,
        PakPage& output,
        std::string* error_out)
    {
        if (offset % kPakPageSize != 0u || offset > file_size - kPakPageSize)
            return fail(error_out, "Pak index page offset is out of bounds");
        stream.clear();
        stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!stream.read(
                reinterpret_cast<char*>(output.data()),
                static_cast<std::streamsize>(output.size())))
        {
            return fail(error_out, "cannot read Pak index page");
        }
        const auto header = pakPageHeader(output);
        if (header.magic != kPakPageMagic
            || header.version != kPakVersion
            || header.used_bytes < kPageHeaderBytes
            || header.used_bytes > kPakPageSize)
        {
            return fail(error_out, "invalid Pak index page header");
        }
        return true;
    }

    bool verifyPakPageDigest(
        const PakPage& page,
        const lux::cxx::algorithm::Sha256Digest& expected) noexcept
    {
        return lux::cxx::algorithm::Sha256::hash(
            std::span<const std::byte>{page.data(), page.size()}) == expected;
    }

    PakPageHeader pakPageHeader(const PakPage& page) noexcept
    {
        PakPageHeader header{};
        ByteReader reader(
            std::span<const std::byte>{page.data(), page.size()}, nullptr);
        static_cast<void>(readPageHeader(reader, header));
        return header;
    }

    bool decodeEntryLeaf(
        const PakPage& page,
        std::vector<PakEntry>& output,
        std::string* error_out)
    {
        output.clear();
        PakPageHeader header;
        ByteReader reader({}, error_out);
        if (!decodePagePrelude(
                page, EPakPageKind::ENTRY_LEAF, header, reader, error_out))
            return false;
        output.reserve(header.count);
        for (std::uint16_t i = 0u; i < header.count; ++i)
        {
            PakEntry entry;
            std::uint16_t reserved = 0u;
            std::uint16_t path_size = 0u;
            if (!readUuid(reader, entry.id)
                || !reader.u64(entry.offset)
                || !reader.u64(entry.size)
                || !reader.u64(entry.uncompressed_size)
                || !reader.u32(entry.asset_magic)
                || !reader.u8(entry.compression)
                || !reader.u8(entry.flags)
                || !reader.u16(reserved)
                || !readDigest(reader, entry.content_digest)
                || !reader.u16(path_size)
                || path_size > kMaximumPathBytes)
            {
                return fail(error_out, "invalid Pak entry leaf row");
            }
            entry.vpath.resize(path_size);
            if (path_size != 0u
                && !reader.bytes(entry.vpath.data(), entry.vpath.size()))
            {
                return fail(error_out, "truncated Pak entry path");
            }
            output.push_back(std::move(entry));
        }
        return true;
    }

    bool decodeEntryInternal(
        const PakPage& page,
        std::vector<PakEntryChild>& output,
        std::string* error_out)
    {
        output.clear();
        PakPageHeader header;
        ByteReader reader({}, error_out);
        if (!decodePagePrelude(
                page, EPakPageKind::ENTRY_INTERNAL, header, reader, error_out))
            return false;
        if (header.count == 0u)
            return fail(error_out, "empty Pak entry internal page");
        output.reserve(header.count);
        for (std::uint16_t i = 0u; i < header.count; ++i)
        {
            PakEntryChild child;
            if (!readUuid(reader, child.maximum_key)
                || !reader.u64(child.offset)
                || !readDigest(reader, child.digest))
            {
                return fail(error_out, "invalid Pak entry child row");
            }
            output.push_back(std::move(child));
        }
        return true;
    }

    bool decodePathLeaf(
        const PakPage& page,
        std::vector<PakPathRow>& output,
        std::string* error_out)
    {
        output.clear();
        PakPageHeader header;
        ByteReader reader({}, error_out);
        if (!decodePagePrelude(
                page, EPakPageKind::PATH_LEAF, header, reader, error_out))
            return false;
        output.reserve(header.count);
        for (std::uint16_t i = 0u; i < header.count; ++i)
        {
            PakPathRow row;
            std::uint16_t path_size = 0u;
            if (!reader.u16(path_size) || path_size > kMaximumPathBytes)
                return fail(error_out, "invalid Pak path leaf row");
            row.vpath.resize(path_size);
            if ((path_size != 0u
                    && !reader.bytes(row.vpath.data(), row.vpath.size()))
                || !readUuid(reader, row.id))
            {
                return fail(error_out, "truncated Pak path leaf row");
            }
            output.push_back(std::move(row));
        }
        return true;
    }

    bool decodePathInternal(
        const PakPage& page,
        std::vector<PakPathChild>& output,
        std::string* error_out)
    {
        output.clear();
        PakPageHeader header;
        ByteReader reader({}, error_out);
        if (!decodePagePrelude(
                page, EPakPageKind::PATH_INTERNAL, header, reader, error_out))
            return false;
        if (header.count == 0u)
            return fail(error_out, "empty Pak path internal page");
        output.reserve(header.count);
        for (std::uint16_t i = 0u; i < header.count; ++i)
        {
            PakPathChild child;
            std::uint16_t path_size = 0u;
            if (!reader.u16(path_size) || path_size > kMaximumPathBytes)
                return fail(error_out, "invalid Pak path child row");
            child.maximum_key.resize(path_size);
            if ((path_size != 0u
                    && !reader.bytes(
                        child.maximum_key.data(), child.maximum_key.size()))
                || !reader.u64(child.offset)
                || !readDigest(reader, child.digest))
            {
                return fail(error_out, "truncated Pak path child row");
            }
            output.push_back(std::move(child));
        }
        return true;
    }

    bool readAllPakEntries(
        std::istream& stream,
        std::uint64_t file_size,
        const PakHeader& header,
        std::vector<PakEntry>& output,
        std::string* error_out)
    {
        output.clear();
        struct Pending final
        {
            std::uint64_t offset;
            lux::cxx::algorithm::Sha256Digest digest;
        };
        std::vector<Pending> pending{
            Pending{header.entry_root_offset, header.entry_root_digest}};
        std::unordered_set<std::uint64_t> visited;
        while (!pending.empty())
        {
            const auto current = pending.back();
            pending.pop_back();
            if (!visited.insert(current.offset).second)
                return fail(error_out, "cycle in Pak entry tree");
            if (visited.size() > header.index_page_count)
                return fail(error_out, "Pak entry tree exceeds page count");
            PakPage page;
            if (!readPakPage(stream, file_size, current.offset, page, error_out)
                || !verifyPakPageDigest(page, current.digest))
            {
                return fail(error_out, "Pak entry page digest mismatch");
            }
            const auto page_header = pakPageHeader(page);
            if (page_header.kind == EPakPageKind::ENTRY_LEAF)
            {
                std::vector<PakEntry> rows;
                if (!decodeEntryLeaf(page, rows, error_out))
                    return false;
                output.insert(
                    output.end(),
                    std::make_move_iterator(rows.begin()),
                    std::make_move_iterator(rows.end()));
            }
            else if (page_header.kind == EPakPageKind::ENTRY_INTERNAL)
            {
                std::vector<PakEntryChild> children;
                if (!decodeEntryInternal(page, children, error_out))
                    return false;
                for (auto it = children.rbegin(); it != children.rend(); ++it)
                    pending.push_back(Pending{it->offset, it->digest});
            }
            else
            {
                return fail(error_out, "wrong page kind in Pak entry tree");
            }
        }
        if (output.size() != header.entry_count
            || !std::is_sorted(output.begin(), output.end(), [](const auto& a, const auto& b)
            {
                return a.id < b.id;
            }))
        {
            return fail(error_out, "Pak entry tree cardinality or ordering mismatch");
        }
        return true;
    }
} // namespace lux::asset::detail
