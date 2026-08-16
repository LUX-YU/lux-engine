#include <lux/engine/resource/asset/AssetVfs.hpp>
#include <lux/engine/resource/asset/VirtualPath.hpp>

#include <algorithm>
#include <unordered_set>

namespace lux::asset
{
    MountId AssetVfs::mount(MountDesc desc)
    {
        if (!desc.provider || !VirtualPath::isLegalRoot(desc.root))
            return kInvalidMountId;

        Mount m{
            next_id_++,
            std::move(desc.root),
            std::move(desc.provider),
            desc.priority,
            next_seq_++
        };

        // Insert keeping (priority desc, seq desc): the first position whose
        // mount loses to the new one. Equal priority -> the new (higher seq)
        // mount lands BEFORE existing ones = head-insert, newest wins.
        const auto pos = std::find_if(
            mounts_.begin(), mounts_.end(),
            [&](const Mount& other)
            {
                return other.priority < m.priority
                    || (other.priority == m.priority && other.seq < m.seq);
            }
        );
        const MountId id = m.id;
        mounts_.insert(pos, std::move(m));
        return id;
    }

    void AssetVfs::unmount(MountId id)
    {
        std::erase_if(mounts_, [id](const Mount& m) { return m.id == id; });
    }

    asset_id_t AssetVfs::resolve(std::string_view vpath) const
    {
        const auto parsed = VirtualPath::parse(vpath);
        if (!parsed.has_value())
            return asset_id_t{};

        const std::string_view rel = parsed.value().relPath();
        for (const Mount& m : mounts_)
        {
            // m.root is "/Game"; parsed root() is "Game".
            if (std::string_view{ m.root }.substr(1) != parsed.value().root())
                continue;
            if (const auto id = m.provider->resolve(rel))
                return *id;
        }
        return asset_id_t{};
    }

    lux::cxx::expected<AssetBlob, EAssetError>
    AssetVfs::open(const asset_id_t& id) const
    {
        if (id.is_nil())
            return lux::cxx::unexpected(EAssetError::ASSET_NOT_EXIST);

        for (const Mount& m : mounts_)
        {
            if (m.provider->contains(id))
                return m.provider->open(id); // tombstone claims + fails here
        }
        return lux::cxx::unexpected(EAssetError::ASSET_NOT_EXIST);
    }

    void AssetVfs::enumerate(
        const std::function<void(const ProviderEntry&)>& fn) const
    {
        std::unordered_set<asset_id_t>  claimed_ids;
        std::unordered_set<std::string> claimed_paths;

        for (const Mount& m : mounts_)
        {
            m.provider->enumerate(
                [&](const ProviderEntry& e)
                {
                    if (!claimed_ids.insert(e.id).second)
                        return;             // id already won by a higher mount
                    if (e.tombstone)
                        return;             // claims the id, emits nothing

                    ProviderEntry abs = e;
                    abs.vpath = m.root + "/" + e.vpath;
                    if (!claimed_paths.insert(abs.vpath).second)
                        return;             // path shadowed by a higher mount
                    fn(abs);
                }
            );
        }
    }

    std::optional<std::string> AssetVfs::pathOf(const asset_id_t& id) const
    {
        if (id.is_nil())
            return std::nullopt;

        for (const Mount& m : mounts_)
        {
            if (!m.provider->contains(id))
                continue;
            // Winner found; a tombstone has no path — report absent.
            if (auto rel = m.provider->pathOf(id))
                return m.root + "/" + *rel;
            return std::nullopt;
        }
        return std::nullopt;
    }

    // Resolve the EntityScene selected by an optional canonical vpath.
    lux::cxx::expected<BootSceneResolve, EBootSceneError>
    resolveBootScene(const IAssetProvider& provider, std::string_view vpath)
    {
        if (!vpath.empty())
        {
            if (auto rid = provider.resolve(vpath); rid && !rid->is_nil())
            {
                bool is_scene = false;
                provider.enumerate([&](const ProviderEntry& entry)
                {
                    if (!entry.tombstone && entry.id == *rid &&
                        entry.type == EAssetType::ENTITY_SCENE)
                    {
                        is_scene = true;
                    }
                });
                if (is_scene)
                    return BootSceneResolve{*rid, std::string(vpath), 1};
                return lux::cxx::unexpected<EBootSceneError>(
                    EBootSceneError::EntryTypeMismatch);
            }
            return lux::cxx::unexpected<EBootSceneError>(
                EBootSceneError::VPathNotFound);
        }

        BootSceneResolve out{};
        provider.enumerate([&](const ProviderEntry& e)
        {
            if (e.tombstone || e.type != EAssetType::ENTITY_SCENE)
                return;
            ++out.scene_count;
            if (out.scene_count == 1)
            {
                out.id    = e.id;
                out.vpath = e.vpath;
            }
        });
        if (out.scene_count == 0)
            return lux::cxx::unexpected<EBootSceneError>(
                EBootSceneError::NoSceneEntry);
        if (out.scene_count > 1)
            return lux::cxx::unexpected<EBootSceneError>(
                EBootSceneError::MultipleScenes);
        return out;
    }
}
