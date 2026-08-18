#pragma once
/**
 * @file EntityPersistenceJournal.hpp
 * @brief Domain-neutral persistence journal record and bounded stream codec.
 */

#include <lux/cxx/algorithm/Sha256.hpp>
#include <lux/cxx/core/StableNameId.hpp>
#include <lux/engine/ecs/scene_format/Identifiers.hpp>
#include <lux/engine/ecs/scene_format/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace lux::ecs::scene_format
{
    struct PersistenceSchemaIdTag final {};
    using PersistenceSchemaId = lux::cxx::StableNameId<PersistenceSchemaIdTag>;

    inline constexpr std::uint32_t kEntityPersistenceJournalVersion = 1u;
    inline constexpr std::uint64_t kMaximumPersistenceRecordBytes =
        64ull * 1024ull * 1024ull;

    struct LUX_ECS_SCENE_FORMAT_PUBLIC
        PersistenceJournalRecord final
    {
        PersistenceSchemaId schema;
        std::uint32_t schema_version{0u};
        lux::cxx::algorithm::Sha256Digest base_digest;
        std::uint64_t sequence{0u};
        std::vector<std::byte> record_bytes;
        lux::cxx::algorithm::Sha256Digest record_digest;

        [[nodiscard]] bool valid() const noexcept;

        friend bool operator==(
            const PersistenceJournalRecord&,
            const PersistenceJournalRecord&) = default;
    };

    enum class EEntityPersistenceJournalError : std::uint8_t
    {
        INVALID_SCHEMA,
        INVALID_BASE_DIGEST,
        INVALID_SEQUENCE,
        RECORD_TOO_LARGE,
        RECORD_DIGEST_MISMATCH,
        CORRUPT_WIRE,
        UNSUPPORTED_VERSION,
        SCHEMA_MISMATCH,
        BASE_MISMATCH,
        NON_MONOTONIC_SEQUENCE
    };

    [[nodiscard]] LUX_ECS_SCENE_FORMAT_PUBLIC
    lux::cxx::expected<
        PersistenceJournalRecord,
        EEntityPersistenceJournalError>
    makePersistenceJournalRecord(
        PersistenceSchemaId schema,
        std::uint32_t schema_version,
        const lux::cxx::algorithm::Sha256Digest& base_digest,
        std::uint64_t sequence,
        std::vector<std::byte> record_bytes) noexcept;

    [[nodiscard]] LUX_ECS_SCENE_FORMAT_PUBLIC
    lux::cxx::expected<
        std::vector<std::byte>,
        EEntityPersistenceJournalError>
    encodePersistenceJournalRecord(
        const PersistenceJournalRecord& record) noexcept;

    [[nodiscard]] LUX_ECS_SCENE_FORMAT_PUBLIC
    lux::cxx::expected<
        PersistenceJournalRecord,
        EEntityPersistenceJournalError>
    decodePersistenceJournalRecord(
        std::span<const std::byte> encoded) noexcept;

    /// One logical subject's journal. Subject identity and storage location
    /// belong to the domain owner; this class deliberately knows neither.
    class LUX_ECS_SCENE_FORMAT_PUBLIC PersistenceJournal final
    {
    public:
        /// Returns true when a new record was appended and false for an exact
        /// idempotent replay of the latest record.
        [[nodiscard]] lux::cxx::expected<
            bool,
            EEntityPersistenceJournalError>
        append(PersistenceJournalRecord record) noexcept;

        /// Replaces older records after the caller has materialized their
        /// meaning. Schema/base identity and sequence checks are unchanged.
        [[nodiscard]] lux::cxx::expected<
            bool,
            EEntityPersistenceJournalError>
        compact(PersistenceJournalRecord record) noexcept;

        [[nodiscard]] const PersistenceJournalRecord* latest() const noexcept;
        [[nodiscard]] std::span<const PersistenceJournalRecord> records()
            const noexcept
        {
            return records_;
        }
        [[nodiscard]] std::uint64_t byteSize() const noexcept;
        [[nodiscard]] bool empty() const noexcept { return records_.empty(); }
        void clear() noexcept { records_.clear(); }

    private:
        [[nodiscard]] lux::cxx::expected<
            bool,
            EEntityPersistenceJournalError>
        validateAppend(const PersistenceJournalRecord& record) const noexcept;

        std::vector<PersistenceJournalRecord> records_;
    };
}
