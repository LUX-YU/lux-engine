#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <algorithm>
#include <exception>
#include <utility>

namespace lux::render
{
    StorageLease::~StorageLease() noexcept { reset(); }

    StorageLease::StorageLease(StorageLease&& other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)), id_(std::exchange(other.id_, {})) {}

    StorageLease& StorageLease::operator=(StorageLease&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            owner_ = std::exchange(other.owner_, nullptr);
            id_ = std::exchange(other.id_, {});
        }
        return *this;
    }

    void StorageLease::reset() noexcept
    {
        auto* owner = std::exchange(owner_, nullptr);
        const auto id = std::exchange(id_, {});
        if (owner) owner->release(id);
    }

    StorageSet::~StorageSet() noexcept { clear(); }

    StorageId StorageSet::find(StorageTypeId type, std::string_view key) const noexcept
    {
        if (closing_) return {};
        for (const auto& record : records_)
            if (record.type == type && record.key == key) return record.id;
        return {};
    }

    RenderResult<StorageId> StorageSet::ensure(
        StorageTypeId type, std::string_view key, ConfigValue config,
        StorageCppType requested_config, StorageCppType requested_ops
    ) noexcept
    {
        if (closing_) return cxx::unexpected(RenderError{kStorageIdentity});
        if (key.empty()) return cxx::unexpected(RenderError{kStorageContract});
        const auto* installed = registry_.types_.find(type);
        if (!installed) return cxx::unexpected(RenderError{kStorageFactory});
        const auto& factory = installed->factory;
        const bool is_wrong_config = requested_config.size && requested_config != factory.config_type;
        const bool is_wrong_ops = requested_ops.size && requested_ops != factory.ops_type;
        if (is_wrong_config || is_wrong_ops) return cxx::unexpected(RenderError{kStorageMismatch});
        if (config)
        {
            const StorageCppType actual{config.type(), config.objectSize(), config.objectAlignment()};
            if (actual != factory.config_type) return cxx::unexpected(RenderError{kStorageMismatch});
        }
        else config = installed->configuration; // already qualified at installation
        if (!config) return cxx::unexpected(RenderError{kStorageContract});
        if (const auto id = find(type, key); !id.isNull())
        {
            const auto& entry = *registry_.entries_.find(id);
            if (!factory.compatible_config(entry.native, config.data()))
                return cxx::unexpected(RenderError{kStorageMismatch});
            return id;
        }
        const auto id = registry_.entries_.emplace(ResourceRegistry::Entry{this, 0, type});
        auto native = factory.create(registry_.native_, id, config.data());
        if (!native || !*native)
        {
            const auto error = native ? RenderError{kStorageFactory} : native.error();
            registry_.entries_.erase(id);
            return cxx::unexpected(error);
        }
        // Native creation may grow another Set. Reacquire the index entry after the callback.
        auto& entry = *registry_.entries_.find(id);
        entry.position = records_.size();
        records_.push_back(Record{id, type, std::string(key), std::move(config), std::move(*native)});
        entry.native = records_.back().native.get();
        ++registry_.creations_;
        return id;
    }

    RenderResult<StorageLease> StorageSet::acquireImpl(
        const StorageDescriptor& description, std::string_view key, ConfigValue config,
        StorageCppType config_type, StorageCppType ops_type
    ) noexcept
    {
        const auto type = registry_.typeId(description.type);
        const auto* factory = registry_.factory(type);
        if (!factory) return cxx::unexpected(RenderError{kStorageFactory});
        if (auto compatible = compatibleStorage(*factory->description, description); !compatible)
            return cxx::unexpected(compatible.error());
        auto id = ensure(type, key, std::move(config), config_type, ops_type);
        if (!id) return cxx::unexpected(id.error());
        ++records_[registry_.entries_.find(*id)->position].leases;
        return StorageLease{*this, *id};
    }

    RenderResult<StorageLease> StorageSet::acquire(
        StorageTypeId type, std::string_view key, ConfigValue config
    ) noexcept
    {
        auto id = ensure(type, key, std::move(config));
        if (!id) return cxx::unexpected(id.error());
        ++records_[registry_.entries_.find(*id)->position].leases;
        return StorageLease{*this, *id};
    }

    RenderResult<StorageId> StorageSet::prepare(
        StorageTypeId type, std::string_view key, ConfigValue config
    ) noexcept
    {
        auto id = ensure(type, key, std::move(config));
        if (!id) return cxx::unexpected(id.error());
        ++records_[registry_.entries_.find(*id)->position].preloads;
        return id;
    }

    RenderResult<void> StorageSet::releasePrepared(StorageId id) noexcept
    {
        auto* entry = registry_.entries_.find(id);
        if (!entry || entry->owner != this || !entry->native)
            return cxx::unexpected(RenderError{kStorageIdentity});
        auto& record = records_[entry->position];
        if (!record.preloads) return cxx::unexpected(RenderError{kStorageIdentity});
        if (--record.preloads == 0 && record.leases == 0) erase(entry->position);
        return {};
    }

    void StorageSet::release(StorageId id) noexcept
    {
        const auto& entry = *registry_.entries_.find(id); // one live noncopyable lease proves the entry
        auto& record = records_[entry.position];
        if (--record.leases == 0 && record.preloads == 0) erase(entry.position);
    }

    void StorageSet::erase(std::size_t position) noexcept
    {
        auto removed = std::move(records_[position]);
        registry_.entries_.erase(removed.id);
        if (position + 1 != records_.size())
        {
            records_[position] = std::move(records_.back());
            registry_.entries_.find(records_[position].id)->position = position;
        }
        records_.pop_back();
        // removed's Native deleter runs only after both containers are coherent. Config/pin stays live.
    }

    void StorageSet::clear() noexcept
    {
        closing_ = true;
        for (auto& record : records_) record.preloads = 0;
        while (!records_.empty())
        {
            const auto ready = std::find_if(records_.begin(), records_.end(), [](const auto& record) {
                return record.leases == 0;
            });
            if (ready == records_.end()) std::terminate(); // caller must finish borrowers before Owner teardown
            erase(std::size_t(ready - records_.begin()));
        }
    }
}
