#include <lux/engine/ecs/scene_format/PersistenceJournal.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

namespace lux::ecs::scene_format
{
    namespace
    {
        constexpr std::uint32_t kJournalRecordMagic = 0x4a45584cu; // LXEJ
        constexpr std::uint32_t kMaximumSchemaNameBytes = 1024u;

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

        void appendDigest(
            std::vector<std::byte>& output,
            const lux::cxx::algorithm::Sha256Digest& digest)
        {
            output.insert(
                output.end(),
                reinterpret_cast<const std::byte*>(digest.data()),
                reinterpret_cast<const std::byte*>(
                    digest.data() + digest.size()));
        }

        [[nodiscard]] bool readDigest(
            std::span<const std::byte> input,
            std::size_t& offset,
            lux::cxx::algorithm::Sha256Digest& digest) noexcept
        {
            if (offset > input.size() ||
                digest.size() > input.size() - offset)
            {
                return false;
            }
            std::memcpy(
                digest.data(),
                input.data() + offset,
                digest.size());
            offset += digest.size();
            return true;
        }

        [[nodiscard]] EEntityPersistenceJournalError validationError(
            const PersistenceJournalRecord& record) noexcept
        {
            if (!record.schema.isValid() ||
                !isCanonicalStableName(
                    record.schema.name()) ||
                record.schema_version == 0u)
            {
                return EEntityPersistenceJournalError::INVALID_SCHEMA;
            }
            if (record.base_digest ==
                lux::cxx::algorithm::Sha256Digest{})
            {
                return EEntityPersistenceJournalError::INVALID_BASE_DIGEST;
            }
            if (record.sequence == 0u)
            {
                return EEntityPersistenceJournalError::INVALID_SEQUENCE;
            }
            if (record.record_bytes.size() >
                kMaximumPersistenceRecordBytes)
            {
                return EEntityPersistenceJournalError::RECORD_TOO_LARGE;
            }
            if (lux::cxx::algorithm::Sha256::hash(record.record_bytes) !=
                record.record_digest)
            {
                return EEntityPersistenceJournalError::
                    RECORD_DIGEST_MISMATCH;
            }
            return EEntityPersistenceJournalError::CORRUPT_WIRE;
        }
    }

    bool PersistenceJournalRecord::valid() const noexcept
    {
        return schema.isValid() &&
            isCanonicalStableName(schema.name()) &&
            schema_version != 0u && base_digest !=
                lux::cxx::algorithm::Sha256Digest{} && sequence != 0u &&
            record_bytes.size() <= kMaximumPersistenceRecordBytes &&
            lux::cxx::algorithm::Sha256::hash(record_bytes) == record_digest;
    }

    lux::cxx::expected<
        PersistenceJournalRecord,
        EEntityPersistenceJournalError>
    makePersistenceJournalRecord(
        PersistenceSchemaId schema,
        std::uint32_t schema_version,
        const lux::cxx::algorithm::Sha256Digest& base_digest,
        std::uint64_t sequence,
        std::vector<std::byte> record_bytes) noexcept
    {
        PersistenceJournalRecord result{
            std::move(schema),
            schema_version,
            base_digest,
            sequence,
            std::move(record_bytes),
            {}};
        result.record_digest = lux::cxx::algorithm::Sha256::hash(result.record_bytes);
        if (!result.valid())
            return lux::cxx::unexpected(validationError(result));
        return result;
    }

    lux::cxx::expected<
        std::vector<std::byte>,
        EEntityPersistenceJournalError>
    encodePersistenceJournalRecord(
        const PersistenceJournalRecord& record) noexcept
    {
        if (!record.valid())
            return lux::cxx::unexpected(validationError(record));
        if (record.schema.name().size() > kMaximumSchemaNameBytes ||
            record.schema.name().size() >
                std::numeric_limits<std::uint32_t>::max())
        {
            return lux::cxx::unexpected(
                EEntityPersistenceJournalError::INVALID_SCHEMA);
        }

        std::vector<std::byte> result;
        const auto name_size = static_cast<std::uint32_t>(
            record.schema.name().size());
        appendPod(result, kJournalRecordMagic);
        appendPod(result, kEntityPersistenceJournalVersion);
        appendPod(result, record.schema.hash());
        appendPod(result, name_size);
        result.insert(
            result.end(),
            reinterpret_cast<const std::byte*>(record.schema.name().data()),
            reinterpret_cast<const std::byte*>(
                record.schema.name().data() + name_size));
        appendPod(result, record.schema_version);
        appendDigest(result, record.base_digest);
        appendPod(result, record.sequence);
        appendPod(
            result,
            static_cast<std::uint64_t>(record.record_bytes.size()));
        appendDigest(result, record.record_digest);
        result.insert(
            result.end(),
            record.record_bytes.begin(),
            record.record_bytes.end());
        appendDigest(result, lux::cxx::algorithm::Sha256::hash(result));
        return result;
    }

