#include <lux/engine/editor/project/Project.hpp>
#include <lux/engine/resource/asset/storage/pak/PakAssetProvider.hpp>
#include <algorithm>
#include <fstream>
#include <unordered_set>
#include <atomic>

namespace lux::editor
{
    namespace
    {
        EditorResult<std::uint64_t> nextProjectInstance() noexcept
        {
            static std::atomic<std::uint64_t> next{1};
            auto value = next.load(std::memory_order_relaxed);
            while (value != UINT64_MAX)
            {
                if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
                {
                    return value;
                }
            }
            return lux::cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "project.identity"});
        }
    }
    EditorResult<ProjectSource> readProjectSource(const std::filesystem::path &file)
    {
        std::error_code error;
        const auto absolute = std::filesystem::absolute(file, error);
        if (error)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "filesystem",
                                                      static_cast<std::uint64_t>(error.value()), error.message()});
        }
        auto lease = ProjectWriteLease::acquire(absolute.parent_path());
        if (!lease)
        {
            return lux::cxx::unexpected(lease.error());
        }
        if (lease->writable())
        {
            auto recovered = recoverProjectFiles(absolute.parent_path());
            if (!recovered)
            {
                return lux::cxx::unexpected(recovered.error());
            }
        }
        else
        {
            const bool publishing = std::filesystem::exists(absolute.parent_path() / ".lux-editor-publication", error);
            if (error)
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "project.publication",
                    static_cast<std::uint64_t>(error.value()), error.message()});
            }
            if (publishing)
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "project.publication", 0,
                    "The writer is publishing this project; retry after it reaches a stable state"});
            }
        }
        const auto size = std::filesystem::file_size(absolute, error);
        if (error || size > ProjectManifestLimits{}.max_bytes)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "project.read",
                                                      static_cast<std::uint64_t>(error.value()),
                                                      "Project is missing, unreadable or exceeds the size limit"});
        }

        std::ifstream input(absolute, std::ios::binary);
        std::string bytes(static_cast<std::size_t>(size), '\0');
        if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
        {
            return lux::cxx::unexpected(
                EditorFailure{EEditorError::SOURCE_FAILURE, "project.read", 0, "Cannot read the project manifest"});
        }
        auto decoded = decodeProjectManifest(bytes);
        if (!decoded)
        {
            const auto &cause = decoded.error();
            return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "project.codec",
                                                      static_cast<std::uint64_t>(cause.code), cause.field, cause});
        }

        ProjectSource result{std::move(*decoded), absolute, {}};
        result.write_lease = std::move(*lease);
        result.manifest_digest = projectContentDigest(std::as_bytes(std::span(bytes)));
        std::unordered_set<std::string> loaded;
        for (const auto &item : result.manifest.assets)
        {
            auto source_digest = projectFileDigest(absolute.parent_path() / item.source_path);
            if (!source_digest)
            {
                return lux::cxx::unexpected(source_digest.error());
            }
            result.source_digests.emplace_back(item.source_path, std::move(*source_digest));
            if (item.cooked_path.empty() || !loaded.insert(item.cooked_path).second)
            {
                continue;
            }
            auto package = readProjectPackage(absolute.parent_path(), item.cooked_path);
            if (!package)
            {
                return lux::cxx::unexpected(package.error());
            }
            if (item.cooked_path != item.source_path)
            {
                auto digest = projectFileDigest(absolute.parent_path() / item.cooked_path);
                if (!digest)
                {
                    return lux::cxx::unexpected(digest.error());
                }
                result.source_digests.emplace_back(item.cooked_path, std::move(*digest));
            }
            result.mounts.push_back(std::move(*package));
        }
        return result;
    }

    Project::Project(object::ObjectDispatcherRef dispatcher, std::uint64_t instance)
        : Object(std::move(dispatcher)), instance_(instance) {}

    EditorResult<std::unique_ptr<Project>> Project::open(ProjectSource &source, process::BlockingScheduler blocking,
                                                         object::ObjectDispatcherRef dispatcher)
    {
        if (!blocking || !dispatcher || !dispatcher.isCurrent())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "project.capabilities"});
        }
        auto instance = nextProjectInstance();
        if (!instance)
        {
            return lux::cxx::unexpected(instance.error());
        }
        auto result = std::unique_ptr<Project>(new Project(std::move(dispatcher), *instance));
        result->mounts_.reserve(source.mounts.size());
        for (const auto &mount : source.mounts)
        {
            const auto mounted = result->vfs_.mount(mount.mount);
            if (mounted == asset::kInvalidMountId)
            {
                return lux::cxx::unexpected(
                    EditorFailure{EEditorError::SOURCE_FAILURE, "project.mount", 0, mount.mount.root});
            }
            result->mounts_.push_back({mount.path, mounted, mount.entries});
        }
        auto reads = process::asset_loading::VfsAssetReadEndpoint::create(result->vfs_.view(), blocking, {256});
        if (!reads)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::EXECUTION_FAILURE,
                                                      "asset.read.endpoint",
                                                      static_cast<std::uint64_t>(reads.error()),
                                                      {},
                                                      reads.error()});
        }
        result->reads_ = std::move(*reads);
        result->root_ = source.file.parent_path();
        result->source_ = std::move(source);
        result->source_.mounts.clear();
        result->rebuildCatalog();
        return result;
    }

    Project::~Project() = default;

    const ProjectAssetEntry *Project::asset(asset::AssetId id) const noexcept
    {
        const auto found = std::ranges::find(source_.manifest.assets, id, &ProjectAssetEntry::id);
        return found == source_.manifest.assets.end() ? nullptr : std::addressof(*found);
    }

    process::asset_loading::AssetReadPort Project::assetReads() const noexcept
    {
        return reads_->port();
    }

    void Project::rebuildCatalog()
    {
        catalog_.clear();
        catalog_by_id_.clear();
        catalog_.reserve(source_.manifest.assets.size());
        for (const auto& entry : source_.manifest.assets)
        {
            catalog_by_id_.emplace(entry.id, catalog_.size());
            catalog_.push_back({entry.id, entry.id, 0, entry.mount_path.empty() ? entry.source_path : entry.mount_path});
        }
        std::unordered_set<asset::AssetId> claimed;
        for (auto package = mounts_.rbegin(); package != mounts_.rend(); ++package)
        {
            const auto source = std::ranges::find(source_.manifest.assets, package->path, &ProjectAssetEntry::cooked_path);
            const auto source_id = source == source_.manifest.assets.end() ? asset::AssetId{} : source->id;
            for (const auto& entry : package->entries)
            {
                if (!claimed.insert(entry.id).second || entry.tombstone)
                {
                    continue;
                }
                auto [found, inserted] = catalog_by_id_.try_emplace(entry.id, catalog_.size());
                if (inserted)
                {
                    catalog_.push_back({entry.id, source_id, entry.magic_number, entry.vpath});
                }
                else
                {
                    catalog_[found->second].magic = entry.magic_number;
                }
            }
        }
        std::ranges::sort(catalog_, {}, &AssetCatalogEntry::path);
        for (std::size_t index{}; index < catalog_.size(); ++index)
        {
            catalog_by_id_[catalog_[index].id] = index;
        }
        ++catalog_revision_;
    }

    const AssetCatalogEntry* Project::catalogAsset(asset::AssetId id) const noexcept
    {
        const auto found = catalog_by_id_.find(id);
        return found == catalog_by_id_.end() ? nullptr : &catalog_[found->second];
    }

    std::string_view Project::assetName(asset::AssetId id) const noexcept
    {
        if (const auto* entry = catalogAsset(id))
        {
            return entry->path;
        }
        return id.isNull() ? "None" : "Unresolved asset";
    }

    AssetReference Project::reference(asset::AssetId id) const noexcept
    {
        return {instance_, catalog_revision_, id};
    }

    EditorResult<asset::AssetId> Project::resolveReference(AssetReference reference, std::uint32_t magic) const
    {
        const auto fail = [](EAssetReferenceError code)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "project.asset-reference",
                static_cast<std::uint64_t>(code), {}, code});
        };
        if (reference.project_instance != instance_)
        {
            return fail(EAssetReferenceError::FOREIGN_PROJECT);
        }
        if (reference.catalog_revision != catalog_revision_)
        {
            return fail(EAssetReferenceError::STALE_CATALOG);
        }
        const auto* entry = catalogAsset(reference.asset);
        if (!entry)
        {
            return fail(EAssetReferenceError::MISSING_ASSET);
        }
        if (magic && entry->magic != magic)
        {
            return fail(EAssetReferenceError::WRONG_TYPE);
        }
        return reference.asset;
    }

    ProjectPublication::~ProjectPublication()
    {
        if (owner_)
        {
            owner_->publishing_ = false;
        }
    }

    ProjectPublication::ProjectPublication(ProjectPublication&& other) noexcept
        : root(std::move(other.root)), manifest_path(std::move(other.manifest_path)),
          before_manifest_digest(std::move(other.before_manifest_digest)), manifest(std::move(other.manifest)),
          files(std::move(other.files)), package_paths(std::move(other.package_paths)), owner_(std::exchange(other.owner_, nullptr)) {}

    ProjectPublication& ProjectPublication::operator=(ProjectPublication&& other) noexcept
    {
        if (this != &other)
        {
            ProjectPublication released(std::move(*this));
            root = std::move(other.root);
            manifest_path = std::move(other.manifest_path);
            before_manifest_digest = std::move(other.before_manifest_digest);
            manifest = std::move(other.manifest);
            files = std::move(other.files);
            package_paths = std::move(other.package_paths);
            owner_ = std::exchange(other.owner_, nullptr);
        }
        return *this;
    }

    std::string_view Project::sourceDigest(std::string_view path) const noexcept
    {
        const auto found = std::ranges::find(source_.source_digests, path, [](const auto& pair) { return pair.first; });
        return found == source_.source_digests.end() ? std::string_view{"missing"} : std::string_view{found->second};
    }

    EditorResult<ProjectPublication> Project::preparePublication(ProjectUpdate& update)
    {
        if (!writable())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::READ_ONLY, "project.publication"});
        }
        if (publishing_)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "project.publication"});
        }
        if (catalog_revision_ == UINT64_MAX)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "project.catalog"});
        }
        auto next = source_.manifest;
        for (const auto id : update.removed)
        {
            std::erase_if(next.assets, [id](const auto& asset) { return asset.id == id; });
        }
        for (const auto& asset : update.assets)
        {
            const auto existing = std::ranges::find(next.assets, asset.id, &ProjectAssetEntry::id);
            if (existing == next.assets.end())
            {
                next.assets.push_back(asset);
            }
            else
            {
                *existing = asset;
            }
        }
        auto valid = validateProjectManifest(next);
        if (!valid)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "project.manifest",
                static_cast<std::uint64_t>(valid.error().code), valid.error().field, valid.error()});
        }
        for (const auto& file : update.files)
        {
            if (!validProjectPath(file.path) || file.before_digest.empty())
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "project.publication", 0, file.path});
            }
        }
        ProjectPublication publication;
        publication.root = root_;
        publication.manifest_path = source_.file.filename().generic_string();
        publication.before_manifest_digest = source_.manifest_digest;
        publication.manifest = std::move(next);
        std::unordered_set<std::string> packages;
        for (const auto& entry : publication.manifest.assets)
        {
            if (entry.cooked_path.empty() || !packages.insert(entry.cooked_path).second)
            {
                continue;
            }
            const bool already_mounted = std::ranges::find(mounts_, entry.cooked_path, &MountedPackage::path) != mounts_.end();
            const auto written = std::ranges::find(update.files, entry.cooked_path, &ProjectFileChange::path);
            if (already_mounted && written != update.files.end())
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "project.package.immutable", 0,
                    entry.cooked_path});
            }
            if (!already_mounted)
            {
                publication.package_paths.push_back(entry.cooked_path);
            }
        }
        publication.files = std::move(update.files);
        publication.owner_ = this;
        publishing_ = true;
        return publication;
    }

    EditorResult<void> Project::adoptPublication(ProjectPublication& publication, ProjectPublicationReceipt& receipt)
    {
        if (publication.owner_ != this || !publishing_)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "project.publication.owner"});
        }
        if (receipt.manifest != publication.manifest || receipt.packages.size() != publication.package_paths.size())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "project.publication.receipt"});
        }
        std::vector<asset::MountDesc> added;
        added.reserve(receipt.packages.size());
        for (std::size_t index{}; index < receipt.packages.size(); ++index)
        {
            const auto& package = receipt.packages[index];
            if (package.path != publication.package_paths[index])
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "project.publication.package"});
            }
            added.push_back(package.mount);
        }
        std::vector<asset::MountId> removed;
        for (const auto& package : mounts_)
        {
            if (std::ranges::find(receipt.manifest.assets, package.path, &ProjectAssetEntry::cooked_path) == receipt.manifest.assets.end())
            {
                removed.push_back(package.id);
            }
        }
        std::unordered_set<asset::AssetId> changed_assets;
        for (const auto& package : mounts_)
        {
            if (std::ranges::find(removed, package.id) != removed.end())
            {
                for (const auto& entry : package.entries)
                {
                    changed_assets.insert(entry.id);
                }
            }
        }
        for (const auto& package : receipt.packages)
        {
            for (const auto& entry : package.entries)
            {
                changed_assets.insert(entry.id);
            }
        }
        auto mounted = vfs_.replaceMounts(removed, added);
        if (!mounted)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "project.mount",
                static_cast<std::uint64_t>(mounted.error()), {}, mounted.error()});
        }
        std::erase_if(mounts_, [&](const auto& package)
        {
            return std::ranges::find(removed, package.id) != removed.end();
        });
        for (std::size_t index{}; index < receipt.packages.size(); ++index)
        {
            auto& package = receipt.packages[index];
            mounts_.push_back({std::move(package.path), (*mounted)[index], std::move(package.entries)});
        }
        source_.manifest = std::move(receipt.manifest);
        source_.manifest_digest = std::move(receipt.manifest_digest);
        for (auto& [path, digest] : receipt.file_digests)
        {
            auto found = std::ranges::find(source_.source_digests, path, [](const auto& pair) { return pair.first; });
            if (found == source_.source_digests.end())
            {
                source_.source_digests.emplace_back(std::move(path), std::move(digest));
            }
            else
            {
                found->second = std::move(digest);
            }
        }
        rebuildCatalog();
        notify<catalogChanged>(catalog_revision_);
        for (const auto id : changed_assets)
        {
            notify<assetContentChanged>(id);
        }
        return {};
    }

    void Project::requestClose() noexcept
    {
        reads_->requestStop();
    }

    EditorResult<bool> Project::advanceClose()
    {
        if (publishing_)
        {
            return false;
        }
        const auto joined = reads_->join();
        if (!joined)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::EXECUTION_FAILURE, "asset.read.join",
                                                      static_cast<std::uint64_t>(joined.error())});
        }
        for (const auto& package : mounts_)
        {
            vfs_.unmount(package.id);
        }
        mounts_.clear();
        return true;
    }
} // namespace lux::editor
