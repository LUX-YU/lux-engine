#pragma once

#include <lux/engine/editor/persistence/SaveSource.hpp>
#include <lux/engine/editor/persistence/WriteCoordinator.hpp>

namespace lux::editor::persistence
{
    // Owner-thread role dispatch; workers receive EncodeWork/PublicationQuery owning values only.
    // Drain the execution adapter before destruction. Registrations are destroyed before their source.
    class SaveService final
    {
    public:
        explicit SaveService(WriteCoordinator& coordinator, SaveLimits limits = {});
        ~SaveService();
        SaveService(const SaveService&) = delete;
        SaveService& operator=(const SaveService&) = delete;
        [[nodiscard]] PersistenceResult<SaveSourceRegistration> registerSource(
            ISaveSource& source,
            contracts::CodeLease code = contracts::CodeLease::builtin()
        );
        [[nodiscard]] PersistenceResult<SaveId> requestSave(SaveRequest request);
        [[nodiscard]] PersistenceResult<SaveStatus> status(SaveId id) const;
        [[nodiscard]] PersistenceResult<ECancelResult> requestCancel(SaveId id);
        [[nodiscard]] PersistenceResult<void> acknowledge(SaveId id);
        [[nodiscard]] PersistenceResult<std::optional<EncodeWork>> takeEncoding();
        [[nodiscard]] PersistenceResult<void> completeEncoding(SaveId id, PersistenceResult<EncodedArtifact> result);
        // Accept already settled disk facts on the owner, separately from worker completion collection.
        void adoptCompletions();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
