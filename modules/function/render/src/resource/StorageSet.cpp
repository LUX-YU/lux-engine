#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <utility>

namespace lux::render
{
    StorageSet::~StorageSet() noexcept
    {
        clear();
    }

    StorageId StorageSet::find(StorageTypeId type, std::string_view key) const noexcept
    {
        const auto found = instances_by_name_.find(std::tuple{type.index, type.gen, key});
        return found == instances_by_name_.end() ? StorageId{} : found->second;
    }

    RenderResult<StorageId> StorageSet::ensure(
        StorageTypeId type,
        std::string_view key,
        const ConfigValue& config,
        CppType requested_config,
        CppType requested_ops
    ) noexcept
    {
        if (key.empty())
        {
            return cxx::unexpected(RenderError{kStorageContract});
        }
        const auto* installed = registry_.factory(type);
        if (!installed)
        {
            return cxx::unexpected(RenderError{kStorageFactory});
        }
        const auto& factory = *installed;
        const bool is_wrong_config = requested_ops.size && requested_config != factory.config_type;
        const bool is_wrong_ops = requested_ops.size && requested_ops != factory.ops_type;
        if (is_wrong_config || is_wrong_ops)
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        if (config)
        {
            const auto actual = config.typeInfo();
            if (actual != factory.config_type)
            {
                return cxx::unexpected(RenderError{kStorageMismatch});
            }
        }
        const auto& selected_config = config ? config : *registry_.configuration(type);
        if (factory.config_type.size && !selected_config)
        {
            return cxx::unexpected(RenderError{kStorageContract});
        }
        if (const auto id = find(type, key); !id.isNull())
        {
            if (factory.compatible_config && !factory.compatible_config(registry_.native(id), selected_config.data()))
            {
                return cxx::unexpected(RenderError{kStorageMismatch});
            }
            return id;
        }
        const auto id = registry_.reserve(*this, type);
        auto native = registry_.create(id, selected_config.data());
        if (!native || !*native)
        {
            const auto error = native ? RenderError{kStorageFactory} : native.error();
            registry_.revoke(id);
            return cxx::unexpected(error);
        }
        // Native creation may grow either Set. Publish only after insertion, using the current position.
        const auto name = instances_by_name_.emplace(NameKey{type.index, type.gen, std::string(key)}, id).first;
        records_.push_back(Record{id, name, std::move(*native)});
        registry_.publish(id, records_.size() - 1, records_.back().native.get());
        return id;
    }

    RenderResult<StorageId> StorageSet::getOrCreateImpl(
        const StorageDescriptor& description,
        std::string_view key,
        const ConfigValue& config,
        CppType config_type,
        CppType ops_type
    ) noexcept
    {
        const auto type = registry_.typeId(description.type);
        const auto* factory = registry_.factory(type);
        if (!factory)
        {
            return cxx::unexpected(RenderError{kStorageFactory});
        }
        if (auto compatible = compatibleStorage(*factory->descriptor, description); !compatible)
        {
            return cxx::unexpected(compatible.error());
        }
        return ensure(type, key, config, config_type, ops_type);
    }

    RenderResult<StorageId> StorageSet::getOrCreate(
        StorageTypeId type,
        std::string_view key,
        const ConfigValue& config
    ) noexcept
    {
        return ensure(type, key, config);
    }

    RenderResult<void> StorageSet::remove(StorageId id) noexcept
    {
        const auto position = registry_.position(id, *this);
        if (!position)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        erase(*position);
        return {};
    }

    void StorageSet::erase(std::size_t position) noexcept
    {
        const auto& record = records_[position];
        instances_by_name_.erase(record.name);
        registry_.revoke(record.id);
        auto native = std::move(records_[position].native);
        if (position + 1 != records_.size())
        {
            records_[position] = std::move(records_.back());
            registry_.relocate(records_[position].id, position);
        }
        records_.pop_back();
        // All indexes are repaired before a Native destructor can release another actual instance.
    }

    void StorageSet::clear() noexcept
    {
        for (const auto& record : records_)
        {
            registry_.revoke(record.id);
        }
        instances_by_name_.clear();
        // Revoke the entire set before any destructor runs. No temporary container or allocation is needed.
        while (!records_.empty())
        {
            auto native = std::move(records_.back().native);
            records_.pop_back();
        }
    }
} // namespace lux::render
