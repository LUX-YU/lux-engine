#pragma once

#include <lux/engine/ecs/scene_format/EntitySectionCodec.hpp>

#include <lux/engine/core/serialization/ByteIO.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lux::ecs::scene_format::detail
{
    using lux::core::serialization::ByteReader;
    using lux::core::serialization::ByteWriter;

    [[nodiscard]] inline EntitySectionCodecFailure failure(
        EEntitySectionCodecError error,
        std::string detail)
    {
        return {error, std::move(detail)};
    }

    class DecodeAllocationBudget final
    {
    public:
        DecodeAllocationBudget(
            std::size_t input_bytes,
            std::uint64_t configured_maximum) noexcept
        {
            constexpr std::uint64_t kFixedAllowance = 64u * 1024u;
            const auto input = static_cast<std::uint64_t>(input_bytes);
            const auto amplified = input >
                    (std::numeric_limits<std::uint64_t>::max() -
                     kFixedAllowance) / 4u
                ? std::numeric_limits<std::uint64_t>::max()
                : input * 4u + kFixedAllowance;
            remaining_ = static_cast<std::size_t>(std::min<std::uint64_t>(
                std::min(configured_maximum, amplified),
                std::numeric_limits<std::size_t>::max()));
        }

        [[nodiscard]] bool consume(
            ByteReader& reader,
            std::uint64_t count,
            std::uint64_t bytes_per_item,
            const char* message) noexcept
        {
            if (bytes_per_item != 0u &&
                count > remaining_ / bytes_per_item)
            {
                reader.fail(message);
                return false;
            }
            remaining_ -= static_cast<std::size_t>(count * bytes_per_item);
            return true;
        }

    private:
        std::size_t remaining_{0u};
    };

    [[nodiscard]] inline bool countFitsRemaining(
        ByteReader& reader,
        std::uint64_t count,
        std::uint64_t minimum_wire_bytes,
        const char* message) noexcept
    {
        if (minimum_wire_bytes != 0u &&
            count > reader.remaining() / minimum_wire_bytes)
        {
            reader.fail(message);
            return false;
        }
        return true;
    }

    template <class Value>
    [[nodiscard]] bool prepareVector(
        ByteReader& reader,
        DecodeAllocationBudget& budget,
        std::vector<Value>& values,
        std::uint32_t count,
        std::uint32_t minimum_wire_bytes,
        const char* truncated_message,
        const char* budget_message) noexcept
    {
        if (!countFitsRemaining(
                reader, count, minimum_wire_bytes, truncated_message) ||
            !budget.consume(reader, count, sizeof(Value), budget_message))
        {
            return false;
        }
        values.resize(count);
        return true;
    }

    template <class Value>
    [[nodiscard]] bool reserveVector(
        ByteReader& reader,
        DecodeAllocationBudget& budget,
        std::vector<Value>& values,
        std::uint32_t count,
        std::uint32_t minimum_wire_bytes,
        const char* truncated_message,
        const char* budget_message) noexcept
    {
        if (!countFitsRemaining(
                reader, count, minimum_wire_bytes, truncated_message) ||
            !budget.consume(reader, count, sizeof(Value), budget_message))
        {
            return false;
        }
        values.reserve(count);
        return true;
    }

    template <class Id>
    [[nodiscard]] bool uuidLess(const Id& lhs, const Id& rhs) noexcept
    {
        const auto left = lhs.value().as_bytes();
        const auto right = rhs.value().as_bytes();
        return std::lexicographical_compare(
            left.begin(), left.end(), right.begin(), right.end());
    }

    template <class Id>
    void writeUuid(ByteWriter& writer, const Id& id)
    {
        const auto bytes = id.value().as_bytes();
        writer.bytes(bytes.data(), bytes.size());
    }

    template <class Id>
    bool readUuid(ByteReader& reader, Id& id) noexcept
    {
        std::array<std::uint8_t, 16u> bytes{};
        if (!reader.bytes(bytes.data(), bytes.size()))
            return false;
        id = Id{uuids::uuid{bytes}};
        return true;
    }

    inline void writeDigest(
        ByteWriter& writer,
        const lux::cxx::algorithm::Sha256Digest& digest)
    {
        writer.bytes(digest.data(), digest.size());
    }

    inline bool readDigest(
        ByteReader& reader,
        lux::cxx::algorithm::Sha256Digest& digest) noexcept
    {
        return reader.bytes(digest.data(), digest.size());
    }

    inline void writeBlob(
        ByteWriter& writer,
        std::span<const std::byte> bytes)
    {
        writer.u32(static_cast<std::uint32_t>(bytes.size()));
        if (!bytes.empty())
            writer.bytes(bytes.data(), bytes.size());
    }

    inline bool readBlob(
        ByteReader& reader,
        std::vector<std::byte>& bytes,
        std::uint64_t maximum_bytes,
        DecodeAllocationBudget& budget) noexcept
    {
        std::uint32_t count = 0u;
        if (!reader.u32(count))
            return false;
        if (count > maximum_bytes)
        {
            reader.fail("blob exceeds codec limit");
            return false;
        }
        if (count > reader.remaining())
        {
            reader.fail("blob bytes do not fit remaining input");
            return false;
        }
        if (!budget.consume(
                reader,
                count,
                sizeof(std::byte),
                "blob exceeds decode allocation budget"))
        {
            return false;
        }
        bytes.resize(count);
        return count == 0u || reader.bytes(bytes.data(), count);
    }

    inline bool readString(
        ByteReader& reader,
        std::string& value,
        std::uint32_t maximum_bytes,
        DecodeAllocationBudget& budget) noexcept
    {
        std::uint32_t count = 0u;
        if (!reader.u32(count))
            return false;
        if (count > maximum_bytes)
        {
            reader.fail("string length exceeds codec limit");
            return false;
        }
        if (count > reader.remaining())
        {
            reader.fail("string bytes do not fit remaining input");
            return false;
        }
        if (!budget.consume(
                reader,
                count,
                sizeof(char),
                "string exceeds decode allocation budget"))
        {
            return false;
        }
        value.resize(count);
        return count == 0u || reader.bytes(value.data(), count);
    }

    class WireNameTable final
    {
    public:
        WireNameTable()
        {
            names_.emplace_back();
        }

        void add(std::string_view name)
        {
            if (!name.empty())
                names_.emplace_back(name);
        }

        void canonicalize()
        {
            std::sort(names_.begin() + 1, names_.end());
            names_.erase(
                std::unique(names_.begin() + 1, names_.end()),
                names_.end());
        }

        [[nodiscard]] std::uint32_t index(std::string_view name) const
            noexcept
        {
            if (name.empty())
                return 0u;
            const auto begin = names_.begin() + 1;
            const auto it = std::lower_bound(begin, names_.end(), name);
            if (it == names_.end() || *it != name)
                return std::numeric_limits<std::uint32_t>::max();
            return static_cast<std::uint32_t>(it - names_.begin());
        }

        [[nodiscard]] std::string_view at(std::uint32_t index) const noexcept
        {
            if (index >= names_.size())
                return {};
            return names_[index];
        }

        [[nodiscard]] std::size_t size() const noexcept
        {
            return names_.size();
        }

        void write(ByteWriter& writer) const
        {
            writer.u32(static_cast<std::uint32_t>(names_.size()));
            for (std::size_t index = 1u; index < names_.size(); ++index)
                writer.str(names_[index]);
        }

        [[nodiscard]] bool read(
            ByteReader& reader,
            const EntitySectionCodecLimits& limits,
            DecodeAllocationBudget& budget) noexcept
        {
            std::uint32_t count = 0u;
            if (!reader.u32(count))
                return false;
            if (count == 0u || count > limits.maximum_names)
            {
                reader.fail("name table count exceeds codec limit");
                return false;
            }
            if (!countFitsRemaining(
                    reader,
                    count - 1u,
                    sizeof(std::uint32_t),
                    "name table cannot fit remaining input") ||
                !budget.consume(
                    reader,
                    count,
                    sizeof(std::string),
                    "name table exceeds decode allocation budget"))
            {
                return false;
            }
            names_.clear();
            names_.reserve(count);
            names_.emplace_back();
            for (std::uint32_t index = 1u; index < count; ++index)
            {
                std::string value;
                if (!readString(
                        reader,
                        value,
                        limits.maximum_string_bytes,
                        budget))
                    return false;
                if (value.empty() ||
                    (index > 1u && names_.back() >= value))
                {
                    reader.fail("name table is not strictly canonical");
                    return false;
                }
                names_.push_back(std::move(value));
            }
            return true;
        }

    private:
        std::vector<std::string> names_;
    };

    inline void writeStableId(
        ByteWriter& writer,
        const WireNameTable& names,
        const lux::ecs::ComponentSchemaId& id)
    {
        writer.u64(id.hash);
        writer.u32(names.index(id.name));
    }

    inline bool readStableId(
        ByteReader& reader,
        const WireNameTable& names,
        lux::ecs::ComponentSchemaId& id,
        DecodeAllocationBudget& budget) noexcept
    {
        std::uint64_t hash = 0u;
        std::uint32_t name_index = 0u;
        if (!reader.u64(hash) || !reader.u32(name_index))
            return false;
        const auto name = names.at(name_index);
        if (name.empty())
        {
            reader.fail("component schema id references an invalid name");
            return false;
        }
        if (!budget.consume(
                reader,
                name.size(),
                sizeof(char),
                "component schema names exceed decode allocation budget"))
        {
            return false;
        }
        id = lux::ecs::ComponentSchemaId{hash, std::string{name}};
        if (!lux::ecs::isValidComponentSchemaId(id))
        {
            reader.fail(
                "component schema id hash or canonical name is invalid");
            return false;
        }
        return true;
    }

    template <class StableId>
    void writeStableId(
        ByteWriter& writer,
        const WireNameTable& names,
        const StableId& id)
    {
        writer.u64(id.hash());
        writer.u32(names.index(id.name()));
    }

    template <class StableId>
    bool readStableId(
        ByteReader& reader,
        const WireNameTable& names,
        StableId& id,
        DecodeAllocationBudget& budget) noexcept
    {
        std::uint64_t hash = 0u;
        std::uint32_t name_index = 0u;
        if (!reader.u64(hash) || !reader.u32(name_index))
            return false;
        const auto name = names.at(name_index);
        if (name.empty())
        {
            reader.fail("stable id references an invalid name");
            return false;
        }
        if (!budget.consume(
                reader,
                name.size(),
                sizeof(char),
                "stable id names exceed decode allocation budget"))
        {
            return false;
        }
        id = StableId{name};
        if (id.hash() != hash || !isValidStableId(id))
        {
            reader.fail("stable id hash or canonical name is invalid");
            return false;
        }
        return true;
    }
    [[nodiscard]] inline bool readCount(
        ByteReader& reader,
        std::uint32_t maximum,
        std::uint32_t& count,
        const char* message) noexcept
    {
        if (!reader.u32(count))
            return false;
        if (count > maximum)
        {
            reader.fail(message);
            return false;
        }
        return true;
    }

    [[nodiscard]] inline EEntitySectionCodecError readerError(
        std::string_view detail) noexcept
    {
        if (detail.find("limit") != std::string_view::npos ||
            detail.find("exceeds") != std::string_view::npos)
        {
            return EEntitySectionCodecError::LIMIT_EXCEEDED;
        }
        if (detail.find("hash") != std::string_view::npos ||
            detail.find("canonical name") != std::string_view::npos)
        {
            return EEntitySectionCodecError::HASH_MISMATCH;
        }
        return EEntitySectionCodecError::TRUNCATED;
    }
}
