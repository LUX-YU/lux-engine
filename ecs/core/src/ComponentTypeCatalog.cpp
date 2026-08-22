#include <lux/engine/ecs/ComponentTypeCatalog.hpp>

#include <lux/cxx/algorithm/hash.hpp>
#include <lux/engine/meta/Meta.hpp>

#include <algorithm>
#include <exception>
#include <type_traits>
#include <utility>

namespace lux::ecs
{
    std::string defaultComponentSchemaName(std::string_view cpp_name)
    {
        std::string result;
        result.reserve(cpp_name.size());
        for (std::size_t index = 0u; index < cpp_name.size(); ++index)
        {
            const char value = cpp_name[index];
            if (value == ':' && index + 1u < cpp_name.size() &&
                cpp_name[index + 1u] == ':')
            {
                result.push_back('.');
                ++index;
                continue;
            }
            result.push_back(
                value >= 'A' && value <= 'Z'
                    ? static_cast<char>(value - 'A' + 'a')
                    : value);
        }
        return result;
    }

    namespace
    {
        thread_local std::vector<ComponentSchemaDescriptor>*
            generated_component_draft{};

        [[nodiscard]] ComponentCatalogFailure failure(
            EComponentCatalogError error,
            std::string_view name,
            std::string_view conflicting = {})
        {
            return ComponentCatalogFailure{
                error,
                std::string{name},
                std::string{conflicting}};
        }
    }

    GeneratedComponentDraftCapture::GeneratedComponentDraftCapture(
        std::vector<ComponentSchemaDescriptor>& output) noexcept
    {
        if (generated_component_draft != nullptr)
            std::terminate();
        generated_component_draft = &output;
        active_ = true;
    }

    GeneratedComponentDraftCapture::~GeneratedComponentDraftCapture()
    {
        if (!active_ || generated_component_draft == nullptr)
            std::terminate();
        generated_component_draft = nullptr;
    }

    void GeneratedComponentDraftCapture::append(
        ComponentSchemaDescriptor descriptor) noexcept
    {
        if (generated_component_draft != nullptr)
            generated_component_draft->push_back(std::move(descriptor));
    }

    lux::cxx::expected<void, ComponentCatalogFailure>
    ComponentTypeCatalog::validate(
        const ComponentSchemaDescriptor& descriptor,
        std::span<const ComponentSchemaDescriptor> pending) const
    {
        if (!descriptor.cpp_type.isValid() ||
            !isValidComponentSchemaId(descriptor.schema_id) ||
            descriptor.schema_version == 0u ||
            !descriptor.operations.has || !descriptor.operations.get ||
            !descriptor.operations.emplace || !descriptor.operations.remove ||
            !descriptor.operations.notify || !descriptor.operations.reserve ||
            !descriptor.operations.transfer ||
            (descriptor.serialization !=
                    EComponentSerializationPolicy::TRANSIENT &&
                !descriptor.operations.no_throw_transfer) ||
            descriptor.provider.empty())
        {
            return lux::cxx::unexpected(failure(
                EComponentCatalogError::INVALID_DESCRIPTOR,
                descriptor.schema_id.name));
        }

        const auto check = [&](const ComponentSchemaDescriptor& existing)
            -> lux::cxx::expected<void, ComponentCatalogFailure>
        {
            if (existing.cpp_type.hash() == descriptor.cpp_type.hash())
            {
                if (existing.cpp_type.name() != descriptor.cpp_type.name())
                    return lux::cxx::unexpected(failure(
                        EComponentCatalogError::CPP_TYPE_HASH_COLLISION,
                        descriptor.cpp_type.name(),
                        existing.cpp_type.name()));
                return lux::cxx::unexpected(failure(
                    EComponentCatalogError::DUPLICATE_CPP_TYPE,
                    descriptor.cpp_type.name(),
                    existing.cpp_type.name()));
            }
            if (existing.schema_id.hash == descriptor.schema_id.hash)
            {
                if (existing.schema_id.name != descriptor.schema_id.name)
                    return lux::cxx::unexpected(failure(
                        EComponentCatalogError::SCHEMA_HASH_COLLISION,
                        descriptor.schema_id.name,
                        existing.schema_id.name));
                return lux::cxx::unexpected(failure(
                    EComponentCatalogError::DUPLICATE_SCHEMA_NAME,
                    descriptor.schema_id.name,
                    existing.schema_id.name));
            }
            if (existing.schema_id.name == descriptor.schema_id.name)
                return lux::cxx::unexpected(failure(
                    EComponentCatalogError::DUPLICATE_SCHEMA_NAME,
                    descriptor.schema_id.name,
                    existing.schema_id.name));
            return {};
        };

        for (const auto& existing : entries_)
            if (auto result = check(existing); !result)
                return result;
        for (const auto& existing : pending)
            if (auto result = check(existing); !result)
                return result;
        return {};
    }

