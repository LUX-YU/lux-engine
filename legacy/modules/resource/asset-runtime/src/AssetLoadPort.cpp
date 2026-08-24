#include <lux/engine/resource/asset/AssetLoadPort.hpp>

#include <lux/engine/resource/asset/AssetManager.hpp>

#include <algorithm>
#include <ranges>

namespace lux::asset_runtime
{
    lux::async::SubmitResult AssetClient::request(
        const lux::asset::asset_id_t& id) const noexcept
    {
        if (!manager_ || !ensure_ || id.is_nil())
            return lux::cxx::unexpected(
                lux::async::ESubmitError::PAYLOAD_INVALID);
        const bool ready = manager_->hasData(id);
        return ensure_.tryNotify(EnsureAssetLoaded{
            id,
            manager_->contentRevision(id),
            ready,
            ready ? nullptr : manager_->vfs()});
    }

    lux::async::SubmitResult AssetClient::invalidate(
        const lux::asset::asset_id_t& id) const noexcept
    {
        if (!manager_ || !invalidate_ || id.is_nil())
            return lux::cxx::unexpected(
                lux::async::ESubmitError::PAYLOAD_INVALID);
        return invalidate_.tryNotify(InvalidateAssetLoad{
            id,
            manager_->contentRevision(id)});
    }

    LoadAsset AssetClient::loadOperation(
        const lux::asset::asset_id_t& id) const noexcept
    {
        if (!manager_ || id.is_nil())
            return LoadAsset{.id = id};
        const bool ready = manager_->hasData(id);
        return LoadAsset{
            id,
            manager_->contentRevision(id),
            ready,
            ready ? nullptr : manager_->vfs()};
    }

    LoadAssetBatch AssetClient::loadBatchOperation(
        std::span<const lux::asset::asset_id_t> ids) const
    {
        auto operations = std::make_shared<std::vector<LoadAsset>>();
        operations->reserve(ids.size());
        for (const auto& id : ids)
        {
            if (id.is_nil())
                continue;
            const bool duplicate = std::ranges::any_of(
                *operations,
                [&id](const LoadAsset& existing)
                {
                    return existing.id == id;
                });
            if (!duplicate)
                operations->push_back(loadOperation(id));
        }
        return LoadAssetBatch{std::move(operations)};
    }
}
