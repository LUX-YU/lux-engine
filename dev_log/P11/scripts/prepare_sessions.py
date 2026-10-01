from pathlib import Path
r=Path(r"E:/SyncForder/CodeRepos/lux-engine-p11")
def edit(name, old, new):
 p=r/name; s=p.read_text(); assert old in s, name; p.write_text(s.replace(old,new))
edit("editor/activities/persistence/include/lux/engine/editor/persistence/SaveSource.hpp",
 "    class SaveService;", "    class SaveService;\n    class PreparedSaveSourceRegistration;")
edit("editor/activities/persistence/include/lux/engine/editor/persistence/SaveSource.hpp",
 "        friend class SaveService;", "        friend class SaveService;\n        friend class PreparedSaveSourceRegistration;")
edit("editor/activities/persistence/include/lux/engine/editor/persistence/SaveSource.hpp",
 "        std::shared_ptr<State> state_;\n    };",
 """        std::shared_ptr<State> state_;
    };
    // Hidden role reservation. Abandonment revokes without invoking the source.
    // Move assignment clears the previous reservation under its previous code owner.
    class PreparedSaveSourceRegistration final
    {
    public:
        ~PreparedSaveSourceRegistration();
        PreparedSaveSourceRegistration(PreparedSaveSourceRegistration&&) noexcept;
        PreparedSaveSourceRegistration& operator=(PreparedSaveSourceRegistration&&) noexcept;
        PreparedSaveSourceRegistration(const PreparedSaveSourceRegistration&) = delete;
        PreparedSaveSourceRegistration& operator=(const PreparedSaveSourceRegistration&) = delete;
    private:
        friend class SaveService;
        explicit PreparedSaveSourceRegistration(std::shared_ptr<SaveSourceRegistration::State>) noexcept;
        std::shared_ptr<SaveSourceRegistration::State> state_;
    };""")
edit("editor/activities/persistence/include/lux/engine/editor/persistence/SaveService.hpp",
 "        [[nodiscard]] PersistenceResult<SaveId> requestSave(SaveRequest request);",
 """        // No describe callback: a factory supplies the real reserved identity while its Session is hidden.
        [[nodiscard]] PersistenceResult<PreparedSaveSourceRegistration> prepareSource(
            sessions::SessionId, ISaveSource&, contracts::CodeLease = contracts::CodeLease::builtin()
        );
        [[nodiscard]] PersistenceResult<void> canPublish(const PreparedSaveSourceRegistration&) const noexcept;
        // Requires a successful canPublish with no intervening callback/mutation, on the same owner.
        // This commit only changes prepared visibility; it allocates nothing and calls no source.
        [[nodiscard]] SaveSourceRegistration publish(PreparedSaveSourceRegistration&&) noexcept;
        [[nodiscard]] PersistenceResult<SaveId> requestSave(SaveRequest request);""")
edit("editor/activities/persistence/src/SaveService.cpp", "        sessions::SessionId session;\n    };",
 "        sessions::SessionId session;\n        const void* service;\n        bool published{};\n    };")
edit("editor/activities/persistence/src/SaveService.cpp", "    struct SaveService::Impl final",
 """    PreparedSaveSourceRegistration::PreparedSaveSourceRegistration(
        std::shared_ptr<SaveSourceRegistration::State> state
    ) noexcept : state_(std::move(state)) {}
    PreparedSaveSourceRegistration::~PreparedSaveSourceRegistration()
    {
        if (state_)
            state_->source = nullptr;
    }
    PreparedSaveSourceRegistration::PreparedSaveSourceRegistration(PreparedSaveSourceRegistration&&) noexcept = default;
    PreparedSaveSourceRegistration& PreparedSaveSourceRegistration::operator=(PreparedSaveSourceRegistration&& other) noexcept
    {
        if (this != &other)
        {
            if (state_)
                state_->source = nullptr;
            state_ = std::move(other.state_);
        }
        return *this;
    }
    struct SaveService::Impl final""")
edit("editor/activities/persistence/src/SaveService.cpp",
 "        std::shared_ptr<SaveSourceRegistration::State> source(sessions::SessionId id)",
 "        std::shared_ptr<SaveSourceRegistration::State> source(sessions::SessionId id, bool include_prepared = false)")
edit("editor/activities/persistence/src/SaveService.cpp",
 "if (auto found = weak.lock(); found && found->session == id && found->source)",
 "if (auto found = weak.lock(); found && found->session == id && found->source && (include_prepared || found->published))")
