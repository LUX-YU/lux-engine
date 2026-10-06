#pragma once

#include <lux/engine/editor/persistence/SaveSource.hpp>
#include <lux/engine/editor/persistence/WriteCoordinator.hpp>
#include <lux/cxx/core/function_ref.hpp>

namespace lux::editor::persistence
{
    // Owner-thread role dispatch; workers receive EncodeWork/PublicationQuery owning values only.
    // Drain the execution adapter before destruction. Registrations are destroyed before their source.
    // During role callbacks/cleanup, status, token revocation and admitted encoding completion remain
    // available. New business/confirmation calls return BUSY; recursive adoption defers to the owner.
    class SaveService final
    {
    public:
        explicit SaveService(WriteCoordinator& coordinator, SaveLimits limits = {});
        // Declared activities retain the real shared coordinator; direct stack composition may borrow it.
        // The owning input must be non-null, as with the reference constructor.
        explicit SaveService(std::shared_ptr<WriteCoordinator> coordinator, SaveLimits limits = {});
        ~SaveService();
        SaveService(const SaveService&) = delete;
        SaveService& operator=(const SaveService&) = delete;
        [[nodiscard]] PersistenceResult<SaveSourceRegistration> registerSource(
            ISaveSource& source,
            lux::object::CodeLease code = lux::object::CodeLease::builtin()
        );
        // Preflight before consuming a completed load. Does not reserve or invoke a source.
        [[nodiscard]] PersistenceResult<void> canPrepareSource() const noexcept;
        // No describe callback: a factory supplies the real reserved identity while its Session is hidden.
        // Admitted rejection destroys the source under dispatch, before its final code pin. A pre-existing
        // BUSY scope is never released here; accepted encoding completion remains receivable during cleanup.
        [[nodiscard]] PersistenceResult<PreparedSaveSourceRegistration> prepareSource(
            sessions::SessionId,
            std::unique_ptr<ISaveSource>,
            lux::object::CodeLease = lux::object::CodeLease::builtin()
        );
        [[nodiscard]] PersistenceResult<void> canPublish(const PreparedSaveSourceRegistration&) const noexcept;
        // Requires a successful canPublish with no intervening callback/mutation, on the same owner.
        // This commit only changes prepared visibility; it allocates nothing and calls no source.
        [[nodiscard]] SaveSourceRegistration publish(PreparedSaveSourceRegistration&&) noexcept;
        // Reload commit: prepare the replacement role first, then run the domain's checked owner swap.
        // commit is synchronous, does not invoke extension callbacks, and leaves its domain unchanged on failure.
        // BUSY keeps the input with its caller. Admitted rejection/old-role cleanup stays under dispatch.
        [[nodiscard]] PersistenceResult<void> replaceSource(
            SaveSourceRegistration&,
            std::unique_ptr<ISaveSource>&,
            lux::object::CodeLease,
            cxx::function_ref<PersistenceResult<void>()> commit
        );
        [[nodiscard]] PersistenceResult<SaveId> requestSave(SaveRequest request);
        [[nodiscard]] PersistenceResult<SaveStatus> status(SaveId id) const;
        [[nodiscard]] PersistenceResult<ECancelResult> requestCancel(SaveId id);
        [[nodiscard]] PersistenceResult<void> acknowledge(SaveId id);
        [[nodiscard]] PersistenceResult<std::optional<EncodeWork>> takeEncoding();
        // Consumes exactly one result for an ENCODING operation on the owner, including during role
        // dispatch. Absorption settles bytes/errors/cancellation only, never calls a role or adopts.
        // Wrong thread, unknown ID or a duplicate/non-ENCODING result is a caller contract error;
        // BUSY denotes the latter, never temporary backpressure for a valid outstanding completion.
        [[nodiscard]] PersistenceResult<void> completeEncoding(SaveId id, PersistenceResult<EncodedArtifact> result);
        // Accept already settled disk facts on the owner, separately from worker completion collection.
        void adoptCompletions();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
