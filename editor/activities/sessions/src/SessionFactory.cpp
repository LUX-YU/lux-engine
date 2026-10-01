#include <lux/engine/editor/sessions/SessionFactory.hpp>
#include <variant>
namespace lux::editor::sessions
{
    struct PreparedSessionData::Data final
    {
        contracts::CodeLease code;
        using VPreparation = std::variant<Prepare, Reload>;
        VPreparation preparation;
        std::optional<ContentStamp> reload;
    };
    PreparedSessionData::PreparedSessionData(contracts::CodeLease code, Prepare prepare)
        : data_(std::make_unique<Data>(std::move(code), std::move(prepare)))
    {}
    PreparedSessionData::PreparedSessionData(contracts::CodeLease code, ContentStamp expected, Reload reload)
        : data_(std::make_unique<Data>(std::move(code), std::move(reload), expected))
    {}
    PreparedSessionData::~PreparedSessionData() = default;
    PreparedSessionData::PreparedSessionData(PreparedSessionData&&) noexcept = default;
    PreparedSessionData& PreparedSessionData::operator=(PreparedSessionData&&) noexcept = default;
    SessionFactoryResult<PreparedSessionInstallation> PreparedSessionData::prepare(
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
        if (!owned || !owned->code.valid() || !prepare || !*prepare)
            return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::INVALID_ARGUMENT, "prepared.data"});
        auto invoke = [&]() -> SessionFactoryResult<PreparedSessionInstallation> {
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
    SessionFactoryResult<PreparedSessionReload> PreparedSessionData::prepareReload(SessionStore& store) &&
    {
        // Store reentry is temporary: keep the accepted decode result intact until owner admission.
        if (!data_ || !data_->code.valid() || !data_->reload)
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
        auto invoke = [&]() -> SessionFactoryResult<PreparedSessionReload> {
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
    {}
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
    bool PreparedSessionData::usesCode(const contracts::CodeLease& code) const noexcept
    {
        return data_ && data_->code.sameOwner(code);
    }
    SessionFactoryEntry::SessionFactoryEntry(contracts::CodeLease code, SessionKindDescriptor descriptor, Decode decode)
        : code_(std::move(code)), descriptor_(std::move(descriptor)), decode_(std::move(decode))
    {}
    SessionFactoryEntry::~SessionFactoryEntry() = default;
    const SessionKindDescriptor& SessionFactoryEntry::descriptor() const noexcept
    {
        return descriptor_;
    }
    struct SessionFactorySnapshot::Data final
    {
        std::vector<std::shared_ptr<SessionFactoryEntry>> entries;
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
        for (std::size_t i{}; i < entries.size(); ++i)
        {
            if (!entries[i])
                return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::INVALID_ARGUMENT, "factory"});
            const auto& entry = *entries[i];
            const bool invalid = !entry.code_.valid() || entry.descriptor_.kind.name.empty() ||
                                 entry.descriptor_.label.empty() || !entry.decode_;
            if (invalid)
                return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::INVALID_ARGUMENT, "factory"});
            for (std::size_t j{}; j < i; ++j)
                if (entries[j]->descriptor_.kind == entry.descriptor_.kind)
                    return cxx::unexpected(
                        SessionFactoryFailure{ESessionFactoryError::INVALID_ARGUMENT, "factory.duplicate"}
                    );
        }
        SessionFactorySnapshot result;
        result.data_ = std::make_shared<Data>(std::move(entries));
        return result;
    }
    SessionFactoryResult<std::shared_ptr<SessionFactoryEntry>> SessionFactorySnapshot::find(SessionKindId kind) const
    {
        for (const auto& entry : entries())
            if (entry->descriptor().kind == kind)
                return entry;
        return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::NOT_FOUND, "factory"});
    }
    std::span<const std::shared_ptr<SessionFactoryEntry>> SessionFactorySnapshot::entries() const noexcept
    {
        return data_ ? std::span<const std::shared_ptr<SessionFactoryEntry>>(data_->entries)
                     : std::span<const std::shared_ptr<SessionFactoryEntry>>{};
    }
    SessionLoadJob::SessionLoadJob(std::shared_ptr<SessionFactoryEntry> entry, SessionLoadInput input)
        : entry_(std::move(entry)), input_(std::move(input))
    {}
    SessionFactoryResult<PreparedSessionData> SessionLoadJob::run(std::stop_token stop) &&
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
                ESessionFactoryError::IO,
                "asset.storage",
                static_cast<std::uint64_t>(blob.error())
            });
        if (blob->bytes.size() > owned.input_.max_bytes)
            return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::CAPACITY, "load.bytes"});
        if (stop.stop_requested())
            return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::CANCELLED, "load"});
        auto invoke = [&]() -> SessionFactoryResult<PreparedSessionData> {
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
}
