#include <lux/engine/editor/storage/PublicationFileStore.hpp>
#include <lux/engine/editor/storage/FilePublication.hpp>
#include <lux/engine/services/ServiceRegistry.hpp>

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
        constexpr services::ServiceContract contracts[]{
            services::ServiceContract::forType<PublicationFileStore, persistence::IArtifactStore>(
                services::ServiceNameView{"lux.editor.persistence.files"}
            )
        };
        services::ServiceResult<std::unique_ptr<PublicationFileStore>>
        createFiles(services::ServiceResolver& resolver, const services::ServiceConfiguration&) noexcept
        {
            auto roots = resolver.definition<PublicationRoots>();
            if (!roots)
            {
                return cxx::unexpected(std::move(roots.error()));
            }
            auto normalized = **roots;
            for (auto* path : {&normalized.project, &normalized.user, &normalized.installation})
            {
                if (path->empty())
                {
                    return cxx::unexpected(services::ServiceFailure{
                        services::EServiceError::INVALID_CONFIGURATION, "Empty publication root", "publication.roots"
                    });
                }
                auto key = publicationTargetKey(*path, ".lux-publication-root");
                if (!key)
                {
                    const auto bytes = key.error().path.generic_u8string();
                    std::string detail(bytes.begin(), bytes.end());
                    detail += " (native " + std::to_string(key.error().native_code) + ")";
                    return cxx::unexpected(services::ServiceFailure{
                        services::EServiceError::INVALID_CONFIGURATION, std::move(detail),
                        "publication.roots", static_cast<std::uint64_t>(key.error().code)
                    });
                }
                *path = std::filesystem::u8path(*key).parent_path();
            }
            return std::make_unique<PublicationFileStore>(
                std::move(normalized.project), std::move(normalized.user), std::move(normalized.installation)
            );
        }
    } // namespace

    constinit const services::ServiceDescriptor kPublicationFileStoreService = []
    {
        auto descriptor = services::ServiceDescriptor::forType<PublicationFileStore, createFiles>(
            services::ServiceNameView{"lux.editor.persistence.files"}, contracts
        );
        descriptor.retention = services::EServiceRetention::SCOPED;
        descriptor.definition_type = cxx::typeToken<PublicationRoots>();
        return descriptor;
    }();

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
        const auto path = std::filesystem::u8path(address);
        if (!path.is_absolute())
        {
            return project_.resolve(address);
        }
        // Routing needs the same physical spelling as a coordinator key (case/links/separators).
        // Resolving within the selected backend still enforces that root; normalizing is not permission.
        auto key = publicationTargetKey(path.root_path(), path);
        if (!key)
        {
            return cxx::unexpected(persistence::PersistenceFailure{
                persistence::EPersistenceError::IO, key.error().path.generic_string(), key.error().native_code
            });
        }
        return select(*key).resolve(*key);
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
