#include <lux/engine/editor/storage/PublicationFileStore.hpp>

namespace lux::editor::storage
{
    namespace
    {
        std::string prefix(const std::filesystem::path& root)
        {
            const auto bytes = (root / "").generic_u8string();
            return {bytes.begin(), bytes.end()};
        }
        persistence::NotPublished readOnly()
        {
            return {{persistence::EPersistenceError::UNSUPPORTED_TARGET, "Installation settings are read only"}};
        }
    } // namespace

    PublicationFileStore::PublicationFileStore(
        std::filesystem::path project,
        std::filesystem::path user,
        std::filesystem::path installation
    )
        : project_(project), user_(user), installation_(installation), project_prefix_(prefix(project)),
          user_prefix_(prefix(user)), installation_prefix_(prefix(installation))
    {
    }
    FileArtifactStore& PublicationFileStore::select(std::string_view key)
    {
        if (key.starts_with(user_prefix_))
        {
            return user_;
        }
        const bool is_installation = !key.starts_with(project_prefix_) && key.starts_with(installation_prefix_);
        if (is_installation)
        {
            return installation_;
        }
        return project_;
    }
    persistence::PersistenceResult<persistence::WriteTarget> PublicationFileStore::resolve(std::string_view address)
    {
        return select(address).resolve(address);
    }
    persistence::VPublicationOutcome PublicationFileStore::publish(
        const persistence::PublicationQuery& query,
        std::stop_token stop
    )
    {
        auto& target = select(query.target.key.value);
        if (&target == &installation_)
        {
            return readOnly();
        }
        return target.publish(query, stop);
    }
    persistence::Reconciliation PublicationFileStore::reconcile(const persistence::PublicationQuery& query)
    {
        auto& target = select(query.target.key.value);
        if (&target == &installation_)
        {
            return {true, readOnly()};
        }
        return target.reconcile(query);
    }
} // namespace lux::editor::storage
