#pragma once

#include <lux/engine/editor/storage/FileArtifactStore.hpp>

namespace lux::services
{
    struct ServiceDescriptor;
}

namespace lux::editor::storage
{
    struct PublicationRoots final
    {
        std::filesystem::path project, user, installation;
    };
    extern const services::ServiceDescriptor kPublicationFileStoreService;

    // Physical publication roots, not an asset VFS. Every selected backend enforces its own root;
    // installation data outside explicit writable roots is read only. Supply normalized roots.
    class PublicationFileStore final : public persistence::IArtifactStore
    {
    public:
        PublicationFileStore(
            std::filesystem::path project,
            std::filesystem::path user,
            std::filesystem::path installation
        );
        PublicationFileStore(const PublicationFileStore&) = delete;
        PublicationFileStore& operator=(const PublicationFileStore&) = delete;
        PublicationFileStore(PublicationFileStore&&) = delete;
        PublicationFileStore& operator=(PublicationFileStore&&) = delete;

        [[nodiscard]] persistence::PersistenceResult<persistence::WriteTarget>
        resolve(std::string_view address) override;
        [[nodiscard]] persistence::VPublicationOutcome
        publish(const persistence::PublicationQuery&, std::stop_token = {}) override;
        [[nodiscard]] persistence::Reconciliation reconcile(const persistence::PublicationQuery&) override;

    private:
        [[nodiscard]] FileArtifactStore& select(std::string_view);
        FileArtifactStore project_, user_, installation_;
        std::string project_prefix_, user_prefix_, installation_prefix_;
    };
} // namespace lux::editor::storage
