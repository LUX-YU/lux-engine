#include <lux/engine/resource/asset/storage/AssetVfs.hpp>
#include <lux/engine/resource/asset/storage/VirtualPath.hpp>

#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
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
[[nodiscard]] std::shared_ptr<const detail::MountTable> snapshot(
    const std::shared_ptr<detail::AssetVfsState> &state) noexcept
{
    return state ? state->published.load(std::memory_order_acquire) : nullptr;
}
} // namespace

AssetVfsView::AssetVfsView(std::shared_ptr<detail::AssetVfsState> state) noexcept : state_(std::move(state))
{
}

AssetVfsView AssetVfsView::capture() const
{
    if (!state_)
    {
        return {};
    }
    auto frozen = std::make_shared<detail::AssetVfsState>();
    frozen->published.store(snapshot(state_), std::memory_order_release);
    return AssetVfsView(std::move(frozen));
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
    {
        return {};
    }

    const auto relative = parsed->relPath();
    for (const auto &mount : table->mounts)
    {
        if (std::string_view{mount.root}.substr(1U) != parsed->root())
        {
            continue;
        }
        if (const auto id = mount.provider->resolve(relative))
        {
            return *id;
        }
    }
    return {};
}

lux::cxx::expected<AssetBlob, EAssetStorageError> AssetVfsView::open(AssetId id) const
{
    const auto table = snapshot(state_);
    if (id.isNull() || !table)
    {
        return lux::cxx::unexpected(EAssetStorageError::NOT_FOUND);
    }

    for (const auto &mount : table->mounts)
    {
        if (mount.provider->contains(id))
        {
            return mount.provider->open(id);
        }
    }
    return lux::cxx::unexpected(EAssetStorageError::NOT_FOUND);
}

void AssetVfsView::enumerate(const std::function<void(const ProviderEntry &)> &fn) const
{
    const auto table = snapshot(state_);
    if (!table)
    {
        return;
    }

    std::unordered_set<AssetId> claimed_ids;
    std::unordered_set<std::string> claimed_paths;
    for (const auto &mount : table->mounts)
    {
        mount.provider->enumerate([&](const ProviderEntry &entry) {
            if (!claimed_ids.insert(entry.id).second || entry.tombstone)
            {
                return;
            }
            auto absolute = entry;
            absolute.vpath = mount.root + "/" + entry.vpath;
            if (claimed_paths.insert(absolute.vpath).second)
            {
                fn(absolute);
            }
        });
    }
}

std::optional<std::string> AssetVfsView::pathOf(AssetId id) const
{
    const auto table = snapshot(state_);
    if (id.isNull() || !table)
    {
        return std::nullopt;
    }

    for (const auto &mount : table->mounts)
    {
        if (!mount.provider->contains(id))
        {
            continue;
        }
        if (auto relative = mount.provider->pathOf(id))
        {
            return mount.root + "/" + *relative;
        }
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
    const auto mounted = replaceMounts({}, std::span(&desc, 1));
    return mounted ? mounted->front() : kInvalidMountId;
}

lux::cxx::expected<std::vector<MountId>, EMountUpdateError> AssetVfs::replaceMounts(std::span<const MountId> removed,
                                                                                    std::span<const MountDesc> added)
{
    for (const auto &descriptor : added)
    {
        if (!descriptor.provider || !VirtualPath::isLegalRoot(descriptor.root))
        {
            return lux::cxx::unexpected(EMountUpdateError::INVALID_DESCRIPTOR);
        }
    }

    std::lock_guard lock{state_->control_mutex};
    const auto current = state_->published.load(std::memory_order_acquire);
    std::unordered_set<MountId> retiring;
    retiring.reserve(removed.size());
    for (const auto id : removed)
    {
        if (!retiring.insert(id).second)
        {
            return lux::cxx::unexpected(EMountUpdateError::DUPLICATE_MOUNT);
        }
        if (std::ranges::find(current->mounts, id, &detail::Mount::id) == current->mounts.end())
        {
            return lux::cxx::unexpected(EMountUpdateError::UNKNOWN_MOUNT);
        }
    }
    const bool ids_exhausted = added.size() > std::numeric_limits<MountId>::max() - state_->next_id;
    const bool sequences_exhausted = added.size() > UINT64_MAX - state_->next_sequence;
    if (ids_exhausted || sequences_exhausted)
    {
        return lux::cxx::unexpected(EMountUpdateError::CAPACITY);
    }
    if (removed.empty() && added.empty())
    {
        return std::vector<MountId>{};
    }

    auto next = std::make_shared<detail::MountTable>();
    next->mounts.reserve(current->mounts.size() - removed.size() + added.size());
    for (const auto &mount : current->mounts)
    {
        if (!retiring.contains(mount.id))
        {
            next->mounts.push_back(mount);
        }
    }
    std::vector<MountId> result;
    result.reserve(added.size());
    auto next_id = state_->next_id;
    auto sequence = state_->next_sequence;
    for (const auto &descriptor : added)
    {
        result.push_back(next_id);
        next->mounts.push_back({next_id++, descriptor.root, descriptor.provider, descriptor.priority, sequence++});
    }
    std::ranges::sort(next->mounts, [](const detail::Mount &left, const detail::Mount &right) {
        return left.priority > right.priority || (left.priority == right.priority && left.sequence > right.sequence);
    });

    state_->next_id = next_id;
    state_->next_sequence = sequence;
    state_->published.store(std::move(next), std::memory_order_release);
    return result;
}

void AssetVfs::unmount(MountId id)
{
    if (id == kInvalidMountId)
    {
        return;
    }

    std::lock_guard lock{state_->control_mutex};
    const auto current = state_->published.load(std::memory_order_acquire);
    const auto found =
        std::ranges::find_if(current->mounts, [id](const detail::Mount &mount) noexcept { return mount.id == id; });
    if (found == current->mounts.end())
    {
        return;
    }

    auto next = std::make_shared<detail::MountTable>(*current);
    std::erase_if(next->mounts, [id](const detail::Mount &mount) noexcept { return mount.id == id; });
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