    lux::cxx::expected<
        PersistenceJournalRecord,
        EEntityPersistenceJournalError>
    decodePersistenceJournalRecord(
        std::span<const std::byte> encoded) noexcept
    {
        constexpr std::size_t kEnvelopeDigestBytes = 32u;
        if (encoded.size() < kEnvelopeDigestBytes)
        {
            return lux::cxx::unexpected(
                EEntityPersistenceJournalError::CORRUPT_WIRE);
        }
        const auto envelope = encoded.first(
            encoded.size() - kEnvelopeDigestBytes);
        lux::cxx::algorithm::Sha256Digest envelope_digest;
        std::memcpy(
            envelope_digest.data(),
            encoded.data() + envelope.size(),
            envelope_digest.size());
        if (lux::cxx::algorithm::Sha256::hash(envelope) != envelope_digest)
        {
            return lux::cxx::unexpected(
                EEntityPersistenceJournalError::CORRUPT_WIRE);
        }

        std::size_t offset = 0u;
        std::uint32_t magic = 0u;
        std::uint32_t version = 0u;
        std::uint64_t schema_hash = 0u;
        std::uint32_t schema_name_size = 0u;
        if (!readPod(envelope, offset, magic) ||
            !readPod(envelope, offset, version) ||
            !readPod(envelope, offset, schema_hash) ||
            !readPod(envelope, offset, schema_name_size) ||
            magic != kJournalRecordMagic)
        {
            return lux::cxx::unexpected(
                EEntityPersistenceJournalError::CORRUPT_WIRE);
        }
        if (version != kEntityPersistenceJournalVersion)
        {
            return lux::cxx::unexpected(
                EEntityPersistenceJournalError::UNSUPPORTED_VERSION);
        }
        if (schema_name_size == 0u ||
            schema_name_size > kMaximumSchemaNameBytes ||
            offset > envelope.size() ||
            schema_name_size > envelope.size() - offset)
        {
            return lux::cxx::unexpected(
                EEntityPersistenceJournalError::INVALID_SCHEMA);
        }
        std::string schema_name{
            reinterpret_cast<const char*>(envelope.data() + offset),
            schema_name_size};
        offset += schema_name_size;

        PersistenceJournalRecord result;
        std::uint64_t record_size = 0u;
        if (!readPod(envelope, offset, result.schema_version) ||
            !readDigest(envelope, offset, result.base_digest) ||
            !readPod(envelope, offset, result.sequence) ||
            !readPod(envelope, offset, record_size) ||
            !readDigest(envelope, offset, result.record_digest) ||
            record_size > kMaximumPersistenceRecordBytes ||
            record_size > std::numeric_limits<std::size_t>::max() ||
            offset > envelope.size() ||
            record_size != envelope.size() - offset)
        {
            return lux::cxx::unexpected(
                EEntityPersistenceJournalError::CORRUPT_WIRE);
        }
        result.schema = PersistenceSchemaId{schema_name};
        if (result.schema.hash() != schema_hash)
        {
            return lux::cxx::unexpected(
                EEntityPersistenceJournalError::CORRUPT_WIRE);
        }
        result.record_bytes.assign(
            envelope.begin() + static_cast<std::ptrdiff_t>(offset),
            envelope.end());
        if (!result.valid())
            return lux::cxx::unexpected(validationError(result));
        return result;
    }

    lux::cxx::expected<bool, EEntityPersistenceJournalError>
    PersistenceJournal::validateAppend(
        const PersistenceJournalRecord& record) const noexcept
    {
        if (!record.valid())
            return lux::cxx::unexpected(validationError(record));
        const auto* current = latest();
        if (!current)
            return true;
        if (record.schema != current->schema ||
            record.schema_version != current->schema_version)
        {
            return lux::cxx::unexpected(
                EEntityPersistenceJournalError::SCHEMA_MISMATCH);
        }
        if (record.base_digest != current->base_digest)
        {
            return lux::cxx::unexpected(
                EEntityPersistenceJournalError::BASE_MISMATCH);
        }
        if (record.sequence < current->sequence)
        {
            return lux::cxx::unexpected(
                EEntityPersistenceJournalError::NON_MONOTONIC_SEQUENCE);
        }
        if (record.sequence == current->sequence)
        {
            if (record == *current)
                return false;
            return lux::cxx::unexpected(
                EEntityPersistenceJournalError::NON_MONOTONIC_SEQUENCE);
        }
        return true;
    }

    lux::cxx::expected<bool, EEntityPersistenceJournalError>
    PersistenceJournal::append(PersistenceJournalRecord record) noexcept
    {
        auto validation = validateAppend(record);
        if (!validation || !*validation)
            return validation;
        records_.push_back(std::move(record));
        return true;
    }

    lux::cxx::expected<bool, EEntityPersistenceJournalError>
    PersistenceJournal::compact(PersistenceJournalRecord record) noexcept
    {
        auto validation = validateAppend(record);
        if (!validation || !*validation)
            return validation;
        records_.clear();
        records_.push_back(std::move(record));
        return true;
    }

    const PersistenceJournalRecord* PersistenceJournal::latest() const noexcept
    {
        return records_.empty() ? nullptr : &records_.back();
    }

    std::uint64_t PersistenceJournal::byteSize() const noexcept
    {
        std::uint64_t result = 0u;
        for (const auto& record : records_)
        {
            const auto remaining =
                std::numeric_limits<std::uint64_t>::max() - result;
            if (record.record_bytes.size() > remaining)
                return std::numeric_limits<std::uint64_t>::max();
            result += record.record_bytes.size();
        }
        return result;
    }
}
