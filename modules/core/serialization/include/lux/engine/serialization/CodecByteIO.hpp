#pragma once

#include <lux/engine/serialization/BinaryReader.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace lux::serialization
{
    // Domain codecs with established wires use this convenience facade over
    // the exact LE primitives. It is not a generic object/archive system.
    class ByteWriter final
    {
    public:
        ByteWriter() noexcept : writer_(bytes_) {}

        void reserve(std::size_t bytes) noexcept
        {
            if (failure_)
            {
                return;
            }
            try
            {
                bytes_.reserve(bytes);
            }
            catch (const std::bad_alloc&)
            {
                failure_ = SerializationFailure{
                    ESerializationError::ALLOCATION_FAILURE,
                    writer_.offset()
                };
            }
            catch (const std::length_error&)
            {
                failure_ = SerializationFailure{
                    ESerializationError::LIMIT_EXCEEDED,
                    writer_.offset()
                };
            }
        }
        [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
        [[nodiscard]] bool ok() const noexcept { return !failure_.has_value(); }
        void bytes(const void* data, std::size_t size) noexcept
        {
            const auto* first = static_cast<const std::byte*>(data);
            capture(writer_.writeBytes({first, size}));
        }
        void u8(std::uint8_t value) noexcept { capture(writer_.writeUnsigned(value)); }
        void u16(std::uint16_t value) noexcept { capture(writer_.writeUnsigned(value)); }
        void u32(std::uint32_t value) noexcept { capture(writer_.writeUnsigned(value)); }
        void u64(std::uint64_t value) noexcept { capture(writer_.writeUnsigned(value)); }
        void i32(std::int32_t value) noexcept { capture(writer_.writeSigned(value)); }
        void f32(float value) noexcept { capture(writer_.writeFloat(value)); }
        void str(std::string_view value) noexcept
        {
            if (value.size() > std::numeric_limits<std::uint32_t>::max())
            {
                failure_ = SerializationFailure{
                    ESerializationError::LIMIT_EXCEEDED,
                    writer_.offset()
                };
                return;
            }
            u32(static_cast<std::uint32_t>(value.size()));
            bytes(value.data(), value.size());
        }
        [[nodiscard]] lux::cxx::expected<
            std::vector<std::byte>, SerializationFailure>
        take() && noexcept
        {
            if (failure_)
            {
                return lux::cxx::unexpected<SerializationFailure>(*failure_);
            }
            return std::move(bytes_);
        }

        [[nodiscard]] std::vector<std::byte> takeOrThrow() &&
        {
            if (failure_)
            {
                if (failure_->code ==
                    ESerializationError::ALLOCATION_FAILURE)
                {
                    throw std::bad_alloc{};
                }
                throw std::length_error("binary serialization limit exceeded");
            }
            return std::move(bytes_);
        }

    private:
        void capture(SerializationResult result) noexcept
        {
            if (!result && !failure_)
            {
                failure_ = result.error();
            }
        }

        std::vector<std::byte> bytes_;
        BinaryWriter writer_;
        std::optional<SerializationFailure> failure_;
    };

    class ByteReader final
    {
    public:
        ByteReader(std::span<const std::byte> bytes, std::string* error) noexcept
            : reader_(bytes), error_(error)
        {
        }

        [[nodiscard]] bool ok() const noexcept { return !failed_; }
        [[nodiscard]] std::size_t remaining() const noexcept
        {
            return reader_.remaining();
        }
        void fail(const char* message) noexcept
        {
            if (!failed_)
            {
                failed_ = true;
                if (error_ != nullptr)
                {
                    try
                    {
                        *error_ = message;
                    }
                    catch (const std::bad_alloc&)
                    {
                    }
                }
            }
        }
        bool bytes(void* data, std::size_t size) noexcept
        {
            if (failed_) return false;
            auto result = reader_.readBytes({static_cast<std::byte*>(data), size});
            if (!result) fail("unexpected EOF");
            return static_cast<bool>(result);
        }
        bool u8(std::uint8_t& value) noexcept { return readUnsigned(value); }
        bool u16(std::uint16_t& value) noexcept { return readUnsigned(value); }
        bool u32(std::uint32_t& value) noexcept { return readUnsigned(value); }
        bool u64(std::uint64_t& value) noexcept { return readUnsigned(value); }
        bool i32(std::int32_t& value) noexcept
        {
            if (failed_) return false;
            auto result = reader_.readSigned<std::int32_t>();
            if (!result) { fail("unexpected EOF"); return false; }
            value = *result;
            return true;
        }
        bool f32(float& value) noexcept
        {
            if (failed_) return false;
            auto result = reader_.readFloat<float>();
            if (!result) { fail("unexpected EOF"); return false; }
            value = *result;
            return true;
        }
        bool str(std::string& value, std::size_t max_size) noexcept
        {
            std::uint32_t size{};
            if (!u32(size)) return false;
            if (size > max_size) { fail("string length exceeds limit"); return false; }
            try { value.resize(size); }
            catch (const std::bad_alloc&) { fail("allocation failure"); return false; }
            return size == 0U || bytes(value.data(), size);
        }

    private:
        template <std::unsigned_integral T>
        bool readUnsigned(T& value) noexcept
        {
            if (failed_) return false;
            auto result = reader_.readUnsigned<T>();
            if (!result) { fail("unexpected EOF"); return false; }
            value = *result;
            return true;
        }

        BinaryReader reader_;
        std::string* error_{};
        bool failed_{};
    };
} // namespace lux::serialization
