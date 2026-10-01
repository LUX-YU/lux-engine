#include <lux/engine/editor/project/ProjectCatalogModel.hpp>
#include <algorithm>
#include <limits>
#include <unordered_map>

namespace lux::editor::project
{
    struct ProjectCatalogSnapshot::Data final
    {
        ProjectCatalogVersion version;
        std::string name;
        std::vector<AssetCatalogEntry> assets;
        std::unordered_map<asset::AssetId, std::size_t> by_id;
    };
    ProjectCatalogSnapshot::ProjectCatalogSnapshot(std::shared_ptr<const Data> data) noexcept
        : version(data->version), name(data->name), assets(data->assets), owner_(std::move(data))
    {}
    ProjectCatalogModel::ProjectCatalogModel(object::ObjectDispatcherRef dispatcher, std::uint64_t instance)
        : LuxObject(std::move(dispatcher)),
          snapshot_(
              std::make_shared<ProjectCatalogSnapshot::Data>(ProjectCatalogSnapshot::Data{{instance, 0}, {}, {}, {}})
          )
    {}
    ProjectCatalogModel::~ProjectCatalogModel() = default;
    ProjectQueryResult<ProjectCatalogVersion> ProjectCatalogModel::version() const noexcept
    {
        if (failure_)
            return lux::cxx::unexpected(VProjectQueryFailure{*failure_});
        return snapshot_->version;
    }
    ProjectQueryResult<ProjectCatalogSnapshot> ProjectCatalogModel::snapshot() const noexcept
    {
        if (failure_)
            return lux::cxx::unexpected(VProjectQueryFailure{*failure_});
        return ProjectCatalogSnapshot{snapshot_};
    }
    const AssetCatalogEntry* ProjectCatalogModel::find(asset::AssetId id) const noexcept
    {
        const auto found = snapshot_->by_id.find(id);
        return found == snapshot_->by_id.end() ? nullptr : &snapshot_->assets[found->second];
    }
    AssetReference ProjectCatalogModel::reference(asset::AssetId id) const noexcept
    {
        return {snapshot_->version.instance, snapshot_->version.revision, id};
    }
    std::span<const AssetCatalogEntry> ProjectCatalogModel::entries() const noexcept
    {
        return snapshot_->assets;
    }
    std::uint64_t ProjectCatalogModel::revision() const noexcept
    {
        return snapshot_->version.revision;
    }
    ProjectQueryResult<asset::AssetId> ProjectCatalogModel::resolve(AssetReference reference, std::uint32_t magic)
        const noexcept
    {
        if (failure_)
            return lux::cxx::unexpected(VProjectQueryFailure{*failure_});
        if (reference.project_instance != snapshot_->version.instance)
            return lux::cxx::unexpected(VProjectQueryFailure{EAssetReferenceError::FOREIGN_PROJECT});
        if (reference.catalog_revision != snapshot_->version.revision)
            return lux::cxx::unexpected(VProjectQueryFailure{EAssetReferenceError::STALE_CATALOG});
        const auto* entry = find(reference.asset);
        if (!entry)
            return lux::cxx::unexpected(VProjectQueryFailure{EAssetReferenceError::MISSING_ASSET});
        if (magic && entry->magic != magic)
            return lux::cxx::unexpected(VProjectQueryFailure{EAssetReferenceError::WRONG_TYPE});
        return reference.asset;
    }
    ProjectQueryResult<void> ProjectCatalogModel::replace(std::string name, std::vector<AssetCatalogEntry> entries)
    {
        if (revision() == std::numeric_limits<std::uint64_t>::max())
            return lux::cxx::unexpected(VProjectQueryFailure{EProjectQueryError::CAPACITY});
        auto candidate = std::make_shared<ProjectCatalogSnapshot::Data>();
        candidate->version = {snapshot_->version.instance, revision() + 1};
        candidate->name = std::move(name);
        candidate->assets = std::move(entries);
        std::ranges::sort(candidate->assets, {}, &AssetCatalogEntry::path);
        candidate->by_id.reserve(candidate->assets.size());
        for (std::size_t i{}; i < candidate->assets.size(); ++i)
        {
            if (!candidate->by_id.emplace(candidate->assets[i].id, i).second)
                return lux::cxx::unexpected(VProjectQueryFailure{EProjectQueryError::INVALID_PAYLOAD});
        }
        snapshot_ = std::move(candidate);
        failure_.reset();
        notification_pending_ = true;
        return {};
    }
    void ProjectCatalogModel::setFailure(std::optional<EProjectQueryError> failure) noexcept
    {
        notification_pending_ |= failure_ != failure;
        failure_ = failure;
    }
    object::SignalDelivery ProjectCatalogModel::dispatchChanges() noexcept
    {
        if (!std::exchange(notification_pending_, false))
            return {};
        return emit(changed, revision());
    }
}
