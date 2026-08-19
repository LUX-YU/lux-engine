#include <lux/engine/ecs/pixel/systems/PixelChunkPersistence.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>

namespace lux::ecs
{
    namespace
    {
        constexpr std::uint32_t kPixelDeltaMagic = 0x4450584cu; // LXPD

        template <class T>
        void appendPod(std::vector<std::byte>& output, const T& value)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            const auto* first = reinterpret_cast<const std::byte*>(&value);
            output.insert(output.end(), first, first + sizeof(T));
        }

        template <class T>
        [[nodiscard]] bool readPod(
            std::span<const std::byte> input,
            std::size_t& offset,
            T& value) noexcept
        {
            static_assert(std::is_trivially_copyable_v<T>);
            if (offset > input.size() || sizeof(T) > input.size() - offset)
                return false;
            std::memcpy(&value, input.data() + offset, sizeof(T));
            offset += sizeof(T);
            return true;
        }

        [[nodiscard]] bool validDelta(
            std::span<const PixelChunkDeltaCell> delta) noexcept
        {
            std::uint32_t previous = 0u;
            bool first = true;
            for (const auto& edit : delta)
            {
                if (edit.x >= PixelFieldRuntime::kChunkSizeCells ||
                    edit.y >= PixelFieldRuntime::kChunkSizeCells)
                {
                    return false;
                }
                const auto ordinal = static_cast<std::uint32_t>(edit.y) *
                    PixelFieldRuntime::kChunkSizeCells + edit.x;
                if (!first && ordinal <= previous)
                    return false;
                first = false;
                previous = ordinal;
            }
            return true;
        }

