#pragma once
/**
 * @file ByteIO.hpp (private)
 * @brief Shared little-endian binary reader/writer for the description codecs.
 *
 * Every `*DescriptionCodec.cpp` used to carry its own byte-identical copy of a
 * `Cursor` reader plus `writeU32` / `writeF32` / ... writers. This collapses
 * them into one `ByteWriter` / `ByteReader` pair (same wire contract: little-
 * endian, no implicit padding). Domain-specific encoders — `Affine3f`,
 * quaternion-unorm16, Script::Type, raw POD arrays — stay in their own codec,
 * built on top of these primitives.
 *
 * This is a domain-neutral binary primitive shared by Runtime codecs and
 * Authoring codecs. Domain wire formats remain in their owning targets.
 */

#include <lux/cxx/binary/Binary.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lux::core::serialization
{
    /// Little-endian, no-padding writer. Owns the output buffer: codecs
    /// `reserve()` an estimate, append primitives, then `std::move(w).take()`
    /// the finished blob.
    class ByteWriter
    {
    public:
        void reserve(std::size_t n)
        {
            if (!writer_.data().empty())
                return;
            std::vector<std::byte> output;
            output.reserve(n);
            writer_ = lux::cxx::BinaryVectorWriter(std::move(output));
        }
        [[nodiscard]] std::size_t size() const noexcept
        {
            return writer_.data().size();
        }

        void bytes(const void* p, std::size_t n)
        {
            const auto* data = static_cast<const std::byte*>(p);
            static_cast<void>(writer_.writeBytes({data, n}));
        }
        void u8(std::uint8_t v) { writeUnsigned(v); }
        void u16(std::uint16_t v) { writeUnsigned(v); }
        void u32(std::uint32_t v) { writeUnsigned(v); }
        void u64(std::uint64_t v) { writeUnsigned(v); }
        void i32(std::int32_t v)
        {
            static_cast<void>(writer_.writeSigned(v));
        }
        void f32(float v)
        {
            static_cast<void>(writer_.writeFloat(
                v,
                lux::cxx::EFloatingPointPolicy::PRESERVE_BITS
            ));
        }
        void str(std::string_view s)
        {
            u32(static_cast<std::uint32_t>(s.size()));
            static_cast<void>(writer_.writeString(s));
        }

        [[nodiscard]] std::vector<std::byte> take() && noexcept
        {
            return std::move(writer_).take();
        }

    private:
        template <class Value>
        void writeUnsigned(Value value)
        {
            static_cast<void>(writer_.writeUnsigned(value));
        }

        lux::cxx::BinaryVectorWriter writer_;
    };

    /// Bounds-checked little-endian reader over a borrowed byte span. Sticky
    /// failure: once any read fails (EOF / limit), `ok()` stays false and every
    /// subsequent read short-circuits, so callers can chain `if (!c.u32(x))
    /// return false;` and surface the first error via the optional `err` sink.
    class ByteReader
    {
    public:
        ByteReader(std::span<const std::byte> data, std::string* err) noexcept
            : reader_(data, data.size()), err_(err) {}

        [[nodiscard]] bool ok() const noexcept
        {
            return !fail_ && reader_.good();
        }
        [[nodiscard]] std::size_t remaining() const noexcept
        {
            return reader_.remaining();
        }

        void fail(const char* msg) noexcept
        {
            if (!fail_) { fail_ = true; if (err_) *err_ = msg; }
        }

        bool bytes(void* dst, std::size_t n) noexcept
        {
            if (fail_) return false;
            std::span<const std::byte> bytes;
            if (!reader_.readBytes(n, bytes))
            {
                fail("unexpected EOF");
                return false;
            }
            std::memcpy(dst, bytes.data(), bytes.size());
            return true;
        }
        bool u8(std::uint8_t& v) noexcept { return readUnsigned(v); }
        bool u16(std::uint16_t& v) noexcept
        {
            return readUnsigned(v);
        }
        bool u32(std::uint32_t& v) noexcept
        {
            return readUnsigned(v);
        }
        bool u64(std::uint64_t& v) noexcept
        {
            return readUnsigned(v);
        }
        bool i32(std::int32_t& v) noexcept
        {
            if (fail_ || !reader_.readSigned(v))
            {
                fail("unexpected EOF");
                return false;
            }
            return true;
        }
        bool f32(float& v) noexcept
        {
            if (fail_ || !reader_.readFloat(
                    v,
                    lux::cxx::EFloatingPointPolicy::PRESERVE_BITS
                ))
            {
                fail("unexpected EOF");
                return false;
            }
            return true;
        }
        bool str(std::string& s, std::size_t max_len) noexcept
        {
            std::uint32_t n;
            if (!u32(n)) return false;
            if (n > max_len) { fail("string length exceeds limit"); return false; }
            s.resize(n);
            if (n == 0) return true;
            return bytes(s.data(), n);
        }

    private:
        template <class Value>
        bool readUnsigned(Value& value) noexcept
        {
            if (fail_ || !reader_.readUnsigned(value))
            {
                fail("unexpected EOF");
                return false;
            }
            return true;
        }

        lux::cxx::BinaryReader reader_;
        std::string*           err_  = nullptr;
        bool                   fail_ = false;
    };

} // namespace lux::core::serialization