    ComponentTypeCatalog::PreparedRegistration::PreparedRegistration(
        ComponentTypeCatalog& target,
        std::vector<ComponentSchemaDescriptor> entries,
        std::unordered_map<std::uint64_t, std::size_t> schema_index,
        std::unordered_map<std::uint64_t, std::size_t> type_index,
        std::size_t added_count) noexcept
        : target_(&target),
          entries_(std::move(entries)),
          schema_index_(std::move(schema_index)),
          type_index_(std::move(type_index)),
          added_count_(added_count)
    {}

    std::size_t
    ComponentTypeCatalog::PreparedRegistration::commit() noexcept
    {
        if (target_ == nullptr)
            std::terminate();
        const auto added = added_count_;
        if (added != 0u)
        {
            target_->entries_.swap(entries_);
            target_->schema_index_.swap(schema_index_);
            target_->type_index_.swap(type_index_);
        }
        target_ = nullptr;
        added_count_ = 0u;
        return added;
    }

    ComponentTypeCatalog::RegisterSchemaResult
    ComponentTypeCatalog::registerSchema(ComponentSchemaDescriptor descriptor)
    {
        const auto registered = registerSchemas(
            std::span<const ComponentSchemaDescriptor>{&descriptor, 1u});
        if (!registered)
            return lux::cxx::unexpected(std::move(registered.error()));
        return &entries_.back();
    }

    ComponentTypeCatalog::PrepareSchemasResult
    ComponentTypeCatalog::prepareSchemas(
        std::span<const ComponentSchemaDescriptor> descriptors)
    {
        std::vector<ComponentSchemaDescriptor> prepared;
        prepared.reserve(descriptors.size());
        for (const auto& source : descriptors)
        {
            auto descriptor = source;
            if (descriptor.schema_id.hash == 0u &&
                !descriptor.schema_id.name.empty())
            {
                descriptor.schema_id.hash =
                    lux::cxx::algorithm::fnv1a(descriptor.schema_id.name);
            }
            if (auto checked = validate(descriptor, prepared); !checked)
                return lux::cxx::unexpected(std::move(checked.error()));
            prepared.push_back(std::move(descriptor));
        }

        if (prepared.empty())
        {
            return PreparedRegistration{
                *this, {}, {}, {}, 0u};
        }

        if (prepared.size() > entries_.max_size() - entries_.size())
        {
            return lux::cxx::unexpected(failure(
                EComponentCatalogError::INVALID_DESCRIPTOR,
                prepared.front().schema_id.name));
        }

        const auto first_index = entries_.size();
        const auto final_size = first_index + prepared.size();

        // Build both indexes away from the live catalogue. In particular,
        // unordered_map allocates a node for every emplace even after reserve;
        // doing those allocations after entries_.push_back() can leave a
        // descriptor visible through all() but absent from one lookup index.
        // Any allocation failure while constructing these candidates leaves
        // the live entries and indexes untouched.
        auto next_entries = entries_;
        next_entries.reserve(final_size);
        for (auto& descriptor : prepared)
            next_entries.push_back(std::move(descriptor));

        auto next_schema_index = schema_index_;
        auto next_type_index = type_index_;
        next_schema_index.reserve(final_size);
        next_type_index.reserve(final_size);

        for (std::size_t offset = 0u; offset < prepared.size(); ++offset)
        {
            const auto index = first_index + offset;
            const auto& descriptor = next_entries[index];
            const auto [schema_position, schema_inserted] =
                next_schema_index.emplace(descriptor.schema_id.hash, index);
            if (!schema_inserted)
            {
                const auto existing_index = schema_position->second;
                const auto& existing = existing_index < first_index
                    ? entries_[existing_index]
                    : next_entries[existing_index];
                const auto error =
                    existing.schema_id.name == descriptor.schema_id.name
                    ? EComponentCatalogError::DUPLICATE_SCHEMA_NAME
                    : EComponentCatalogError::SCHEMA_HASH_COLLISION;
                return lux::cxx::unexpected(failure(
                    error,
                    descriptor.schema_id.name,
                    existing.schema_id.name));
            }

            const auto [type_position, type_inserted] =
                next_type_index.emplace(descriptor.cpp_type.hash(), index);
            if (!type_inserted)
            {
                const auto existing_index = type_position->second;
                const auto& existing = existing_index < first_index
                    ? entries_[existing_index]
                    : next_entries[existing_index];
                const auto error =
                    existing.cpp_type.name() == descriptor.cpp_type.name()
                    ? EComponentCatalogError::DUPLICATE_CPP_TYPE
                    : EComponentCatalogError::CPP_TYPE_HASH_COLLISION;
                return lux::cxx::unexpected(failure(
                    error,
                    descriptor.cpp_type.name(),
                    existing.cpp_type.name()));
            }
        }

        return PreparedRegistration{
            *this,
            std::move(next_entries),
            std::move(next_schema_index),
            std::move(next_type_index),
            descriptors.size()};
    }