        [[nodiscard]] bool sameSchema(
            const lux::ecs::scene_format::PersistenceSchemaId& schema) noexcept
        {
            const auto expected = pixelChunkPersistenceSchema();
            return schema.view() == expected.view();
        }
    }

    lux::ecs::scene_format::PersistenceSchemaId
    pixelChunkPersistenceSchema()
    {
        return lux::ecs::scene_format::PersistenceSchemaId{
            "lux.pixel.chunk_delta"};
    }

    lux::cxx::expected<
        lux::ecs::scene_format::PersistenceJournalRecord,
        EPixelChunkPersistenceError>
    encodePixelChunkPersistence(
        const PixelChunkDeltaSnapshot& snapshot) noexcept
    {
        if (snapshot.base_digest ==
                lux::cxx::algorithm::Sha256Digest{} ||
            snapshot.sequence == 0u ||
            snapshot.delta.empty() || !validDelta(snapshot.delta) ||
            snapshot.delta.size() > std::numeric_limits<std::uint32_t>::max())
        {
            return lux::cxx::unexpected(
                snapshot.delta.empty()
                    ? EPixelChunkPersistenceError::EMPTY_DELTA
                    : EPixelChunkPersistenceError::INVALID_PAYLOAD);
        }

        std::vector<std::byte> payload;
        appendPod(payload, kPixelDeltaMagic);
        appendPod(payload, kPixelChunkPersistenceSchemaVersion);
        appendPod(payload, snapshot.coordinate.x);
        appendPod(payload, snapshot.coordinate.y);
        appendPod(
            payload,
            static_cast<std::uint32_t>(snapshot.delta.size()));
        for (const auto& edit : snapshot.delta)
        {
            appendPod(payload, edit.x);
            appendPod(payload, edit.y);
            appendPod(payload, edit.material);
        }
        auto record = lux::ecs::scene_format::makePersistenceJournalRecord(
            pixelChunkPersistenceSchema(),
            kPixelChunkPersistenceSchemaVersion,
            snapshot.base_digest,
            snapshot.sequence,
            std::move(payload));
        if (!record)
        {
            return lux::cxx::unexpected(
                EPixelChunkPersistenceError::JOURNAL_REJECTED);
        }
        return std::move(*record);
    }

    lux::cxx::expected<
        PixelChunkDeltaSnapshot,
        EPixelChunkPersistenceError>
    decodePixelChunkPersistence(
        const lux::ecs::scene_format::PersistenceJournalRecord& record) noexcept
    {
        if (!record.valid() || !sameSchema(record.schema) ||
            record.schema_version != kPixelChunkPersistenceSchemaVersion)
        {
            return lux::cxx::unexpected(
                EPixelChunkPersistenceError::INVALID_SCHEMA);
        }
        std::size_t offset = 0u;
        std::uint32_t magic = 0u;
        std::uint32_t version = 0u;
        std::uint32_t count = 0u;
        PixelChunkDeltaSnapshot result;
        result.base_digest = record.base_digest;
        result.sequence = record.sequence;
        if (!readPod(record.record_bytes, offset, magic) ||
            !readPod(record.record_bytes, offset, version) ||
            !readPod(record.record_bytes, offset, result.coordinate.x) ||
            !readPod(record.record_bytes, offset, result.coordinate.y) ||
            !readPod(record.record_bytes, offset, count) ||
            magic != kPixelDeltaMagic ||
            version != kPixelChunkPersistenceSchemaVersion || count == 0u ||
            count > PixelFieldRuntime::kChunkCellCount)
        {
            return lux::cxx::unexpected(
                EPixelChunkPersistenceError::INVALID_PAYLOAD);
        }
        constexpr std::size_t kCellBytes =
            sizeof(std::uint16_t) * 2u + sizeof(MaterialId);
        if (offset > record.record_bytes.size() ||
            static_cast<std::size_t>(count) >
                (record.record_bytes.size() - offset) / kCellBytes ||
            static_cast<std::size_t>(count) * kCellBytes !=
                record.record_bytes.size() - offset)
        {
            return lux::cxx::unexpected(
                EPixelChunkPersistenceError::INVALID_PAYLOAD);
        }
        result.delta.resize(count);
        for (auto& edit : result.delta)
        {
            if (!readPod(record.record_bytes, offset, edit.x) ||
                !readPod(record.record_bytes, offset, edit.y) ||
                !readPod(record.record_bytes, offset, edit.material))
            {
                return lux::cxx::unexpected(
                    EPixelChunkPersistenceError::INVALID_PAYLOAD);
            }
        }
        if (offset != record.record_bytes.size() ||
            !validDelta(result.delta))
        {
            return lux::cxx::unexpected(
                EPixelChunkPersistenceError::INVALID_PAYLOAD);
        }
        return result;
    }

    lux::cxx::expected<void, EPixelChunkPersistenceError>
    mergePixelChunkPersistence(
        PixelChunkLoad& base,
        const lux::ecs::scene_format::PersistenceJournalRecord& record) noexcept
    {
        auto decoded = decodePixelChunkPersistence(record);
        if (!decoded)
            return lux::cxx::unexpected(decoded.error());
        if (decoded->coordinate != base.coordinate)
        {
            return lux::cxx::unexpected(
                EPixelChunkPersistenceError::INVALID_PAYLOAD);
        }
        if (decoded->base_digest != base.base_digest)
        {
            return lux::cxx::unexpected(
                EPixelChunkPersistenceError::BASE_MISMATCH);
        }
        if (decoded->sequence < base.sequence)
        {
            return lux::cxx::unexpected(
                EPixelChunkPersistenceError::SEQUENCE_MISMATCH);
        }
        base.sequence = decoded->sequence;
        base.delta = std::move(decoded->delta);
        return {};
    }

    lux::cxx::expected<bool, EPixelChunkPersistenceError>
    PixelChunkPersistenceStore::capture(
        const PixelFieldRuntime& runtime,
        PixelFieldHandle field,
        PixelChunkCoord coordinate) noexcept
    {
        PixelChunkDeltaSnapshot snapshot;
        if (!runtime.captureChunkDelta(field, coordinate, snapshot))
        {
            ++rejected_records_;
            return lux::cxx::unexpected(
                EPixelChunkPersistenceError::CHUNK_UNAVAILABLE);
        }
        if (snapshot.delta.empty())
            return false;
        auto record = encodePixelChunkPersistence(snapshot);
        if (!record)
        {
            ++rejected_records_;
            return lux::cxx::unexpected(record.error());
        }
        auto found = journals_.find(coordinate);
        if (found == journals_.end())
        {
            found = journals_.emplace(
                coordinate,
                lux::ecs::scene_format::PersistenceJournal{}).first;
        }
        auto compacted = found->second.compact(std::move(*record));
        if (!compacted)
        {
            if (compacted.error() ==
                lux::ecs::scene_format::EEntityPersistenceJournalError::
                    BASE_MISMATCH)
            {
                ++base_mismatches_;
                return lux::cxx::unexpected(
                    EPixelChunkPersistenceError::BASE_MISMATCH);
            }
            ++rejected_records_;
            return lux::cxx::unexpected(
                EPixelChunkPersistenceError::JOURNAL_REJECTED);
        }
        if (*compacted)
            ++captures_;
        return *compacted;
    }

    lux::cxx::expected<bool, EPixelChunkPersistenceError>
    PixelChunkPersistenceStore::restore(PixelChunkLoad& base) noexcept
    {
        const auto* record = latest(base.coordinate);
        if (!record)
            return false;
        auto merged = mergePixelChunkPersistence(base, *record);
        if (!merged)
        {
            if (merged.error() == EPixelChunkPersistenceError::BASE_MISMATCH)
                ++base_mismatches_;
            else
                ++rejected_records_;
            return lux::cxx::unexpected(merged.error());
        }
        ++recoveries_;
        return true;
    }

    const lux::ecs::scene_format::PersistenceJournalRecord*
    PixelChunkPersistenceStore::latest(
        PixelChunkCoord coordinate) const noexcept
    {
        const auto found = journals_.find(coordinate);
        return found == journals_.end() ? nullptr : found->second.latest();
    }

    PixelChunkPersistenceSnapshot
    PixelChunkPersistenceStore::snapshot() const noexcept
    {
        PixelChunkPersistenceSnapshot result;
        result.chunks = journals_.size();
        result.captures = captures_;
        result.recoveries = recoveries_;
        result.base_mismatches = base_mismatches_;
        result.rejected_records = rejected_records_;
        for (const auto& [_, journal] : journals_)
        {
            const auto remaining =
                std::numeric_limits<std::uint64_t>::max() -
                result.record_bytes;
            result.record_bytes = journal.byteSize() > remaining
                ? std::numeric_limits<std::uint64_t>::max()
                : result.record_bytes + journal.byteSize();
        }
        return result;
    }

    void PixelChunkPersistenceStore::clear() noexcept
    {
        journals_.clear();
        captures_ = 0u;
        recoveries_ = 0u;
        base_mismatches_ = 0u;
        rejected_records_ = 0u;
    }
}
