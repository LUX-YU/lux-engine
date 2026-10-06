#pragma once

#include <lux/engine/editor/persistence/ArtifactStore.hpp>
#include <memory>

namespace lux::editor::persistence
{
    class WriteCoordinator;
    // Bounded observation of one physical lane. It does not block writers and must precede its
    // coordinator's destruction. A later admission invalidates the observation even after acknowledgement.
    class WriteObservation final
    {
    public:
        ~WriteObservation() noexcept;
        WriteObservation(WriteObservation&&) noexcept;
        WriteObservation& operator=(WriteObservation&&) noexcept;
        WriteObservation(const WriteObservation&) = delete;
        WriteObservation& operator=(const WriteObservation&) = delete;
        [[nodiscard]] PersistenceResult<void> validate() const;

    private:
        friend class WriteCoordinator;
        WriteObservation(WriteCoordinator&, WriteTargetKey, std::uint64_t) noexcept;
        WriteCoordinator* owner_{};
        WriteTargetKey key_;
        std::uint64_t revision_{};
    };
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
        // Requires an idle target; UNKNOWN is still a writer. Used across asynchronous source reads.
        [[nodiscard]] PersistenceResult<WriteObservation> observeIdle(WriteTargetKey);
        [[nodiscard]] PersistenceResult<void> provideEncoded(WriteTicket ticket, EncodedArtifact artifact);
        // Removal is a publication in the same bounded FIFO lane, with no encoded payload.
        [[nodiscard]] PersistenceResult<void> provideRemoval(WriteTicket ticket);
        [[nodiscard]] PersistenceResult<void> cancelBeforePublish(WriteTicket ticket, PersistenceFailure failure);
        [[nodiscard]] PersistenceResult<std::optional<PublicationQuery>> takeReady();
        [[nodiscard]] PersistenceResult<void> complete(WriteTicket ticket, VPublicationOutcome outcome);
        [[nodiscard]] PersistenceResult<void> reconcile(WriteTicket ticket, IArtifactStore& store);
        [[nodiscard]] PersistenceResult<WriteStatus> status(WriteTicket ticket) const;
        [[nodiscard]] PersistenceResult<void> acknowledge(WriteTicket ticket);
        [[nodiscard]] std::size_t size() const noexcept;

    private:
        friend class WriteObservation;
        [[nodiscard]] PersistenceResult<void> validate(const WriteObservation&) const;
        void release(const WriteObservation&) noexcept;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
    // Admits an already frozen derived product without source-save version inheritance.
    [[nodiscard]] PersistenceResult<WriteTicket> publishEncodedArtifact(
        WriteCoordinator&,
        WriteTarget,
        EncodedArtifact
    );

}
