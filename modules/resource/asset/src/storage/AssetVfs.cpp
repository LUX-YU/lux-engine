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
        AssetVfsState() : published(std::make_shared<const MountTable>()) {}

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
            const std::shared_ptr<detail::AssetVfsState>& state
        ) noexcept
        {
            return state ? state->published.load(std::memory_order_acquire) : nullptr;
        }

        void revoke(const std::shared_ptr<detail::AssetVfsState>& state, MountId id) noexcept
        {
            // Drop the old snapshot after unlocking: provider destructors may call back
            // into the control plane. Already accepted reads own an independent snapshot.
            std::shared_ptr<const detail::MountTable> previous;
            {
                std::lock_guard lock{state->control_mutex};
                previous = state->published.load(std::memory_order_acquire);
                auto next = std::make_shared<detail::MountTable>();
                next->mounts.reserve(previous->mounts.size() - 1U);
                for (const auto& mount : previous->mounts)
                {
                    if (mount.id != id)
                    {
                        next->mounts.push_back(mount);
                    }
                }
                state->published.store(std::move(next), std::memory_order_release);
            }
        }
    } // namespace

    MountLease::MountLease(std::shared_ptr<detail::AssetVfsState> state, MountId id) noexcept
        : state_(std::move(state)), id_(id)
    {
    }

    MountLease::~MountLease() noexcept
    {
        if (id_ != kInvalidMountId)
        {
            const auto id = std::exchange(id_, kInvalidMountId);
            const auto state = std::move(state_);
            revoke(state, id);
        }
    }

    MountLease::MountLease(MountLease&& other) noexcept
        : state_(std::move(other.state_)), id_(std::exchange(other.id_, kInvalidMountId))
    {
    }

    MountLease& MountLease::operator=(MountLease&& other) noexcept
    {
        if (this != &other)
        {
            MountLease previous{std::move(*this)};
            state_ = std::move(other.state_);
            id_ = std::exchange(other.id_, kInvalidMountId);
        }
        return *this;
    }

    AssetVfsView::AssetVfsView(std::shared_ptr<detail::AssetVfsState> state) noexcept : state_(std::move(state)) {}

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

    namespace
    {
        AssetId resolve(const std::shared_ptr<detail::AssetVfsState>& state, std::string_view vpath)
        {
            const auto parsed = VirtualPath::parse(vpath);
            const auto table = snapshot(state);
            if (!parsed || !table)
            {
                return {};
            }

            const auto relative = parsed->relPath();
            for (const auto& mount : table->mounts)
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

        lux::cxx::expected<AssetBlob, EAssetStorageError> open(
            const std::shared_ptr<detail::AssetVfsState>& state,
            AssetId id,
            std::size_t max_bytes
        )
        {
            const auto table = snapshot(state);
            if (id.isNull() || !table)
            {
                return lux::cxx::unexpected(EAssetStorageError::NOT_FOUND);
            }

            for (const auto& mount : table->mounts)
            {
                if (mount.provider->contains(id))
                {
                    return mount.provider->open(id, max_bytes);
                }
            }
            return lux::cxx::unexpected(EAssetStorageError::NOT_FOUND);
        }

        void enumerate(
            const std::shared_ptr<detail::AssetVfsState>& state,
            const std::function<void(const ProviderEntry&)>& fn
        )
        {
            const auto table = snapshot(state);
            if (!table)
            {
                return;
            }

            std::unordered_set<AssetId> claimed_ids;
            std::unordered_set<std::string> claimed_paths;
            for (const auto& mount : table->mounts)
            {
                mount.provider->enumerate(
                    [&](const ProviderEntry& entry)
                    {
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
                    }
                );
            }
        }

        std::optional<std::string> pathOf(const std::shared_ptr<detail::AssetVfsState>& state, AssetId id)
        {
            const auto table = snapshot(state);
            if (id.isNull() || !table)
            {
                return std::nullopt;
            }

            for (const auto& mount : table->mounts)
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

    } // namespace

    AssetId AssetVfsView::resolve(std::string_view path) const
    {
        return asset::resolve(state_, path);
    }

    lux::cxx::expected<AssetBlob, EAssetStorageError> AssetVfsView::open(AssetId id, std::size_t limit) const
    {
        return asset::open(state_, id, limit);
    }

    void AssetVfsView::enumerate(const std::function<void(const ProviderEntry&)>& visitor) const
    {
        asset::enumerate(state_, visitor);
    }

    std::optional<std::string> AssetVfsView::pathOf(AssetId id) const
    {
        return asset::pathOf(state_, id);
    }

    AssetId AssetVfs::resolve(std::string_view path) const
    {
        return asset::resolve(state_, path);
    }

    lux::cxx::expected<AssetBlob, EAssetStorageError> AssetVfs::open(AssetId id, std::size_t limit) const
    {
        return asset::open(state_, id, limit);
    }

    void AssetVfs::enumerate(const std::function<void(const ProviderEntry&)>& visitor) const
    {
        asset::enumerate(state_, visitor);
    }

    std::optional<std::string> AssetVfs::pathOf(AssetId id) const
    {
        return asset::pathOf(state_, id);
    }

    AssetVfs::AssetVfs() : state_(std::make_shared<detail::AssetVfsState>()) {}

    AssetVfs::~AssetVfs() = default;

    MountResult AssetVfs::mount(MountDesc desc) noexcept
    {
        auto mounted = replaceMounts({}, std::span(&desc, 1));
        if (!mounted)
        {
            return lux::cxx::unexpected(mounted.error());
        }
        return std::move(mounted->front());
    }

    MountBatchResult AssetVfs::replaceMounts(std::span<MountLease> removed, std::span<const MountDesc> added) noexcept
    {
        for (const auto& descriptor : added)
        {
            const bool is_invalid_descriptor = !descriptor.provider || !VirtualPath::isLegalRoot(descriptor.root);
            if (is_invalid_descriptor)
            {
                return lux::cxx::unexpected(EMountUpdateError::INVALID_DESCRIPTOR);
            }
        }

        std::shared_ptr<const detail::MountTable> current;
        std::lock_guard lock{state_->control_mutex};
        current = state_->published.load(std::memory_order_acquire);
        std::unordered_set<MountId> retiring;
        retiring.reserve(removed.size());
        for (const auto& lease : removed)
        {
            const bool is_unknown_mount = lease.state_ != state_ || lease.id_ == kInvalidMountId;
            if (is_unknown_mount)
            {
                return lux::cxx::unexpected(EMountUpdateError::UNKNOWN_MOUNT);
            }
            retiring.insert(lease.id_);
        }
        const bool ids_exhausted = added.size() > std::numeric_limits<MountId>::max() - state_->next_id;
        const bool sequences_exhausted = added.size() > UINT64_MAX - state_->next_sequence;
        const bool is_exhausted = ids_exhausted || sequences_exhausted;
        if (is_exhausted)
        {
            return lux::cxx::unexpected(EMountUpdateError::CAPACITY);
        }
        const bool is_noop = removed.empty() && added.empty();
        if (is_noop)
        {
            return std::vector<MountLease>{};
        }

        auto next = std::make_shared<detail::MountTable>();
        next->mounts.reserve(current->mounts.size() - removed.size() + added.size());
        for (const auto& mount : current->mounts)
        {
            if (!retiring.contains(mount.id))
            {
                next->mounts.push_back(mount);
            }
        }
        std::vector<MountLease> result;
        result.reserve(added.size());
        auto next_id = state_->next_id;
        auto sequence = state_->next_sequence;
        for (const auto& descriptor : added)
        {
            result.push_back(MountLease{state_, next_id});
            next->mounts.push_back({next_id++, descriptor.root, descriptor.provider, descriptor.priority, sequence++});
        }
        std::ranges::sort(
            next->mounts,
            [](const detail::Mount& left, const detail::Mount& right)
            {
                return left.priority > right.priority ||
                       (left.priority == right.priority && left.sequence > right.sequence);
            }
        );

        state_->next_id = next_id;
        state_->next_sequence = sequence;
        state_->published.store(std::move(next), std::memory_order_release);
        for (auto& lease : removed)
        {
            lease.id_ = kInvalidMountId;
            lease.state_.reset();
        }
        return result;
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
