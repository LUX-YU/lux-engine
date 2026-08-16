#pragma once
/**
 * @file PixelChunkPersistence.hpp
 * @brief Pixel-owned sparse chunk delta journal codec and scene-local store.
 */

#include <lux/engine/ecs/pixel/systems/PixelFieldRuntime.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/resource/entity_scene/EntityPersistenceJournal.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>

namespace lux::ecs
{
    inline constexpr std::uint32_t kPixelChunkPersistenceSchemaVersion = 1u;

    enum class EPixelChunkPersistenceError : std::uint8_t
    {
        CHUNK_UNAVAILABLE,
        EMPTY_DELTA,
        INVALID_SCHEMA,
        INVALID_PAYLOAD,
        BASE_MISMATCH,
        SEQUENCE_MISMATCH,
        JOURNAL_REJECTED
    };

    [[nodiscard]] LUX_FUNCTION_PUBLIC
    lux::entity_scene::PersistenceSchemaId
    pixelChunkPersistenceSchema();

    [[nodiscard]] LUX_FUNCTION_PUBLIC lux::cxx::expected<
        lux::entity_scene::PersistenceJournalRecord,
        EPixelChunkPersistenceError>
    encodePixelChunkPersistence(
        const PixelChunkDeltaSnapshot& snapshot) noexcept;

    [[nodiscard]] LUX_FUNCTION_PUBLIC lux::cxx::expected<
        PixelChunkDeltaSnapshot,
        EPixelChunkPersistenceError>
    decodePixelChunkPersistence(
        const lux::entity_scene::PersistenceJournalRecord& record) noexcept;

    /// Applies a journal delta to an owning decoded base before the expensive
    /// PreparedPixelChunk expansion. This is pure worker-safe CPU work.
    [[nodiscard]] LUX_FUNCTION_PUBLIC lux::cxx::expected<
        void,
        EPixelChunkPersistenceError>
    mergePixelChunkPersistence(
        PixelChunkLoad& base,
        const lux::entity_scene::PersistenceJournalRecord& record) noexcept;

    struct PixelChunkPersistenceSnapshot final
    {
        std::size_t chunks{0u};
        std::uint64_t record_bytes{0u};
        std::uint64_t captures{0u};
        std::uint64_t recoveries{0u};
        std::uint64_t base_mismatches{0u};
        std::uint64_t rejected_records{0u};
    };

    /// Pixel owns subject identity (chunk coordinate) and compaction policy.
    /// The stored record remains the domain-neutral journal envelope.
    class LUX_FUNCTION_PUBLIC PixelChunkPersistenceStore final
    {
    public:
        [[nodiscard]] lux::cxx::expected<
            bool,
            EPixelChunkPersistenceError>
        capture(
            const PixelFieldRuntime& runtime,
            PixelFieldHandle field,
            PixelChunkCoord coordinate) noexcept;

        [[nodiscard]] lux::cxx::expected<
            bool,
            EPixelChunkPersistenceError>
        restore(PixelChunkLoad& base) noexcept;

        [[nodiscard]] const lux::entity_scene::PersistenceJournalRecord*
        latest(PixelChunkCoord coordinate) const noexcept;

        [[nodiscard]] PixelChunkPersistenceSnapshot snapshot() const noexcept;
        void clear() noexcept;

    private:
        std::unordered_map<
            PixelChunkCoord,
            lux::entity_scene::PersistenceJournal,
            PixelChunkCoordHash> journals_;
        std::uint64_t captures_{0u};
        std::uint64_t recoveries_{0u};
        std::uint64_t base_mismatches_{0u};
        std::uint64_t rejected_records_{0u};
    };
}