    ComponentTypeCatalog::RegisterSchemasResult
    ComponentTypeCatalog::registerSchemas(
        std::span<const ComponentSchemaDescriptor> descriptors)
    {
        auto prepared = prepareSchemas(descriptors);
        if (!prepared)
            return lux::cxx::unexpected(std::move(prepared.error()));
        return prepared->commit();
    }

    ComponentTypeCatalog::ValidationResult
    ComponentTypeCatalog::validateSchemas(std::span<const ComponentSchemaDescriptor> descriptors) const
    {
        std::vector<ComponentSchemaDescriptor> prepared;
        prepared.reserve(descriptors.size());
        for (const auto& source : descriptors)
        {
            auto descriptor = source;
            if (descriptor.schema_id.hash == 0u &&
                !descriptor.schema_id.name.empty())
            {
                descriptor.schema_id.hash =
                    lux::cxx::algorithm::fnv1a(descriptor.schema_id.name);
            }
            if (auto checked = validate(descriptor, prepared); !checked)
                return checked;
            prepared.push_back(std::move(descriptor));
        }
        return {};
    }

    const ComponentSchemaDescriptor* ComponentTypeCatalog::findBySchema(
        std::string_view name) const noexcept
    {
        const auto hash = lux::cxx::algorithm::fnv1a(name);
        const auto found = schema_index_.find(hash);
        if (found == schema_index_.end())
            return nullptr;
        const auto& descriptor = entries_[found->second];
        return descriptor.schema_id.name == name ? &descriptor : nullptr;
    }

    const ComponentSchemaDescriptor* ComponentTypeCatalog::findByCppName(
        std::string_view name) const noexcept
    {
        const auto found = std::find_if(
            entries_.begin(),
            entries_.end(),
            [name](const ComponentSchemaDescriptor& descriptor)
            {
                return descriptor.cpp_type.name() == name;
            });
        return found == entries_.end() ? nullptr : &*found;
    }

    const ComponentSchemaDescriptor* ComponentTypeCatalog::findByType(
        lux::cxx::TypeToken type) const noexcept
    {
        const auto found = type_index_.find(type.hash());
        if (found == type_index_.end())
            return nullptr;
        const auto& descriptor = entries_[found->second];
        return descriptor.cpp_type == type ? &descriptor : nullptr;
    }

    std::span<const ComponentSchemaDescriptor> ComponentTypeCatalog::all() const noexcept
    {
        return entries_;
    }

    ComponentCatalogExp<std::size_t> initializeGeneratedMetadata(
        ComponentTypeCatalog& catalog)
    {
        if (lux::meta::ReflectionRegistry::initialized())
        {
            return lux::cxx::unexpected(failure(
                EComponentCatalogError::INVALID_DESCRIPTOR,
                "generated metadata already drained"));
        }
        std::vector<ComponentSchemaDescriptor> descriptors;
        {
            GeneratedComponentDraftCapture capture{descriptors};
            lux::meta::meta_module_init();
        }
        return catalog.registerSchemas(descriptors);
    }

    ComponentCatalogValidationResult validateComponentSchemas(
        const ComponentTypeCatalog& catalog,
        std::span<const std::string_view> schema_names
    )
    {
        for (const auto name : schema_names)
            if (!catalog.findBySchema(name))
                return lux::cxx::unexpected(failure(
                    EComponentCatalogError::MISSING_SCHEMA,
                    name));
        return {};
    }
}
