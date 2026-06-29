#include <lux/engine/serialize/Archive.hpp>

#include <array>
#include <cassert>
#include <stdexcept>

namespace lux::serialize
{

    // ─────────────────────────────────────────────────────────────────
    //  ArchiveWriter
    // ─────────────────────────────────────────────────────────────────

    void ArchiveWriter::writeBytes(const void* p, std::size_t n)
    {
        if (n == 0)
            return;
        const auto old_size = sink_->size();
        sink_->resize(old_size + n);
        std::memcpy(sink_->data() + old_size, p, n);
    }

    void ArchiveWriter::writeString(std::string_view s)
    {
        // Length is a u32 in bytes; strings over 4 GiB are not supported.
        // (No real file will hit that, but assert in debug to catch the
        // pathological case of writing an unbounded string by accident.)
        assert(s.size() <= static_cast<std::size_t>(UINT32_MAX) &&
               "ArchiveWriter::writeString: string longer than UINT32_MAX bytes");
        const auto len = static_cast<std::uint32_t>(s.size());
        writePod(len);
        writeBytes(s.data(), len);
    }

    void ArchiveWriter::writeUuid(const uuids::uuid& u)
    {
        const auto bytes = u.as_bytes();
        // `as_bytes()` returns a `std::span<const std::byte, 16>`. We commit
        // the 16-byte representation verbatim; stduuid's `from_bytes()` on
        // the read side accepts the same layout.
        writeBytes(bytes.data(), bytes.size());
    }

    std::size_t ArchiveWriter::reserveU32()
    {
        const std::size_t offset = sink_->size();
        const std::uint32_t zero = 0;
        writePod(zero);
        return offset;
    }

    void ArchiveWriter::patchU32At(std::size_t offset, std::uint32_t value)
    {
        assert(offset + sizeof(std::uint32_t) <= sink_->size() &&
               "ArchiveWriter::patchU32At: offset past end of sink");
        std::memcpy(sink_->data() + offset, &value, sizeof(std::uint32_t));
    }

    // ─────────────────────────────────────────────────────────────────
    //  ArchiveReader
    // ─────────────────────────────────────────────────────────────────

    void ArchiveReader::readBytes(void* p, std::size_t n)
    {
        if (n == 0)
            return;
        if (cur_ + n > end_)
            throw std::out_of_range(
                "ArchiveReader::readBytes: read past end of buffer");
        std::memcpy(p, cur_, n);
        cur_ += n;
    }

    std::string ArchiveReader::readString()
    {
        const auto len = readPod<std::uint32_t>();
        std::string out(len, '\0');
        if (len > 0)
            readBytes(out.data(), len);
        return out;
    }

    uuids::uuid ArchiveReader::readUuid()
    {
        // stduuid's uuid ctor takes `std::array<uint8_t, 16>` (its
        // `value_type` is the byte type). `as_bytes()` returns
        // `std::span<const std::byte>` for the same layout; we shuttle
        // the 16 bytes through a uint8_t array to satisfy the API.
        std::array<std::uint8_t, 16> raw{};
        readBytes(raw.data(), raw.size());
        return uuids::uuid(raw);
    }

    void ArchiveReader::skip(std::size_t n)
    {
        if (cur_ + n > end_)
            throw std::out_of_range(
                "ArchiveReader::skip: skip past end of buffer");
        cur_ += n;
    }

} // namespace lux::serialize
