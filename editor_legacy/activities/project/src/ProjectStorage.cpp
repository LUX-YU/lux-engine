#include <lux/engine/editor/detail/SignalDelivery.hpp>
#include <lux/engine/log/Log.hpp>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <lux/engine/editor/PublicationProbe.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/resource/asset/storage/pak/PakAssetProvider.hpp>
#include <unordered_set>

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
    } // namespace
    ProjectStorage::ProjectStorage(
        object::ObjectDispatcherRef dispatcher,
        asset::AssetVfs& assets,
        std::uint64_t instance,
        process::TaskScope& tasks,
        process::BlockingScheduler blocking
    )
        : lux::object::LuxObject(dispatcher), tasks_(tasks), blocking_(blocking), vfs_(assets),
          catalog_(std::move(dispatcher), instance)
    {}

    EditorResult<std::unique_ptr<ProjectStorage>> ProjectStorage::open(
        PreparedProjectOpen& source,
        asset::AssetVfs& assets,
        process::BlockingScheduler blocking,
        process::TaskScope& tasks,
        object::ObjectDispatcherRef dispatcher
    )
    {
        if (source.file_.empty())
            return cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "project.open.consumed"});
        if (!blocking || !dispatcher || !dispatcher.isCurrent())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "project.capabilities"});
        }
        auto instance = nextProjectInstance();
        if (!instance)
        {
            return lux::cxx::unexpected(instance.error());
        }
        auto result = std::unique_ptr<ProjectStorage>(
            new ProjectStorage(std::move(dispatcher), assets, *instance, tasks, blocking)
        );
        result->mounts_.reserve(source.mounts_.size());
        for (const auto& mount : source.mounts_)
        {
            const auto mounted = result->vfs_.mount(mount.mount);
            if (mounted == asset::kInvalidMountId)
            {
                return lux::cxx::unexpected(
                    EditorFailure{EEditorError::SOURCE_FAILURE, "project.mount", 0, mount.mount.root}
                );
            }
            result->mounts_.push_back({mount.path, mounted, mount.entries});
        }
        auto reads = process::asset_loading::VfsAssetReadEndpoint::create(result->vfs_.view(), blocking, tasks, {256});
        if (!reads)
        {
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::EXECUTION_FAILURE,
                "asset.read.endpoint",
                static_cast<std::uint64_t>(reads.error()),
                {},
                reads.error()
            });
        }
        result->reads_ = std::move(*reads);
        result->root_ = source.file_.parent_path();
        result->source_ = std::move(source);
        result->source_.mounts_.clear();
        result->rebuildCatalog();
        return result;
    }

    ProjectStorage::~ProjectStorage()
    {
        requestClose();
        const auto completed = tasks_.execution().waitUntil([&]() noexcept {
            auto result = advanceClose();
            if (!result)
            {
                log::error("project.close", "{}: {}", result.error().domain, result.error().message);
                return true;
            }
            return *result;
        });
        if (!completed)
            std::terminate();
        // Also detach partial mounts when construction failed before a read endpoint existed.
        for (const auto& package : mounts_)
            vfs_.unmount(package.id);
    }

    const ProjectAssetEntry* ProjectStorage::asset(asset::AssetId id) const noexcept
    {
        const auto found = std::ranges::find(source_.manifest_.assets, id, &ProjectAssetEntry::id);
        return found == source_.manifest_.assets.end() ? nullptr : std::addressof(*found);
    }

    process::asset_loading::AssetReadPort ProjectStorage::assetReads() const noexcept
    {
        return reads_->port();
    }

    void ProjectStorage::rebuildCatalog()
    {
        std::vector<AssetCatalogEntry> entries;
        std::unordered_map<asset::AssetId, std::size_t> by_id;
        entries.reserve(source_.manifest_.assets.size());
        for (const auto& entry : source_.manifest_.assets)
        {
            by_id.emplace(entry.id, entries.size());
            entries.push_back({entry.id, entry.id, 0, entry.mount_path.empty() ? entry.source_path : entry.mount_path});
        }
        std::unordered_set<asset::AssetId> claimed;
        for (auto package = mounts_.rbegin(); package != mounts_.rend(); ++package)
        {
            const auto source =
                std::ranges::find(source_.manifest_.assets, package->path, &ProjectAssetEntry::cooked_path);
            const auto source_id = source == source_.manifest_.assets.end() ? asset::AssetId{} : source->id;
            for (const auto& entry : package->entries)
            {
                if (!claimed.insert(entry.id).second || entry.tombstone)
                {
                    continue;
                }
                auto [found, inserted] = by_id.try_emplace(entry.id, entries.size());
                if (inserted)
                {
                    entries.push_back({entry.id, source_id, entry.magic_number, entry.vpath});
                }
                else
                {
                    entries[found->second].magic = entry.magic_number;
                }
            }
        }
        const auto adopted = catalog_.replace(source_.manifest_.name, std::move(entries));
        if (!adopted)
            std::terminate(); // Identity/revision admission already occurred before publication.
    }

    const AssetCatalogEntry* ProjectStorage::catalogAsset(asset::AssetId id) const noexcept
    {
        return catalog_.find(id);
    }

    std::string_view ProjectStorage::assetName(asset::AssetId id) const noexcept
    {
        if (const auto* entry = catalogAsset(id))
        {
            return entry->path;
        }
        return id.isNull() ? "None" : "Unresolved asset";
    }

    AssetReference ProjectStorage::reference(asset::AssetId id) const noexcept
    {
        return catalog_.reference(id);
    }

    EditorResult<asset::AssetId> ProjectStorage::resolveReference(AssetReference reference, std::uint32_t magic) const
    {
        auto result = catalog_.resolve(reference, magic);
        if (!result)
        {
            return std::visit(
                [](auto error) -> EditorResult<asset::AssetId> {
                    return lux::cxx::unexpected(EditorFailure{
                        EEditorError::INVALID_ARGUMENT,
                        "project.asset-reference",
                        static_cast<std::uint64_t>(error),
                        {},
                        error
                    });
                },
                result.error()
            );
        }
        return *result;
    }

    PreparedProjectPublication::~PreparedProjectPublication()
    {
        if (owner_)
        {
            if (!owner_->dispatcherRef().isCurrent())
                std::terminate();
            // Payload cleanup may call back. The original reservation remains held until it returns.
            plan_.reset();
            owner_->publishing_ = false;
            for (const auto& waiter : owner_->publication_waiters_)
                waiter.request();
            owner_->publication_waiters_.clear();
        }
    }

    PreparedProjectPublication::PreparedProjectPublication(PreparedProjectPublication&& other) noexcept
        : plan_(std::move(other.plan_)), owner_(std::exchange(other.owner_, nullptr))
    {}

    PreparedProjectPublication& PreparedProjectPublication::operator=(PreparedProjectPublication&& other) noexcept
    {
        if (this != &other)
        {
            PreparedProjectPublication released(std::move(*this));
            plan_ = std::move(other.plan_);
            owner_ = std::exchange(other.owner_, nullptr);
        }
        return *this;
    }

    std::string_view ProjectStorage::sourceDigest(std::string_view path) const noexcept
    {
        const auto found = std::ranges::find(source_.source_digests_, path, [](const auto& pair) { return pair.first; });
        return found == source_.source_digests_.end() ? std::string_view{"missing"} : std::string_view{found->second};
    }

    EditorResult<PreparedProjectPublication> ProjectStorage::preparePublication(ProjectUpdate& update)
    {
        if (!dispatcherRef().isCurrent())
            return cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "project.publication.thread"});
        if (closing_)
            return lux::cxx::unexpected(EditorFailure{EEditorError::CLOSING, "project.publication"});
        if (!writable())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::READ_ONLY, "project.publication"});
        }
        if (publishing_)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "project.publication"});
        }
        if (catalog_.revision() == UINT64_MAX)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "project.catalog"});
        }
        auto next = source_.manifest_;
        if (update.plugins)
            next.plugins = *update.plugins;
        if (update.default_scene)
            next.default_scene = *update.default_scene;
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
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::SOURCE_FAILURE,
                "project.manifest",
                static_cast<std::uint64_t>(valid.error().code),
                valid.error().field,
                valid.error()
            });
        }
        std::vector<std::string> package_paths;
        std::unordered_set<std::string> packages;
        for (const auto& entry : next.assets)
        {
            if (entry.cooked_path.empty() || !packages.insert(entry.cooked_path).second)
            {
                continue;
            }
            const bool already_mounted =
                std::ranges::find(mounts_, entry.cooked_path, &MountedPackage::path) != mounts_.end();
            const auto written = std::ranges::find(update.files, entry.cooked_path, &ProjectFileChange::path);
            if (already_mounted && written != update.files.end())
            {
                return lux::cxx::unexpected(
                    EditorFailure{EEditorError::INVALID_ARGUMENT, "project.package.immutable", 0, entry.cooked_path}
                );
            }
            if (!already_mounted)
            {
                package_paths.push_back(entry.cooked_path);
            }
        }
        auto plan = ProjectPublicationPlan::prepare(
            root_, source_.file_.filename().generic_string(), source_.manifest_digest_,
            std::move(next), update.files, std::move(package_paths)
        );
        if (!plan)
            return cxx::unexpected(std::move(plan.error()));
        PreparedProjectPublication publication;
        publication.plan_ = std::move(*plan);
        publication.owner_ = this;
        publishing_ = true;
        update.files.clear(); // Rejected requests retain their payload; successful adoption transfers it once.
        return publication;
    }

    EditorResult<void> ProjectStorage::adoptPublication(
        PreparedProjectPublication& publication,
        ProjectPublicationReceipt& receipt
    )
    {
        if (!dispatcherRef().isCurrent())
            return cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "project.publication.thread"});
        if (publication.owner_ != this || !publishing_)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "project.publication.owner"});
        }
        const auto& plan = publication.plan();
        const bool is_wrong_plan = receipt.plan.get() != publication.plan_.get();
        const bool is_stale_baseline = source_.manifest_digest_ != plan.beforeManifestDigest();
        const bool is_wrong_manifest = receipt.manifest != plan.manifest() ||
            receipt.manifest_digest != projectContentDigest(plan.manifestBytes().view());
        const bool is_wrong_packages = receipt.packages.size() != plan.packagePaths().size();
        const bool is_invalid_receipt = is_wrong_plan || is_stale_baseline || is_wrong_manifest || is_wrong_packages;
        if (is_invalid_receipt)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "project.publication.receipt"});
        }
        std::vector<asset::MountDesc> added;
        added.reserve(receipt.packages.size());
        for (std::size_t index{}; index < receipt.packages.size(); ++index)
        {
            const auto& package = receipt.packages[index];
            if (package.path != plan.packagePaths()[index])
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "project.publication.package"}
                );
            }
            added.push_back(package.mount);
        }
        std::vector<asset::MountId> removed;
        for (const auto& package : mounts_)
        {
            if (std::ranges::find(receipt.manifest.assets, package.path, &ProjectAssetEntry::cooked_path) ==
                receipt.manifest.assets.end())
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
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::SOURCE_FAILURE,
                "project.mount",
                static_cast<std::uint64_t>(mounted.error()),
                {},
                mounted.error()
            });
        }
        std::erase_if(mounts_, [&](const auto& package) {
            return std::ranges::find(removed, package.id) != removed.end();
        });
        for (std::size_t index{}; index < receipt.packages.size(); ++index)
        {
            auto& package = receipt.packages[index];
            mounts_.push_back({std::move(package.path), (*mounted)[index], std::move(package.entries)});
        }
        source_.manifest_ = std::move(receipt.manifest);
        source_.manifest_digest_ = std::move(receipt.manifest_digest);
        for (auto& [path, digest] : receipt.file_digests)
        {
            auto found = std::ranges::find(source_.source_digests_, path, [](const auto& pair) { return pair.first; });
            if (found == source_.source_digests_.end())
            {
                source_.source_digests_.emplace_back(std::move(path), std::move(digest));
            }
            else
            {
                found->second = std::move(digest);
            }
        }
        rebuildCatalog();
        changed_assets_.insert(changed_assets_.end(), changed_assets.begin(), changed_assets.end());
        return {};
    }

    void ProjectStorage::whenPublicationAvailable(process::CompletionWork::Request waiter)
    {
        if (!publishing_)
            waiter.request();
        else
            publication_waiters_.push_back(std::move(waiter));
    }

    void ProjectStorage::dispatchEvents() noexcept
    {
        auto changed = std::exchange(changed_assets_, {});
        lux::editor::detail::reportSignalDelivery(catalog_.dispatchChanges(), "catalog.changed");
        for (const auto id : changed)
        {
            lux::editor::detail::reportSignalDelivery(emit(assetContentChanged, id), "assetContentChanged");
        }
    }

    EditorResult<process::asset_loading::AssetReadPort> ProjectStorage::captureAssetReads()
    {
        auto endpoint =
            process::asset_loading::VfsAssetReadEndpoint::create(vfs_.view().capture(), blocking_, tasks_, {256});
        if (!endpoint)
        {
            return lux::cxx::unexpected(
                EditorFailure{EEditorError::EXECUTION_FAILURE, "asset.read.capture", 0, {}, endpoint.error()}
            );
        }
        std::erase_if(read_snapshots_, [](const auto& value) { return value.expired(); });
        read_snapshots_.push_back(*endpoint);
        return (*endpoint)->port();
    }

    void ProjectStorage::requestClose() noexcept
    {
        closing_ = true;
        if (reads_)
            reads_->requestStop();
        for (const auto& weak : read_snapshots_)
        {
            if (auto endpoint = weak.lock())
            {
                endpoint->requestStop();
            }
        }
    }

    EditorResult<bool> ProjectStorage::advanceClose()
    {
        if (publishing_)
        {
            return false;
        }
        const auto check = [](process::asset_loading::VfsAssetReadEndpoint& endpoint) -> EditorResult<bool> {
            using Error = process::asset_loading::EVfsAssetReadEndpointError;
            const auto joined = endpoint.join();
            if (!joined && joined.error() == Error::BUSY)
            {
                return false;
            }
            if (!joined && joined.error() != Error::ALREADY_JOINED)
            {
                return lux::cxx::unexpected(EditorFailure{
                    EEditorError::EXECUTION_FAILURE,
                    "asset.read.join",
                    static_cast<std::uint64_t>(joined.error())
                });
            }
            return true;
        };
        auto primary = reads_ ? check(*reads_) : EditorResult<bool>{true};
        if (!primary || !*primary)
        {
            return primary;
        }
        for (const auto& weak : read_snapshots_)
        {
            if (auto endpoint = weak.lock())
            {
                auto result = check(*endpoint);
                if (!result || !*result)
                {
                    return result;
                }
            }
        }
        read_snapshots_.clear();
        for (const auto& package : mounts_)
        {
            vfs_.unmount(package.id);
        }
        mounts_.clear();
        return true;
    }
} // namespace lux::editor
