#include <lux/engine/editor/configuration/Settings.hpp>
#include <algorithm>
#include <array>

namespace lux::editor::settings
{
    namespace
    {
        auto failure(ESettingsError code, std::string detail)
        {
            return cxx::unexpected(SettingsFailure{code, std::move(detail)});
        }
        SettingsResult<std::vector<std::byte>> encoded(const ConfigurationValue& value)
        {
            std::vector<std::byte> bytes;
            auto result = value.encode(bytes);
            if (!result)
                return failure(ESettingsError::INVALID_VALUE, "configuration encoding");
            return bytes;
        }
    } // namespace
    struct SettingsEntry::Storage final
    {
        std::string id, label;
        ConfigurationDescriptor configuration;
        SettingsDescriptor descriptor;
        explicit Storage(const SettingsDescriptor& source)
            : id(source.id.name()), label(source.label),
              configuration(source.configuration ? *source.configuration : ConfigurationDescriptor{}),
              descriptor(source)
        {
            descriptor.id = SettingsIdView{id};
            descriptor.label = label;
            descriptor.configuration = &configuration;
        }
    };
    SettingsEntry::SettingsEntry(lux::object::CodeLease code, const SettingsDescriptor& descriptor, Apply apply)
        : code_(std::move(code)), descriptor_(&descriptor), apply_(std::move(apply))
    {
    }
    SettingsEntry::~SettingsEntry() = default;
    std::shared_ptr<SettingsEntry> SettingsEntry::create(
        lux::object::CodeLease code,
        const SettingsDescriptor& descriptor,
        Apply apply
    )
    {
        auto result = std::shared_ptr<SettingsEntry>(new SettingsEntry(std::move(code), descriptor, std::move(apply)));
        result->storage_ = std::make_unique<const Storage>(descriptor);
        result->descriptor_ = &result->storage_->descriptor;
        return result;
    }
    const SettingsDescriptor& SettingsEntry::descriptor() const noexcept
    {
        return *descriptor_;
    }
    bool SettingsEntry::usesCode(const lux::object::CodeLease& code) const noexcept
    {
        return code_.sameOwner(code);
    }
    SettingsResult<void> SettingsEntry::validateDescriptor() const noexcept
    {
        const auto& value = *descriptor_;
        constexpr auto all_scopes = (scopeBit(ESettingsScope::LAUNCH) << 1) - 1;
        const bool has_identity = value.id.isValid() && !value.label.empty();
        const bool has_schema = value.configuration && !value.configuration->schema_name.empty() &&
                                value.configuration->schema_version && value.configuration->codec.valid() &&
                                value.configuration->reflection;
        const bool has_application = value.apply == ESettingsApply::RESTART || bool(apply_);
        const bool has_policy = value.validate && value.scopes && !(value.scopes & ~all_scopes) &&
                                value.apply <= ESettingsApply::RESTART && has_application;
        const bool is_invalid = !code_.valid() || !has_identity || !has_schema || !has_policy;
        if (is_invalid)
            return failure(ESettingsError::INVALID_DESCRIPTOR, std::string(value.id.name()));
        return {};
    }
    SettingsResult<void> SettingsEntry::validate(const ConfigurationValue& value) const noexcept
    {
        auto registered = validateDescriptor();
        if (!registered)
            return registered;
        if (value.type() != descriptor_->configuration->codec.type)
            return failure(ESettingsError::INVALID_VALUE, "configuration type mismatch");
        return descriptor_->validate(value);
    }
    SettingsResult<ConfigurationValue> SettingsEntry::defaults() const noexcept
    {
        if (!meta::ReflectionRegistry::initialized())
            return failure(ESettingsError::UNAVAILABLE, "configuration reflection unavailable");
        return defaults(meta::ReflectionRegistry::instance());
    }
    SettingsResult<void> SettingsEntry::validateDefault(meta::ReflectionRegistry& registry) const noexcept
    {
        auto value = defaults(registry);
        return value ? SettingsResult<void>{} : cxx::unexpected(value.error());
    }
    SettingsResult<void> SettingsEntry::apply(const ConfigurationValue& value)
    {
        auto valid = validate(value);
        if (!valid)
            return valid;
        if (!apply_)
            return failure(ESettingsError::UNAVAILABLE, "setting has no active application receiver");
        try
        {
            return apply_(value);
        }
        catch (const std::bad_alloc&)
        {
            std::terminate();
        }
        catch (...)
        {
            return failure(ESettingsError::CALLBACK, "settings application callback");
        }
    }
    SettingsResult<ConfigurationValue> SettingsEntry::defaults(meta::ReflectionRegistry& registry) const noexcept
    {
        auto valid = validateDescriptor();
        if (!valid)
            return cxx::unexpected(valid.error());
        auto value = ConfigurationValue::create(
            *descriptor_->configuration,
            std::make_shared<lux::object::CodeLease>(code_),
            registry
        );
        if (!value)
            return failure(ESettingsError::UNAVAILABLE, "configuration reflection/type unavailable");
        // The codec's default is authoritative, including custom defaults differing from C++ construction.
        std::vector<std::byte> bytes;
        auto encoded_default = descriptor_->configuration->codec.encode_default(bytes);
        if (!encoded_default || !descriptor_->configuration->codec.decode(bytes, value->data()))
            return failure(ESettingsError::INVALID_VALUE, "default configuration codec");
        auto checked = validate(*value);
        if (!checked)
            return cxx::unexpected(checked.error());
        return std::move(*value);
    }
    SettingsResult<ConfigurationValue> SettingsEntry::decode(std::uint32_t schema, std::span<const std::byte> bytes)
        const noexcept
    {
        auto value = defaults();
        if (!value)
            return value;
        std::vector<std::byte> migrated;
        if (schema != descriptor_->configuration->schema_version)
        {
            if (!descriptor_->migrate)
                return failure(ESettingsError::UNSUPPORTED_VERSION, std::string(descriptor_->id.name()));
            auto converted = descriptor_->migrate(schema, bytes);
            if (!converted)
                return cxx::unexpected(converted.error());
            migrated = std::move(*converted);
            bytes = migrated;
        }
        if (!value->decode(bytes))
            return failure(ESettingsError::INVALID_VALUE, "configuration payload");
        auto checked = validate(*value);
        if (!checked)
            return cxx::unexpected(checked.error());
        return value;
    }
    SettingsResult<void> validateSettingsEntries(
        std::span<const std::shared_ptr<SettingsEntry>> entries,
        std::size_t capacity
    )
    {
        if (entries.size() > capacity)
            return failure(ESettingsError::CAPACITY, "settings declarations");
        std::vector<const SettingsEntry*> sorted;
        sorted.reserve(entries.size());
        for (const auto& entry : entries)
        {
            if (!entry)
                return failure(ESettingsError::INVALID_DESCRIPTOR, "null settings entry");
            auto valid = entry->validateDescriptor();
            if (!valid)
                return valid;
            sorted.push_back(entry.get());
        }
        std::ranges::sort(sorted, {}, [](const SettingsEntry* entry) { return entry->descriptor().id.hash(); });
        for (std::size_t i = 1; i < sorted.size(); ++i)
        {
            const auto lhs = sorted[i - 1]->descriptor().id;
            const auto rhs = sorted[i]->descriptor().id;
            if (lhs.hash() == rhs.hash())
                return failure(
                    lhs.name() == rhs.name() ? ESettingsError::DUPLICATE : ESettingsError::COLLISION,
                    std::string(rhs.name())
                );
        }
        return {};
    }
    SettingsResult<SettingsResolution> resolveSettings(
        std::shared_ptr<const SettingsEntry> entry,
        std::span<const SettingsDocument> sources
    )
    {
        if (!entry)
            return failure(ESettingsError::INVALID_DESCRIPTOR, "null settings entry");
        auto value = entry->defaults();
        if (!value)
            return cxx::unexpected(value.error());
        std::array<const SettingsDocument*, 5> ordered{};
        for (const auto& source : sources)
        {
            const auto scope = static_cast<std::size_t>(source.scope);
            if (scope >= ordered.size())
                return failure(ESettingsError::INVALID_SCOPE, "settings source scope");
            if (source.schema != 1)
                return failure(ESettingsError::UNSUPPORTED_VERSION, "settings document");
            if (ordered[scope])
                return failure(ESettingsError::DUPLICATE, "settings source scope");
            ordered[scope] = &source;
        }
        SettingsResolution result{entry, std::move(*value), {}};
        const auto& descriptor = entry->descriptor();
        for (const auto* source : ordered)
        {
            if (!source)
                continue;
            const SettingsValue* selected{};
            for (const auto& item : source->values)
            {
                if (item.id != descriptor.id.name())
                    continue;
                if (selected)
                    return failure(ESettingsError::DUPLICATE, item.id);
                selected = &item;
            }
            if (!selected)
                continue;
            if (!(descriptor.scopes & scopeBit(source->scope)))
                return failure(ESettingsError::INVALID_SCOPE, selected->id);
            auto incoming = entry->decode(selected->schema, selected->bytes);
            if (!incoming)
                return cxx::unexpected(incoming.error());
            if (descriptor.merge)
            {
                auto merged = descriptor.merge(result.desired, *incoming);
                if (!merged)
                    return cxx::unexpected(merged.error());
                auto valid = entry->validate(result.desired);
                if (!valid)
                    return cxx::unexpected(valid.error());
            }
            else
                result.desired = std::move(*incoming);
            result.source = source->scope;
        }
        return result;
    }
    SettingsResult<SettingsDraft> makeSettingsDraft(const SettingsResolution& value, const SettingsDocument& source)
    {
        if (!value.entry)
            return failure(ESettingsError::INVALID_DESCRIPTOR, "null settings resolution");
        const bool is_invalid_scope =
            source.scope > ESettingsScope::LAUNCH || !(value.entry->descriptor().scopes & scopeBit(source.scope));
        if (is_invalid_scope)
            return failure(ESettingsError::INVALID_SCOPE, "settings draft");
        auto bytes = encoded(value.desired);
        if (!bytes)
            return cxx::unexpected(bytes.error());
        auto copy = value.entry->decode(value.entry->descriptor().configuration->schema_version, *bytes);
        if (!copy)
            return cxx::unexpected(copy.error());
        SettingsDraft result{value.entry, source.scope, source.file_version, std::move(*copy), {}, {}};
        for (const auto& item : source.values)
            if (item.id == value.entry->descriptor().id.name())
                result.persisted = item.bytes;
        return result;
    }
    SettingsResult<SettingsDocument> prepareSettings(
        const SettingsDraft& draft,
        const SettingsDocument& source,
        const SettingsEntry& current
    )
    {
        const bool is_file_mismatch = draft.based_on != source.file_version || draft.scope != source.scope;
        const bool is_registration_mismatch = draft.entry.get() != &current;
        if (is_file_mismatch || is_registration_mismatch)
            return failure(ESettingsError::CONFLICT, "settings file or declaration changed; reload/revert required");
        auto valid = current.validate(draft.desired);
        if (!valid)
            return cxx::unexpected(valid.error());
        auto bytes = encoded(draft.desired);
        if (!bytes)
            return cxx::unexpected(bytes.error());
        SettingsDocument candidate = source;
        const auto& descriptor = current.descriptor();
        SettingsValue row{
            std::string(descriptor.id.name()),
            descriptor.configuration->schema_version,
            std::move(*bytes)
        };
        auto found = std::ranges::find(candidate.values, row.id, &SettingsValue::id);
        if (found == candidate.values.end())
            candidate.values.push_back(std::move(row));
        else
            *found = std::move(row);
        return candidate;
    }
} // namespace lux::editor::settings
