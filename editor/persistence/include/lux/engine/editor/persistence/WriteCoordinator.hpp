#pragma once

#include <lux/engine/editor/persistence/ArtifactStore.hpp>
#include <memory>

namespace lux::editor::persistence
{
    enum class EWriteStage : std::uint8_t
    {
        RESERVED,
        READY,
        PUBLISHING,
        UNKNOWN,
        TERMINAL
    };
    struct WriteStatus final
    {
        EWriteStage stage{EWriteStage::RESERVED};
        std::optional<VPublicationOutcome> outcome;
    };
    struct WriteLimits final
    {
        std::size_t tickets{128};
        std::size_t artifact_bytes{256 * 1024 * 1024};
    };
    // Owner-thread coordinator. Every producer that can address the same target borrows this instance.
    class WriteCoordinator final
    {
    public:
        explicit WriteCoordinator(WriteLimits limits = {});
        ~WriteCoordinator();
        WriteCoordinator(const WriteCoordinator&) = delete;
        WriteCoordinator& operator=(const WriteCoordinator&) = delete;
        [[nodiscard]] PersistenceResult<WriteTicket> reserve(WriteTarget target, WriteOrigin origin);
        [[nodiscard]] PersistenceResult<void> provideEncoded(WriteTicket ticket, EncodedArtifact artifact);
        [[nodiscard]] PersistenceResult<void> cancelBeforePublish(WriteTicket ticket, PersistenceFailure failure);
        [[nodiscard]] PersistenceResult<std::optional<PublicationQuery>> takeReady();
        [[nodiscard]] PersistenceResult<void> complete(WriteTicket ticket, VPublicationOutcome outcome);
        [[nodiscard]] PersistenceResult<void> reconcile(WriteTicket ticket, IArtifactStore& store);
        [[nodiscard]] PersistenceResult<WriteStatus> status(WriteTicket ticket) const;
        [[nodiscard]] PersistenceResult<void> acknowledge(WriteTicket ticket);
        [[nodiscard]] std::size_t size() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
