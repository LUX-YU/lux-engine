#pragma once

#include <lux/engine/editor/persistence/ArtifactStore.hpp>
#include <filesystem>

namespace lux::editor::storage
{
    class FileArtifactStore final : public persistence::IArtifactStore
    {
    public:
        // Optional host durability extension. Runs only after the real replacement succeeded.
        using ConfirmDurability = persistence::PersistenceResult<void> (*)(const std::filesystem::path&, void*);
        explicit FileArtifactStore(
            std::filesystem::path root,
            ConfirmDurability confirm = nullptr,
            void* context = nullptr
        );
        FileArtifactStore(const FileArtifactStore&) = delete;
        FileArtifactStore& operator=(const FileArtifactStore&) = delete;
        FileArtifactStore(FileArtifactStore&&) = delete;
        FileArtifactStore& operator=(FileArtifactStore&&) = delete;
        [[nodiscard]] persistence::PersistenceResult<persistence::WriteTarget> resolve(std::string_view address
        ) override;
        [[nodiscard]] persistence::VPublicationOutcome publish(
            const persistence::PublicationQuery&,
            std::stop_token = {}
        ) override;
        [[nodiscard]] persistence::Reconciliation reconcile(const persistence::PublicationQuery&) override;

    private:
        std::filesystem::path root_;
        ConfirmDurability confirm_{};
        void* context_{};
    };
}
