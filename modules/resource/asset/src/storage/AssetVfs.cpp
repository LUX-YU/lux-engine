#include <lux/engine/resource/asset/storage/AssetVfs.hpp>
#include <lux/engine/resource/asset/storage/VirtualPath.hpp>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <new>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lux::asset::detail
{
    struct Mount final
    {
        MountId id{};
        std::string root;
        std::shared_ptr<IAssetProvider> provider;
        int priority{};
        std::uint64_t sequence{};
    };

    struct MountTable final
    {
        std::vector<Mount> mounts;
    };

    struct AssetVfsState final
    {
        AssetVfsState() : published(std::make_shared<const MountTable>())
        {
        }

        std::mutex control_mutex;
        std::atomic<std::shared_ptr<const MountTable>> published;
        MountId next_id{1U};
        std::uint64_t next_sequence{1U};
    };
} // namespace lux::asset::detail

namespace lux::asset
{
    namespace
    {
        [[nodiscard]] std::shared_ptr<const detail::MountTable>
        snapshot(const std::shared_ptr<detail::AssetVfsState>& state) noexcept
        {
            return state ? state->published.load(std::memory_order_acquire) : nullptr;
        }
    } // namespace

    AssetVfsView::AssetVfsView(std::shared_ptr<detail::AssetVfsState> state) noexcept
        : state_(std::move(state))
    {
    }

    AssetVfsView::operator bool() const noexcept
    {
        return static_cast<bool>(state_);
    }

    AssetId AssetVfsView::resolve(std::string_view vpath) const
    {
        const auto parsed = VirtualPath::parse(vpath);
        const auto table = snapshot(state_);
        if (!parsed || !table)
            return {};

        const auto relative = parsed->relPath();
        for (const auto& mount : table->mounts)
        {
            if (std::string_view{mount.root}.substr(1U) != parsed->root())
                continue;
            if (const auto id = mount.provider->resolve(relative))
                return *id;
        }
        return {};
    }

    lux::cxx::expected<AssetBlob, EAssetStorageError> AssetVfsView::open(AssetId id) const
    {
        const auto table = snapshot(state_);
        if (id.isNull() || !table)
            return lux::cxx::unexpected(EAssetStorageError::NOT_FOUND);

        for (const auto& mount : table->mounts)
            if (mount.provider->contains(id))
                return mount.provider->open(id);
        return lux::cxx::unexpected(EAssetStorageError::NOT_FOUND);
    }

    void AssetVfsView::enumerate(const std::function<void(const ProviderEntry&)>& fn) const
    {
        const auto table = snapshot(state_);
        if (!table)
            return;

        std::unordered_set<AssetId> claimed_ids;
        std::unordered_set<std::string> claimed_paths;
        for (const auto& mount : table->mounts)
        {
            mount.provider->enumerate([&](const ProviderEntry& entry) {
                if (!claimed_ids.insert(entry.id).second || entry.tombstone)
                    return;
                auto absolute = entry;
                absolute.vpath = mount.root + "/" + entry.vpath;
                if (claimed_paths.insert(absolute.vpath).second)
                    fn(absolute);
            });
        }
    }

    std::optional<std::string> AssetVfsView::pathOf(AssetId id) const
    {
        const auto table = snapshot(state_);
        if (id.isNull() || !table)
            return std::nullopt;

        for (const auto& mount : table->mounts)
        {
            if (!mount.provider->contains(id))
                continue;
            if (auto relative = mount.provider->pathOf(id))
                return mount.root + "/" + *relative;
            return std::nullopt;
        }
        return std::nullopt;
    }

    AssetVfs::AssetVfs() : state_(std::make_shared<detail::AssetVfsState>())
    {
    }

    AssetVfs::~AssetVfs() = default;

    MountId AssetVfs::mount(MountDesc desc)
    {
        if (!desc.provider || !VirtualPath::isLegalRoot(desc.root))
            return kInvalidMountId;

        std::lock_guard lock{state_->control_mutex};
        if (state_->next_id == kInvalidMountId || state_->next_sequence == 0U)
            return kInvalidMountId;

        try
        {
            const auto current = state_->published.load(std::memory_order_acquire);
            auto next = std::make_shared<detail::MountTable>(*current);
            detail::Mount mount{
                state_->next_id,
                std::move(desc.root),
                std::move(desc.provider),
                desc.priority,
                state_->next_sequence
            };
            const auto position = std::ranges::find_if(next->mounts, [&](const detail::Mount& other) noexcept {
                return other.priority < mount.priority ||
                    (other.priority == mount.priority && other.sequence < mount.sequence);
            });
            next->mounts.insert(position, std::move(mount));
            const auto result = state_->next_id++;
            ++state_->next_sequence;
            state_->published.store(std::move(next), std::memory_order_release);
            return result;
        }
        catch (const std::bad_alloc&)
        {
            return kInvalidMountId;
        }
    }

    void AssetVfs::unmount(MountId id)
    {
        if (id == kInvalidMountId)
            return;

        std::lock_guard lock{state_->control_mutex};
        const auto current = state_->published.load(std::memory_order_acquire);
        const auto found = std::ranges::find_if(current->mounts, [id](const detail::Mount& mount) noexcept {
            return mount.id == id;
        });
        if (found == current->mounts.end())
            return;

        auto next = std::make_shared<detail::MountTable>(*current);
        std::erase_if(next->mounts, [id](const detail::Mount& mount) noexcept { return mount.id == id; });
        state_->published.store(std::move(next), std::memory_order_release);
    }

    AssetVfsView AssetVfs::view() const noexcept
    {
        return AssetVfsView{state_};
    }

    std::size_t AssetVfs::mountCount() const noexcept
    {
        const auto table = snapshot(state_);
        return table ? table->mounts.size() : 0U;
    }
} // namespace lux::asset
