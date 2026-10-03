#pragma once

#include <lux/engine/editor/persistence/ArtifactStore.hpp>
#include <lux/engine/editor/sessions/PersistenceCheckpoint.hpp>

namespace lux::editor::persistence
{
    struct SaveId final
    {
        std::uint64_t value{};
        friend bool operator==(SaveId, SaveId) noexcept = default;
    };
    enum class ESaveMode : std::uint8_t
    {
        SAVE,
        SAVE_AS,
        EXPORT_COPY
    };
    struct SaveRequest final
    {
        sessions::SessionId session;
        ESaveMode mode{ESaveMode::SAVE};
        std::optional<WriteTarget> destination;
        asset::AssetId asset;
        // Absent captures the current source at admission. Explicit review/draft requests retain
        // their source through extensible target resolution and the final source describe boundary.
        std::optional<sessions::ContentStamp> based_on;
    };
    enum class EAdoption : std::uint8_t
    {
        NONE,
        APPLIED,
        CLOSED,
        STALE_HISTORY,
        STALE_BINDING,
        OLDER_RECEIPT,
        BUSY
    };
    enum class ESaveStage : std::uint8_t
    {
        CAPTURING,
        CAPTURED,
        ENCODING,
        READY,
        PUBLISHING,
        AWAITING_ADOPTION,
        TERMINAL
    };
    enum class ECancelResult : std::uint8_t
    {
        REQUESTED,
        TOO_LATE,
        ALREADY_TERMINAL
    };
    struct SaveOutcome final
    {
        VPublicationOutcome publication;
        EAdoption adoption{EAdoption::NONE};
    };
    struct SaveStatus final
    {
        ESaveStage stage;
        sessions::ContentStamp content;
        WriteTicket ticket;
        std::optional<SaveOutcome> outcome;
        // Original admitted intent and physical destination, independent of later binding/closure.
        SaveRequest request;
        WriteTarget target;
    };
    struct SaveLimits final
    {
        std::size_t max_active_saves{32};
        std::size_t snapshot_bytes{128 * 1024 * 1024};
        std::size_t terminal_records{128};
    };
    struct SaveSourceInfo final
    {
        sessions::ContentStamp content;
        sessions::BindingRevision binding;
        std::optional<WriteTarget> target;
    };
    struct SaveReceipt final
    {
        sessions::ContentStamp content;
        sessions::BindingRevision binding;
        sessions::PublicationOrder order;
        WriteTarget target;
        CommitReceipt publication;
    };
} // namespace lux::editor::persistence
