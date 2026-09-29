#pragma once

#include <lux/engine/editor/persistence/WriteLane.hpp>
#include <optional>
#include <memory>
#include <stop_token>
#include <variant>

namespace lux::editor::persistence
{
    enum class EDurability : std::uint8_t
    {
        FILE_FLUSHED,
        UNCONFIRMED
    };
    struct CommitReceipt final
    {
        std::string version;
        EDurability durability{EDurability::UNCONFIRMED};
        std::optional<PersistenceFailure> warning;
    };
    struct NotPublished final
    {
        PersistenceFailure failure;
    };
    struct PublicationUnknown final
    {
        PersistenceFailure failure;
        // Backend-owned reconciliation identity, not permission to release the lane.
        std::string token;
    };
    using VPublicationOutcome = std::variant<CommitReceipt, NotPublished, PublicationUnknown>;
    struct PublicationQuery final
    {
        WriteTicket ticket;
        WriteTarget target;
        std::shared_ptr<const EncodedArtifact> artifact;
        std::string token;
    };
    struct Reconciliation final
    {
        bool writer_retired{};
        VPublicationOutcome outcome;
    };
    class IArtifactStore
    {
    public:
        virtual ~IArtifactStore() = default;
        [[nodiscard]] virtual PersistenceResult<WriteTarget> resolve(std::string_view address) = 0;
        [[nodiscard]] virtual VPublicationOutcome publish(const PublicationQuery& work, std::stop_token stop = {}) = 0;
        // A matching read alone is insufficient. The backend must also retire all future write capability.
        [[nodiscard]] virtual Reconciliation reconcile(const PublicationQuery& work) = 0;
    };
}
