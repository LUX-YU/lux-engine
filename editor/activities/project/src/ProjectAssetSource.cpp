#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/editor/storage/ProjectContentOpening.hpp>
#include <lux/engine/editor/persistence/ArtifactStore.hpp>
#include <lux/engine/platform/FilePath.hpp>

#include <algorithm>
#include <fstream>

namespace lux::editor
{
    namespace
    {
        // A fixed source location and version. A delayed first read cannot silently adopt a newer file.
        class SourceProvider final : public asset::IAssetProvider
        {
        public:
            SourceProvider(
                std::filesystem::path root,
                ProjectAssetEntry entry,
                std::size_t limit,
                std::string expected_digest
            )
                : root_(std::move(root)), entry_(std::move(entry)), limit_(limit), digest_(std::move(expected_digest))
            {}

            std::optional<asset::AssetId> resolve(std::string_view path) const override
            {
                return path == entry_.source_path ? std::optional{entry_.id} : std::nullopt;
            }
            bool contains(const asset::AssetId& id) const override
            {
                return id == entry_.id;
            }
            std::optional<std::string> pathOf(const asset::AssetId& id) const override
            {
                return contains(id) ? std::optional{entry_.source_path} : std::nullopt;
            }
            void enumerate(const std::function<void(const asset::ProviderEntry&)>& visit) const override
            {
                visit({entry_.id, 0, entry_.source_path});
            }
            lux::cxx::expected<asset::AssetBlob, asset::EAssetStorageError>
            open(const asset::AssetId& id, std::size_t max_bytes) const noexcept override
            {
                using Error = asset::EAssetStorageError;
                if (!contains(id))
                    return lux::cxx::unexpected(Error::NOT_FOUND);
                std::error_code error;
                const auto root = std::filesystem::canonical(lux::engine::platform::nativeFilePath(root_), error);
                if (error)
                    return lux::cxx::unexpected(Error::IO_FAILURE);
                const auto path = std::filesystem::canonical(
                    lux::engine::platform::nativeFilePath(root / std::filesystem::u8path(entry_.source_path)),
                    error
                );
                if (error)
                    return lux::cxx::unexpected(Error::IO_FAILURE);
                const auto relative = path.lexically_relative(root);
                const bool escapes = relative.empty() || relative.is_absolute() ||
                                     std::ranges::any_of(relative, [](const auto& part) { return part == ".."; });
                if (escapes)
                    return lux::cxx::unexpected(Error::UNSUPPORTED);
                const auto native_path = lux::engine::platform::nativeFilePath(path);
                const auto size = std::filesystem::file_size(native_path, error);
                if (error)
                    return lux::cxx::unexpected(Error::IO_FAILURE);
                if (size > std::min(limit_, max_bytes))
                    return lux::cxx::unexpected(Error::LIMIT_EXCEEDED);
                auto bytes = std::make_shared<std::vector<std::byte>>(static_cast<std::size_t>(size));
                std::ifstream file(native_path, std::ios::binary);
                if (!file.read(reinterpret_cast<char*>(bytes->data()), static_cast<std::streamsize>(size)) ||
                    file.peek() != std::char_traits<char>::eof() || file.bad())
                    return lux::cxx::unexpected(Error::IO_FAILURE);
                if (projectContentDigest(*bytes) != digest_)
                    return lux::cxx::unexpected(Error::CONTENT_CHANGED);
                return asset::AssetBlob::fromShared(lux::cxx::SharedBytes<>::fromOwner(bytes, *bytes));
            }

        private:
            std::filesystem::path root_;
            ProjectAssetEntry entry_;
            std::size_t limit_;
            std::string digest_;
        };
    }

    EditorResult<asset::AssetVfsView> ProjectStorage::captureSource(
        asset::AssetId id,
        std::size_t max_bytes,
        std::string expected_digest
    ) const noexcept
    {
        const auto* entry = asset(id);
        if (closing_ || !entry || !max_bytes || expected_digest.empty())
            return lux::cxx::unexpected(
                EditorFailure{closing_ ? EEditorError::CLOSING : EEditorError::INVALID_ARGUMENT, "source.capture"}
            );
        asset::AssetVfs sources;
        const auto mount = sources.mount(
            {"/sources", std::make_shared<SourceProvider>(root_, *entry, max_bytes, std::move(expected_digest)), 0}
        );
        if (mount == asset::kInvalidMountId)
            return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "source.mount"});
        return sources.view().capture();
    }
}

namespace lux::editor
{
    namespace
    {
        template <class Error> auto openingFailure(std::string domain, const Error& cause)
        {
            auto code = EEditorError::SOURCE_FAILURE;
            if constexpr (requires { cause.code == decltype(cause.code)::BUSY; })
            {
                if (cause.code == decltype(cause.code)::BUSY)
                    code = EEditorError::BUSY;
            }
            else if constexpr (requires { cause == Error::BUSY; })
            {
                if (cause == Error::BUSY)
                    code = EEditorError::BUSY;
            }
            if constexpr (requires { cause.session == sessions::ESessionError::BUSY; })
            {
                if (cause.session == sessions::ESessionError::BUSY)
                    code = EEditorError::BUSY;
            }
            if constexpr (requires { cause.retryable; })
                if (cause.retryable)
                    code = EEditorError::BUSY;
            return cxx::unexpected(EditorFailure{code, std::move(domain), 0, {}, cause});
        }
    }
    EditorResult<sessions::OpenAssetId> openProjectContent(
        ProjectStorage& project, persistence::IArtifactStore& files, sessions::SessionOpening& opening,
        AssetReference reference, const sessions::SessionFactorySnapshot& snapshot
    )
    {
        auto resolved = project.resolveReference(reference, 0);
        if (!resolved)
            return cxx::unexpected(resolved.error());
        const auto* asset = project.asset(*resolved);
        if (!asset)
            return cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "project.source"});
        const auto entry = *asset;
        auto factory = snapshot.selectSource(entry.source_type, entry.source_version);
        if (!factory)
        {
            return openingFailure("asset.authoring", factory.error());
        }
        auto target = files.resolve(entry.source_path);
        if (!target)
            return openingFailure("source.target", target.error());
        // Resolving an external backend can invoke code. Revalidate the original catalog reference
        // before capturing source bytes, rather than retaining a catalog pointer across that call.
        if (auto current = project.resolveReference(reference, 0); !current)
            return cxx::unexpected(current.error());
        auto source = project.captureSource(entry.id, 64 * 1024 * 1024, target->expected_version);
        if (!source)
            return cxx::unexpected(source.error());
        sessions::OpenAssetRequest request{
            reference.project_instance,
            (*factory)->descriptor().kind,
            {std::move(*source), entry.id, sessions::BoundSource{entry.id, target->key.value}, *target}
        };
        auto opened = opening.open(std::move(request), snapshot);
        if (!opened)
            return openingFailure("open.admission", opened.error());
        return *opened;
    }
}
