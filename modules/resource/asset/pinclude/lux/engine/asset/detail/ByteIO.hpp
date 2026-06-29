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
 * Lives under `pinclude/.../detail` so only the asset module and its own test
 * targets see it (pinclude headers are not installed).
 */

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lux::asset::detail
{
    /// Little-endian, no-padding writer. Owns the output buffer: codecs
    /// `reserve()` an estimate, append primitives, then `std::move(w).take()`
    /// the finished blob.
    class ByteWriter
    {
    public:
        void reserve(std::size_t n) { buf_.reserve(n); }
        [[nodiscard]] std::size_t size() const noexcept { return buf_.size(); }

        void bytes(const void* p, std::size_t n)
        {
            const auto* s = static_cast<const std::byte*>(p);
            buf_.insert(buf_.end(), s, s + n);
        }
        void u8 (std::uint8_t  v) { buf_.push_back(static_cast<std::byte>(v)); }
        void u16(std::uint16_t v) { const std::uint8_t b[2]{ byte_(v,0), byte_(v,8) }; bytes(b, 2); }
        void u32(std::uint32_t v) { const std::uint8_t b[4]{ byte_(v,0), byte_(v,8), byte_(v,16), byte_(v,24) }; bytes(b, 4); }
        void u64(std::uint64_t v) { std::uint8_t b[8]; for (int i = 0; i < 8; ++i) b[i] = byte_(v, 8*i); bytes(b, 8); }
        void i32(std::int32_t  v) { u32(static_cast<std::uint32_t>(v)); }
        void f32(float v)         { u32(std::bit_cast<std::uint32_t>(v)); }
        void str(std::string_view s)
        {
            u32(static_cast<std::uint32_t>(s.size()));
            if (!s.empty()) bytes(s.data(), s.size());
        }

        [[nodiscard]] std::vector<std::byte> take() && noexcept { return std::move(buf_); }

    private:
        template <class T>
        static std::uint8_t byte_(T v, int shift) noexcept
        {
            return static_cast<std::uint8_t>(v >> shift);
        }
        std::vector<std::byte> buf_;
    };

    /// Bounds-checked little-endian reader over a borrowed byte span. Sticky
    /// failure: once any read fails (EOF / limit), `ok()` stays false and every
    /// subsequent read short-circuits, so callers can chain `if (!c.u32(x))
    /// return false;` and surface the first error via the optional `err` sink.
    class ByteReader
    {
    public:
        ByteReader(std::span<const std::byte> data, std::string* err) noexcept
            : data_(data), err_(err) {}

        [[nodiscard]] bool ok() const noexcept { return !fail_; }
        [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - pos_; }

        void fail(const char* msg) noexcept
        {
            if (!fail_) { fail_ = true; if (err_) *err_ = msg; }
        }

        bool bytes(void* dst, std::size_t n) noexcept
        {
            if (fail_) return false;
            if (remaining() < n) { fail("unexpected EOF"); return false; }
            std::memcpy(dst, data_.data() + pos_, n);
            pos_ += n;
            return true;
        }
        bool u8(std::uint8_t& v) noexcept { return bytes(&v, 1); }
        bool u16(std::uint16_t& v) noexcept
        {
            std::uint8_t b[2];
            if (!bytes(b, 2)) return false;
            v = static_cast<std::uint16_t>(b[0])
              | static_cast<std::uint16_t>(static_cast<std::uint16_t>(b[1]) << 8);
            return true;
        }
        bool u32(std::uint32_t& v) noexcept
        {
            std::uint8_t b[4];
            if (!bytes(b, 4)) return false;
            v = static_cast<std::uint32_t>(b[0])
              | (static_cast<std::uint32_t>(b[1]) << 8)
              | (static_cast<std::uint32_t>(b[2]) << 16)
              | (static_cast<std::uint32_t>(b[3]) << 24);
            return true;
        }
        bool u64(std::uint64_t& v) noexcept
        {
            std::uint8_t b[8];
            if (!bytes(b, 8)) return false;
            v = 0;
            for (int i = 0; i < 8; ++i)
                v |= static_cast<std::uint64_t>(b[i]) << (8 * i);
            return true;
        }
        bool i32(std::int32_t& v) noexcept
        {
            std::uint32_t u;
            if (!u32(u)) return false;
            v = static_cast<std::int32_t>(u);
            return true;
        }
        bool f32(float& v) noexcept
        {
            std::uint32_t u;
            if (!u32(u)) return false;
            v = std::bit_cast<float>(u);
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
        std::span<const std::byte> data_;
        std::size_t                pos_  = 0;
        std::string*               err_  = nullptr;
        bool                       fail_ = false;
    };

} // namespace lux::asset::detail
