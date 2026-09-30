#include <lux/engine/editor/io/ProjectArtifactStore.hpp>
#include <lux/engine/editor/storage/FilePublication.hpp>

namespace lux::editor::io
{
    using namespace persistence;
    namespace
    {
        PersistenceFailure failure(const storage::FilePublicationFailure& error)
        {
            return {EPersistenceError::IO, error.path.generic_string(), error.native_code};
        }
        PersistenceResult<void> confirm(
            ProjectArtifactStore::ConfirmDurability callback,
            const std::filesystem::path& path,
            void* context
        )
        try
        {
            return callback(path, context);
        }
        catch (const std::bad_alloc&)
        {
            std::terminate();
        }
        catch (...)
        {
            return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::IO, "Durability callback failed"});
        }
    }
    ProjectArtifactStore::ProjectArtifactStore(std::filesystem::path root, ConfirmDurability confirm, void* context)
        : root_(std::move(root)), confirm_(confirm), context_(context)
    {}
    PersistenceResult<WriteTarget> ProjectArtifactStore::resolve(std::string_view address)
    {
        auto key = storage::publicationTargetKey(root_, std::filesystem::u8path(address));
        if (!key)
            return lux::cxx::unexpected(failure(key.error()));
        auto version = storage::publicationFileDigest(std::filesystem::u8path(*key));
        if (!version)
            return lux::cxx::unexpected(failure(version.error()));
        return WriteTarget{{std::move(*key)}, std::move(*version)};
    }
    VPublicationOutcome ProjectArtifactStore::publish(const PublicationQuery& work, std::stop_token stop)
    {
        const bool missing_payload = work.action == EPublicationAction::WRITE && !work.artifact;
        if (missing_payload || stop.stop_requested())
            return NotPublished{
                {stop.stop_requested() ? EPersistenceError::CANCELLED : EPersistenceError::INVALID_ARGUMENT}
            };
        auto resolved = resolve(work.target.key.value);
        if (!resolved)
            return NotPublished{resolved.error()};
        const bool is_conflict =
            resolved->key != work.target.key || resolved->expected_version != work.target.expected_version;
        if (is_conflict)
            return NotPublished{{EPersistenceError::CONFLICT, work.target.key.value}};
        const auto path = std::filesystem::u8path(work.target.key.value);
        if (work.action == EPublicationAction::REMOVE)
        {
            // This synchronous publisher is the sole remaining writer of this taken lane item.
            // A confirmed deletion is a disk fact even when directory durability cannot be confirmed.
            auto current = storage::publicationFileDigest(path);
            if (!current)
                return NotPublished{failure(current.error())};
            if (*current != work.target.expected_version)
                return NotPublished{{EPersistenceError::CONFLICT, work.target.key.value}};
            std::error_code error;
            std::filesystem::remove(path, error);
            if (error)
                return NotPublished{{EPersistenceError::IO, path.generic_string(), std::uint64_t(error.value())}};
            CommitReceipt receipt{"missing", EDurability::UNCONFIRMED};
            if (confirm_)
            {
                auto confirmed = confirm(confirm_, path, context_);
                if (!confirmed)
                    receipt.warning = confirmed.error();
            }
            return receipt;
        }
        auto staging_directory = path;
        staging_directory += ".lux-save-" + std::to_string(work.ticket.value) + ".tmp";
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error)
            return NotPublished{{EPersistenceError::IO, path.generic_string(), std::uint64_t(error.value())}};
        const bool created = std::filesystem::create_directory(staging_directory, error);
        if (!created || error)
            return NotPublished{
                {EPersistenceError::IO, staging_directory.generic_string(), std::uint64_t(error.value())}
            };
        const auto staged = staging_directory / "payload";
        struct Cleanup final
        {
            const std::filesystem::path& path;
            const std::filesystem::path& directory;
            ~Cleanup()
            {
                std::error_code error;
                std::filesystem::remove(path, error);
                std::filesystem::remove(directory, error);
            }
        } cleanup{staged, staging_directory};
        auto written = storage::writePublicationFile(staged, work.artifact->bytes);
        if (!written)
            return NotPublished{failure(written.error())};
        if (stop.stop_requested())
            return NotPublished{{EPersistenceError::CANCELLED}};
        // Recheck the precondition at commit, not only during request admission.
        auto current = storage::publicationFileDigest(path);
        if (!current)
            return NotPublished{failure(current.error())};
        if (*current != work.target.expected_version)
            return NotPublished{{EPersistenceError::CONFLICT}};
        CommitReceipt receipt{storage::publicationDigest(work.artifact->bytes), EDurability::FILE_FLUSHED};
        auto replaced = storage::replacePublicationFile(staged, path);
        if (!replaced)
            return NotPublished{failure(replaced.error())};
        if (confirm_)
        {
            auto confirmed = confirm(confirm_, path, context_);
            if (!confirmed)
            {
                receipt.durability = EDurability::UNCONFIRMED;
                receipt.warning = confirmed.error();
            }
        }
        return receipt;
    }
    Reconciliation ProjectArtifactStore::reconcile(const PublicationQuery& work)
    {
        // This synchronous backend has no detached writer after publish returns.
        auto current = resolve(work.target.key.value);
        if (!current)
            return {true, PublicationUnknown{current.error(), work.token}};
        const bool removed = work.action == EPublicationAction::REMOVE && current->expected_version == "missing";
        const bool written =
            work.artifact && current->expected_version == storage::publicationDigest(work.artifact->bytes);
        if (removed || written)
            return {true, CommitReceipt{current->expected_version, EDurability::UNCONFIRMED}};
        if (current->expected_version == work.target.expected_version)
            return {true, NotPublished{{EPersistenceError::IO, "Verified unchanged target after writer retirement"}}};
        return {
            true,
            PublicationUnknown{{EPersistenceError::CONFLICT, "Target changed to an unrelated version"}, work.token}
        };
    }
}