edit("editor/activities/persistence/src/SaveService.cpp", "        void releaseSnapshot(Operation& op)",
 """        PersistenceResult<PreparedSaveSourceRegistration> prepare(
            sessions::SessionId id, ISaveSource& source_value, contracts::CodeLease code
        )
        {
            if (!id.valid() || !code.valid())
                return failed(EPersistenceError::INVALID_ARGUMENT);
            if (source(id, true))
                return failed(EPersistenceError::BUSY);
            std::erase_if(sources, [](const auto& weak) {
                auto value = weak.lock();
                return !value || !value->source;
            });
            auto entry = std::make_shared<SaveSourceRegistration::State>(std::move(code), &source_value, id, this);
            sources.push_back(entry);
            return PreparedSaveSourceRegistration{std::move(entry)};
        }
        void releaseSnapshot(Operation& op)""")
p=r/"editor/activities/persistence/src/SaveService.cpp";s=p.read_text();a=s.index("        if (impl_->source(info->content.session))");b=s.index("    PersistenceResult<SaveId> SaveService::requestSave",a)
s=s[:a]+"""        auto prepared = impl_->prepare(info->content.session, source, std::move(code));
        if (!prepared)
            return lux::cxx::unexpected(prepared.error());
        prepared->state_->published = true;
        return SaveSourceRegistration{std::move(prepared->state_)};
    }
    PersistenceResult<PreparedSaveSourceRegistration> SaveService::prepareSource(
        sessions::SessionId id, ISaveSource& source, contracts::CodeLease code
    )
    {
        if (!impl_->onOwner())
            return failed(EPersistenceError::WRONG_THREAD);
        if (impl_->dispatching)
            return failed(EPersistenceError::BUSY);
        const Impl::DispatchScope dispatch{impl_->dispatching};
        return impl_->prepare(id, source, std::move(code));
    }
    PersistenceResult<void> SaveService::canPublish(const PreparedSaveSourceRegistration& prepared) const noexcept
    {
        if (!impl_->onOwner())
            return failed(EPersistenceError::WRONG_THREAD);
        if (impl_->dispatching)
            return failed(EPersistenceError::BUSY);
        const auto& state = prepared.state_;
        const bool invalid = !state || state->service != impl_.get() || !state->source || state->published;
        if (invalid)
            return failed(EPersistenceError::INVALID_ARGUMENT);
        return {};
    }
    SaveSourceRegistration SaveService::publish(PreparedSaveSourceRegistration&& prepared) noexcept
    {
        if (!canPublish(prepared))
            std::terminate();
        prepared.state_->published = true;
        return SaveSourceRegistration{std::move(prepared.state_)};
    }
"""+s[b:];p.write_text(s)
# A typed key for a reserved identity conveys no access to hidden content.
edit("editor/editing/include/lux/engine/editor/sessions/SessionStore.hpp",
 "        template <class T> [[nodiscard]] TSessionAccess<T> access() noexcept",
 """        template <class T> [[nodiscard]] SessionResult<TSessionKey<T>> key(const SessionReservation& reservation) const noexcept
        {
            auto checked = reservedKey(reservation, lux::cxx::typeToken<T>());
            if (!checked)
                return lux::cxx::unexpected(checked.error());
            return TSessionKey<T>{*checked};
        }
        template <class T> [[nodiscard]] TSessionAccess<T> access() noexcept""")
edit("editor/editing/include/lux/engine/editor/sessions/SessionStore.hpp",
 "        void abandon(SessionId id) noexcept;",
 """        [[nodiscard]] SessionResult<SessionId> reservedKey(const SessionReservation&, lux::cxx::TypeToken) const noexcept;
        void abandon(SessionId id) noexcept;""")
edit("editor/editing/src/sessions/SessionStore.cpp", "    void SessionStore::abandon(SessionId id) noexcept",
 """    SessionResult<SessionId> SessionStore::reservedKey(const SessionReservation& reservation, lux::cxx::TypeToken type) const noexcept
    {
        if (reservation.store_ != this)
            return lux::cxx::unexpected(ESessionError::WRONG_STORE);
        auto slot = impl_->slot(reservation.id());
        if (!slot)
            return lux::cxx::unexpected(slot.error());
        if ((*slot)->type != type)
            return lux::cxx::unexpected(ESessionError::WRONG_TYPE);
        return reservation.id();
    }
    void SessionStore::abandon(SessionId id) noexcept""")
