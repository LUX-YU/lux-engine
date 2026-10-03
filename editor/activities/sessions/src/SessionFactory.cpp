#include <lux/engine/editor/sessions/SessionFactory.hpp>
#include <variant>
#include <algorithm>
#include <unordered_map>
namespace lux::editor::sessions
{
    struct SessionPreparation::Data final
    {
        contracts::CodeLease code;
        using VPreparation = std::variant<Prepare, Reload>;
        VPreparation preparation;
        std::optional<ContentStamp> reload;
    };
    SessionPreparation::SessionPreparation(contracts::CodeLease code, Prepare prepare)
        : data_(std::make_unique<Data>(std::move(code), std::move(prepare)))
    {
    }
    SessionPreparation::SessionPreparation(contracts::CodeLease code, ContentStamp expected, Reload reload)
        : data_(std::make_unique<Data>(std::move(code), std::move(reload), expected))
    {
    }
    SessionPreparation::~SessionPreparation() = default;
    SessionPreparation::SessionPreparation(SessionPreparation&&) noexcept = default;
    SessionPreparation& SessionPreparation::operator=(SessionPreparation&&) noexcept = default;
    SessionFactoryResult<PreparedSessionInstallation> SessionPreparation::prepare(
        SessionStore& store,
        persistence::SaveService& saves
    ) &&
    {
        // A completed worker result survives temporary owner admission failure, even when called
        // through an rvalue. The actual owning transfer starts only after both owners permit it.
        const auto session_ready = store.canReserve();
        if (!session_ready)
            return cxx::unexpected(factoryFailure(session_ready.error()));
        const auto save_ready = saves.canPrepareSource();
        if (!save_ready)
            return cxx::unexpected(SessionFactoryFailure{
                save_ready.error().code == persistence::EPersistenceError::BUSY ? ESessionFactoryError::BUSY
                                                                                : ESessionFactoryError::WRONG_THREAD,
                "persistence",
                static_cast<std::uint64_t>(save_ready.error().code),
                save_ready.error().detail
            });
        auto owned = std::move(data_);
        auto* prepare = owned ? std::get_if<Prepare>(&owned->preparation) : nullptr;
        const bool is_invalid_owner = !owned || !owned->code.valid();
        const bool is_missing_prepare = !prepare || !*prepare;
        const bool is_invalid_input = is_invalid_owner || is_missing_prepare;
        if (is_invalid_input)
            return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::INVALID_ARGUMENT, "prepared.data"});
        auto invoke = [&]() -> SessionFactoryResult<PreparedSessionInstallation>
        {
            if (owned->code.sameOwner(contracts::CodeLease::builtin()))
                return (*prepare)(store, saves);
            try
            {
                return (*prepare)(store, saves);
            }
            catch (const std::bad_alloc&)
            {
                std::terminate();
            }
            catch (...)
            {
                return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::CONSTRUCT, "plugin.session.prepare"}
                );
            }
        };
        auto result = invoke();
        if (result && !result->usesCode(owned->code))
            return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::ROLE, "installation.code"});
        return result;
    }
    SessionFactoryResult<PreparedSessionReload> SessionPreparation::prepareReload(SessionStore& store) &&
    {
        // Store reentry is temporary: keep the accepted decode result intact until owner admission.
        const bool is_invalid_owner = !data_ || !data_->code.valid();
        const bool is_missing_reload = data_ && !data_->reload;
        const bool is_invalid_input = is_invalid_owner || is_missing_reload;
        if (is_invalid_input)
            return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::INVALID_ARGUMENT, "reload.data"});
        const auto ready = store.describe(data_->reload->session);
        if (!ready)
            return cxx::unexpected(factoryFailure(ready.error()));
        if (ready->admission != EEditAdmission::AVAILABLE)
            return cxx::unexpected(factoryFailure(ESessionError::BUSY));
        auto* reload = std::get_if<Reload>(&data_->preparation);
        if (!reload || !*reload)
            return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::INVALID_ARGUMENT, "reload.unsupported"});
        // The domain callback checks its original stamp/gate before consuming the owned source.
        // On BUSY it leaves its input intact, so no encoder/decoder is rerun on retry.
        auto invoke = [&]() -> SessionFactoryResult<PreparedSessionReload>
        {
            if (data_->code.sameOwner(contracts::CodeLease::builtin()))
                return (*reload)(store);
            try
            {
                return (*reload)(store);
            }
            catch (const std::bad_alloc&)
            {
                std::terminate();
            }
            catch (...)
            {
                return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::CALLBACK, "plugin.reload"});
            }
        };
        auto result = invoke();
        if (result || result.error().code != ESessionFactoryError::BUSY)
            data_.reset();
        return result;
    }
    PreparedSessionReload::PreparedSessionReload(
        contracts::CodeLease code,
        SessionId session,
        Adopt adopt,
        std::unique_ptr<persistence::ISaveSource> source
    )
        : code_(std::move(code)), session_(session), adopt_(std::move(adopt)), source_(std::move(source))
    {
    }
    PreparedSessionReload::~PreparedSessionReload() = default;
    PreparedSessionReload::PreparedSessionReload(PreparedSessionReload&&) noexcept = default;
    PreparedSessionReload& PreparedSessionReload::operator=(PreparedSessionReload&& other) noexcept
    {
        if (this != &other)
        {
            PreparedSessionReload previous(std::move(*this));
            code_ = std::move(other.code_);
            session_ = other.session_;
            adopt_ = std::move(other.adopt_);
            source_ = std::move(other.source_);
        }
        return *this;
    }
    bool SessionPreparation::usesCode(const contracts::CodeLease& code) const noexcept
    {
        return data_ && data_->code.sameOwner(code);
    }
    struct SessionFactoryEntry::DescriptorStorage final
    {
        std::string text;
        std::vector<std::string_view> extensions;
        SessionKindDescriptor descriptor;
        explicit DescriptorStorage(const SessionKindDescriptor& input)
        {
            auto size = input.kind.name().size() + input.label.size();
            if (input.source)
                size += input.source->canonical_name.size() + input.source->save_extension.size();
            for (const auto extension : input.extensions)
                size += extension.size();
            text.reserve(size);
            text.append(input.kind.name()).append(input.label);
            for (const auto extension : input.extensions)
                text.append(extension);
            if (input.source)
                text.append(input.source->canonical_name).append(input.source->save_extension);
            const std::string_view bytes{text};
            std::size_t offset{};
            const auto take = [&](std::size_t count)
            {
                const auto value = bytes.substr(offset, count);
                offset += count;
                return value;
            };
            descriptor.kind = SessionKindIdView{take(input.kind.name().size())};
            descriptor.label = take(input.label.size());
            extensions.reserve(input.extensions.size());
            for (const auto extension : input.extensions)
                extensions.push_back(take(extension.size()));
            descriptor.extensions = extensions;
            if (input.source)
                descriptor.source = SourceAuthoring{
                    take(input.source->canonical_name.size()),
                    input.source->version,
                    take(input.source->save_extension.size()),
                    input.source->is_default
                };
        }
    };
    std::shared_ptr<SessionFactoryEntry> SessionFactoryEntry::create(
        contracts::CodeLease code,
        const SessionKindDescriptor& descriptor,
        Decode decode
    )
    {
        auto storage = std::make_unique<const DescriptorStorage>(descriptor);
        auto entry = std::shared_ptr<SessionFactoryEntry>(
            new SessionFactoryEntry(std::move(code), storage->descriptor, std::move(decode))
        );
        entry->storage_ = std::move(storage);
        return entry;
    }
    SessionFactoryEntry::SessionFactoryEntry(
        contracts::CodeLease code,
        const SessionKindDescriptor& descriptor,
        Decode decode
    )
        : code_(std::move(code)), descriptor_(&descriptor), decode_(std::move(decode))
    {
    }
    SessionFactoryEntry::~SessionFactoryEntry() = default;
    const SessionKindDescriptor& SessionFactoryEntry::descriptor() const noexcept
    {
        return *descriptor_;
    }
    struct SessionFactorySnapshot::Data final
    {
        struct Identity final
        {
            std::uint64_t hash;
            std::size_t entry;
        };
        std::vector<std::shared_ptr<SessionFactoryEntry>> entries;
        std::vector<Identity> index;
        std::unordered_map<asset::AssetTypeId, std::vector<std::size_t>> sources;
    };
    SessionFactoryResult<SessionFactorySnapshot> SessionFactorySnapshot::create(
        std::vector<std::shared_ptr<SessionFactoryEntry>> entries,
        std::size_t capacity
    )
    {
        for (auto& entry : entries)
            if (entry)
            {
                auto code = entry->code_;
                entry = contracts::pinCodeOwner(std::move(code), std::move(entry));
            }
        if (entries.size() > capacity)
            return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::CAPACITY, "factory"});
        std::unordered_map<asset::AssetTypeId, std::vector<std::size_t>> sources;
        for (std::size_t i{}; i < entries.size(); ++i)
        {
            if (!entries[i])
                return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::INVALID_ARGUMENT, "factory"});
            const auto& entry = *entries[i];
            const auto& descriptor = entry.descriptor();
            const bool is_invalid_identity =
                !descriptor.kind.isValid() || descriptor.kind.hash() != cxx::Fnv1a64::hash(descriptor.kind.name());
            const bool is_invalid_binding = !entry.code_.valid() || !entry.decode_;
            const bool is_invalid_description = descriptor.label.empty();
            const bool is_invalid = is_invalid_identity || is_invalid_binding || is_invalid_description;
            if (is_invalid)
                return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::INVALID_ARGUMENT, "factory"});
            if (const auto& source = entry.descriptor_->source)
            {
                const auto& name = source->canonical_name;
                const auto& suffix = source->save_extension;
                const bool is_invalid_name = name.empty() || name.size() > 192 ||
                                             !std::ranges::all_of(
                                                 name,
                                                 [](unsigned char c)
                                                 {
                                                     return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                                            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
                                                 }
                                             );
                const bool is_invalid_suffix = suffix.size() < 2 || suffix.size() > 64 || suffix.front() != '.' ||
                                               !std::ranges::all_of(
                                                   std::string_view{suffix}.substr(1),
                                                   [](unsigned char c) {
                                                       return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                                              (c >= '0' && c <= '9') || c == '_' || c == '-';
                                                   }
                                               );
                const bool is_invalid_source = is_invalid_name || is_invalid_suffix || !source->version;
                if (is_invalid_source)
                {
                    return cxx::unexpected(
                        SessionFactoryFailure{ESessionFactoryError::INVALID_ARGUMENT, "factory.source"}
                    );
                }
                auto& indices = sources[source->type()];
                if (!indices.empty() && entries[indices.front()]->descriptor_->source->canonical_name != name)
                {
                    return cxx::unexpected(
                        SessionFactoryFailure{ESessionFactoryError::INVALID_ARGUMENT, "factory.source.collision"}
                    );
                }
                indices.push_back(i);
            }
        }
        std::vector<Data::Identity> index;
        index.reserve(entries.size());
        for (std::size_t i{}; i < entries.size(); ++i)
            index.push_back({entries[i]->descriptor().kind.hash(), i});
        std::ranges::sort(index, {}, &Data::Identity::hash);
        for (std::size_t i = 1; i < index.size(); ++i)
        {
            if (index[i - 1].hash != index[i].hash)
                continue;
            const bool is_duplicate = entries[index[i - 1].entry]->descriptor().kind.name() ==
                                      entries[index[i].entry]->descriptor().kind.name();
            return cxx::unexpected(SessionFactoryFailure{
                is_duplicate ? ESessionFactoryError::INVALID_ARGUMENT : ESessionFactoryError::HASH_COLLISION,
                is_duplicate ? "factory.duplicate" : "factory.identity.collision"
            });
        }
        SessionFactorySnapshot result;
        result.data_ = std::make_shared<Data>(std::move(entries), std::move(index), std::move(sources));
        return result;
    }
    SessionFactoryResult<std::shared_ptr<SessionFactoryEntry>> SessionFactorySnapshot::find(SessionKindId kind) const
    {
        const auto pinned = data_;
        if (pinned)
        {
            const auto hash = cxx::Fnv1a64::hash(kind.name);
            const auto found = std::ranges::lower_bound(pinned->index, hash, {}, &Data::Identity::hash);
            if (found != pinned->index.end() && found->hash == hash)
            {
                const auto& entry = pinned->entries[found->entry];
                // Source/open requests are cold text boundaries; do not accept a colliding external name.
                if (entry->descriptor().kind.name() == kind.name)
                    return entry;
            }
        }
        return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::NOT_FOUND, "factory"});
    }
    SessionFactoryResult<std::shared_ptr<SessionFactoryEntry>> SessionFactorySnapshot::selectSource(
        std::string_view canonical_name,
        std::uint32_t version,
        std::optional<SessionKindId> preferred
    ) const
    {
        if (canonical_name.empty() || !version)
        {
            return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::INVALID_ARGUMENT, "factory.source"});
        }
        const auto pinned = data_;
        if (!pinned)
        {
            return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::NOT_FOUND, "factory.source"});
        }
        const auto found = pinned->sources.find(asset::AssetTypeId::fromName(canonical_name));
        if (found == pinned->sources.end())
        {
            return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::NOT_FOUND, "factory.source"});
        }
        std::shared_ptr<SessionFactoryEntry> only, selected;
        std::size_t matches{}, defaults{};
        std::string candidates;
        for (const auto index : found->second)
        {
            const auto& entry = pinned->entries[index];
            const auto& source = *entry->descriptor_->source;
            if (source.canonical_name != canonical_name || source.version != version)
            {
                continue;
            }
            if (preferred && entry->descriptor_->kind.name() == preferred->name)
            {
                return entry;
            }
            ++matches;
            only = entry;
            if (source.is_default)
            {
                ++defaults;
                selected = entry;
            }
            if (!candidates.empty())
            {
                candidates += ", ";
            }
            candidates += entry->descriptor_->kind.name();
        }
        if (!matches || preferred)
        {
            return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::NOT_FOUND, "factory.source"});
        }
        if (defaults == 1)
        {
            return selected;
        }
        if (matches == 1)
        {
            return only;
        }
        return cxx::unexpected(
            SessionFactoryFailure{ESessionFactoryError::AMBIGUOUS, "factory.source", 0, std::move(candidates)}
        );
    }
    std::span<const std::shared_ptr<SessionFactoryEntry>> SessionFactorySnapshot::entries() const noexcept
    {
        return data_ ? std::span<const std::shared_ptr<SessionFactoryEntry>>(data_->entries)
                     : std::span<const std::shared_ptr<SessionFactoryEntry>>{};
    }
    SessionLoadJob::SessionLoadJob(std::shared_ptr<SessionFactoryEntry> entry, SessionLoadInput input)
        : entry_(std::move(entry)), input_(std::move(input))
    {
    }
    SessionFactoryResult<SessionPreparation> SessionLoadJob::run(std::stop_token stop) &&
    {
        auto owned = std::move(*this);
        const bool invalid =
            !owned.entry_ || !owned.input_.source || owned.input_.asset.isNull() || !owned.input_.max_bytes;
        if (invalid)
            return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::INVALID_ARGUMENT, "load"});
        if (stop.stop_requested())
            return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::CANCELLED, "load"});
        auto blob = owned.input_.source.open(owned.input_.asset);
        if (!blob)
            return cxx::unexpected(SessionFactoryFailure{
                blob.error() == asset::EAssetStorageError::CONTENT_CHANGED ? ESessionFactoryError::STALE_CONTENT
                                                                           : ESessionFactoryError::IO,
                "asset.storage",
                static_cast<std::uint64_t>(blob.error())
            });
        if (blob->bytes.size() > owned.input_.max_bytes)
            return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::CAPACITY, "load.bytes"});
        if (stop.stop_requested())
            return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::CANCELLED, "load"});
        auto invoke = [&]() -> SessionFactoryResult<SessionPreparation>
        {
            if (owned.entry_->code_.sameOwner(contracts::CodeLease::builtin()))
                return owned.entry_->decode_(owned.input_, blob->bytes.view(), stop);
            try
            {
                return owned.entry_->decode_(owned.input_, blob->bytes.view(), stop);
            }
            catch (const std::bad_alloc&)
            {
                std::terminate();
            }
            catch (...)
            {
                return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::DECODE, "plugin.session.decode"});
            }
        };
        auto result = invoke();
        if (result && !result->usesCode(owned.entry_->code_))
            return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::DECODE, "decoded.code"});
        return result;
    }
} // namespace lux::editor::sessions
