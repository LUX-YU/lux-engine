#include "Model.hpp"
#include <cmath>

namespace skeleton
{
    using namespace sessions;
    using namespace editing;
    using namespace persistence;
    namespace
    {
        template <class T> T invariant(cxx::expected<T, EditFailure> value)
        {
            if (!value)
                std::terminate();
            return std::move(*value);
        }
        ESessionError historyError(const EditFailure &value)
        {
            if (value.code == EEditError::BUSY)
                return ESessionError::BUSY;
            if (value.code == EEditError::WRONG_THREAD)
                return ESessionError::WRONG_THREAD;
            if (value.code == EEditError::STALE_BASE)
                return ESessionError::STALE_CONTENT;
            return ESessionError::INVALID_ARGUMENT;
        }
        auto saveFailure(ESessionError error)
        {
            const auto code = error == ESessionError::BUSY ? EPersistenceError::BUSY : EPersistenceError::STALE_SOURCE;
            return cxx::unexpected(PersistenceFailure{code, "skeleton.session", static_cast<std::uint64_t>(error)});
        }
        std::size_t bytes(const rdesc::Skeleton &source)
        {
            auto size = sizeof(source) + source.bones.size() * sizeof(rdesc::BoneRestPose);
            for (const auto &bone : source.bones)
                size += bone.name.size();
            return size;
        }
    } // namespace
    class Edit final : public PreparedEdit
    {
    public:
        Edit(const Edit &) = delete;
        Edit &operator=(const Edit &) = delete;
        Edit(Edit &&) = delete;
        Edit &operator=(Edit &&) = delete;
        Edit(Session &owner, rdesc::Skeleton value) : owner_(owner), value_(std::move(value)) {}
        EEditEffect effect() const noexcept override { return EEditEffect::CHANGE; }

    private:
        void apply() noexcept override { std::swap(owner_.source_, value_); }
        void publish(const CommitInfo &) noexcept override { owner_.state_.contentChanged(); }
        Session &owner_;
        rdesc::Skeleton value_;
    };
    class Operation final : public EditOperation
    {
    public:
        Operation(const Operation &) = delete;
        Operation &operator=(const Operation &) = delete;
        Operation(Operation &&) = delete;
        Operation &operator=(Operation &&) = delete;
        Operation(Session &owner, StateId before, rdesc::Skeleton value)
            : owner_(owner), base_(before), before_(owner.source_), after_(std::move(value))
        {
        }
        HistoryId historyId() const noexcept override { return base_.history; }
        StateId baseState() const noexcept override { return base_; }
        std::string_view label() const noexcept override { return "Edit bone name and global translation"; }
        std::size_t retainedBytesUpperBound() const noexcept override { return bytes(before_) + bytes(after_); }
        EditResult<PreparedEditPtr> prepare(const ApplyContext &context,
                                            EditPreparationBudget &budget) const noexcept override
        {
            const auto &value = context.direction == EDirection::FORWARD ? after_ : before_;
            if (auto reserved = budget.reserve(bytes(value)); !reserved)
                return cxx::unexpected(reserved.error());
            return std::make_unique<Edit>(owner_, value);
        }

    private:
        Session &owner_;
        StateId base_;
        rdesc::Skeleton before_, after_;
    };
    Session::Session(SessionId id,
                     SourceBinding binding,
                     std::shared_ptr<const asset::SkeletonAsset> source,
                     std::unique_ptr<EditHistory> history)
        : state_(id, std::move(binding)), info_(source->info()),
          auxiliary_(source->auxiliaryPayloads().begin(), source->auxiliaryPayloads().end()), source_(source->data()),
          history_(std::move(history))
    {
        if (state_.binding() && !state_.loaded(currentContent().state))
            std::terminate();
    }
    SessionResult<std::unique_ptr<Session>> Session::create(SessionId id,
                                                            SourceBinding binding,
                                                            std::shared_ptr<const asset::SkeletonAsset> source)
    {
        if (!source)
            return cxx::unexpected(ESessionError::INVALID_ARGUMENT);
        auto history = EditHistory::create({{128, max_bytes, max_bytes, 256}, {}});
        if (!history)
            return cxx::unexpected(historyError(history.error()));
        return std::unique_ptr<Session>{new Session(id, std::move(binding), std::move(source), std::move(*history))};
    }
    ContentStamp Session::currentContent() const noexcept
    {
        return {state_.id(), invariant(history_->view()).snapshot.current};
    }
    SessionInfo Session::describe() const
    {
        const auto current = currentContent();
        return {state_.id(),
                kind,
                state_.binding(),
                current,
                state_.observed(),
                !state_.checkpoint().clean(current.state, state_.bindingRevision()),
                state_.admission()};
    }
    SessionResult<ClosePermit> Session::prepareClose(ContentStamp expected) noexcept
    {
        return state_.prepareClose(currentContent(), expected);
    }
    SessionResult<void> Session::edit(ContentStamp expected, std::size_t bone, std::string name, float x)
    {
        return state_.gate().withEdit(
            [&](EditScope &) -> SessionResult<void>
            {
                if (currentContent() != expected)
                    return cxx::unexpected(ESessionError::STALE_CONTENT);
                const bool is_invalid_value = bone >= source_.bones.size() || name.empty() || !std::isfinite(x);
                if (is_invalid_value)
                    return cxx::unexpected(ESessionError::INVALID_ARGUMENT);
                const bool is_unchanged =
                    source_.bones[bone].name == name && source_.global_transform.translation().x() == x;
                if (is_unchanged)
                    return {};
                auto after = source_;
                after.bones[bone].name = std::move(name);
                after.global_transform.translation().x() = x;
                EditOperationPtr operation = std::make_unique<Operation>(*this, expected.state, std::move(after));
                auto applied = EditExecutor{}.execute(*history_, operation);
                if (!applied)
                    return cxx::unexpected(historyError(applied.error()));
                return {};
            });
    }
    SessionResult<void> Session::replay(bool forward)
    {
        return state_.gate().withEdit(
            [&](EditScope &) -> SessionResult<void>
            {
                const auto result = forward ? EditExecutor{}.redo(*history_) : EditExecutor{}.undo(*history_);
                if (!result)
                    return cxx::unexpected(historyError(result.error()));
                return {};
            });
    }
    SessionResult<rdesc::Skeleton> Session::read() const
    {
        return state_.gate().withRead([&]() -> SessionResult<rdesc::Skeleton> { return source_; });
    }
    SessionResult<void> Session::withRead(cxx::function_ref<void()> callback) const
    {
        return state_.gate().withRead(
            [&]() -> SessionResult<void>
            {
                callback();
                return {};
            });
    }
    SessionResult<std::shared_ptr<const asset::SkeletonAsset>> Session::freeze(asset::AssetId id) const
    {
        return state_.gate().withRead(
            [&]() -> SessionResult<std::shared_ptr<const asset::SkeletonAsset>>
            {
                auto info = info_;
                info.id = id;
                auto value =
                    asset::SkeletonAsset::create(info, std::make_shared<const rdesc::Skeleton>(source_), auxiliary_);
                if (!value)
                    return cxx::unexpected(ESessionError::INVALID_ARGUMENT);
                return std::move(*value);
            });
    }
    SessionResult<void> Session::adopt(ContentStamp expected, Session &candidate)
    {
        return state_.gate().withEdit(
            [&](EditScope &) -> SessionResult<void>
            {
                if (currentContent() != expected)
                    return cxx::unexpected(ESessionError::STALE_CONTENT);
                using std::swap;
                swap(source_, candidate.source_);
                swap(info_, candidate.info_);
                swap(auxiliary_, candidate.auxiliary_);
                swap(history_, candidate.history_);
                return state_.loaded(currentContent().state);
            });
    }
    class Actions final : public HistoryActions
    {
    public:
        Actions(const Actions &) = delete;
        Actions &operator=(const Actions &) = delete;
        Actions(Actions &&) = delete;
        Actions &operator=(Actions &&) = delete;
        Actions(TSessionAccess<Session> access, TSessionKey<Session> key) : access_(access), key_(key) {}
        SessionFactoryResult<HistoryActionsInfo> query() const override
        {
            auto owner = access_.read(key_);
            if (!owner)
                return cxx::unexpected(factoryFailure(owner.error()));
            auto history = owner->get().history();
            if (!history)
                return cxx::unexpected(factoryFailure(historyError(history.error())));
            return HistoryActionsInfo{{key_.id(), history->snapshot.current}, history->can_undo, history->can_redo};
        }
        SessionFactoryResult<ContentStamp> undo() override { return replay(false); }
        SessionFactoryResult<ContentStamp> redo() override { return replay(true); }

    private:
        SessionFactoryResult<ContentStamp> replay(bool forward)
        {
            auto owner = access_.edit(key_);
            if (!owner)
                return cxx::unexpected(factoryFailure(owner.error()));
            auto applied = owner->get().replay(forward);
            if (!applied)
                return cxx::unexpected(factoryFailure(applied.error()));
            return owner->get().describe().current;
        }
        TSessionAccess<Session> access_;
        TSessionKey<Session> key_;
    };
    class Encoder final : public IEncodeJob
    {
    public:
        Encoder(const Encoder &) = delete;
        Encoder &operator=(const Encoder &) = delete;
        Encoder(Encoder &&) = delete;
        Encoder &operator=(Encoder &&) = delete;
        explicit Encoder(std::shared_ptr<const asset::SkeletonAsset> frozen) : frozen_(std::move(frozen)) {}
        PersistenceResult<EncodedArtifact> encode(std::stop_token stop) override
        {
            if (stop.stop_requested())
                return cxx::unexpected(PersistenceFailure{EPersistenceError::CANCELLED});
            auto encoded =
                asset::TAssetSerDeser<asset::SkeletonAsset>::encode(*frozen_, asset::AssetEncodeLimits{max_bytes});
            if (!encoded)
                return cxx::unexpected(PersistenceFailure{
                    EPersistenceError::ENCODE, "skeleton.codec", static_cast<std::uint64_t>(encoded.error().code)});
            return EncodedArtifact{std::move(*encoded)};
        }

    private:
        std::shared_ptr<const asset::SkeletonAsset> frozen_;
    };
    class Rebind final : public IPreparedRebind
    {
    public:
        Rebind(const Rebind &) = delete;
        Rebind &operator=(const Rebind &) = delete;
        Rebind(Rebind &&) = delete;
        Rebind &operator=(Rebind &&) = delete;
        Rebind(TSessionAccess<Session> access,
               TSessionKey<Session> key,
               SourceBinding binding,
               WriteTarget target,
               std::optional<WriteTarget> &installed,
               BindingRevision &revision,
               BindingChangePermit permit)
            : access_(access), key_(key), binding_(std::move(binding)), target_(std::move(target)),
              installed_(installed), revision_(revision), permit_(std::move(permit))
        {
        }
        EAdoption apply(SaveReceipt &&receipt) noexcept override
        {
            auto owner = access_.edit(key_);
            if (!owner)
                return EAdoption::CLOSED;
            auto &model = owner->get();
            auto changed =
                model.state_.rebind(permit_, std::move(binding_), model.currentContent().state, receipt.order);
            if (!changed)
                return EAdoption::STALE_BINDING;
            target_.expected_version = std::move(receipt.publication.version);
            installed_ = std::move(target_);
            revision_ = model.state_.bindingRevision();
            return EAdoption::APPLIED;
        }

    private:
        TSessionAccess<Session> access_;
        TSessionKey<Session> key_;
        SourceBinding binding_;
        WriteTarget target_;
        std::optional<WriteTarget> &installed_;
        BindingRevision &revision_;
        BindingChangePermit permit_;
    };
    class SaveSource final : public ISaveSource
    {
    public:
        SaveSource(const SaveSource &) = delete;
        SaveSource &operator=(const SaveSource &) = delete;
        SaveSource(SaveSource &&) = delete;
        SaveSource &operator=(SaveSource &&) = delete;
        SaveSource(TSessionAccess<Session> access,
                   TSessionKey<Session> key,
                   std::optional<WriteTarget> target,
                   BindingRevision revision,
                   contracts::CodeLease code)
            : code_(std::move(code)), access_(access), key_(key), target_(std::move(target)), revision_(revision)
        {
        }
        PersistenceResult<SaveSourceInfo> describe() const override
        {
            auto owner = access_.read(key_);
            if (!owner)
                return saveFailure(owner.error());
            auto state = owner->get().state_.gate().withRead(
                [&]() -> SessionResult<SaveSourceInfo>
                {
                    if (owner->get().state_.bindingRevision() != revision_)
                        return cxx::unexpected(ESessionError::STALE_BINDING);
                    return SaveSourceInfo{owner->get().currentContent(), revision_, target_};
                });
            if (!state)
                return saveFailure(state.error());
            return std::move(*state);
        }
        PersistenceResult<FrozenSave> captureForSave(const SaveSourceInfo &expected,
                                                     const SaveRequest &request,
                                                     std::size_t limit) override
        {
            auto current = describe();
            if (!current)
                return cxx::unexpected(current.error());
            const bool is_stale = current->content != expected.content || current->binding != expected.binding;
            if (is_stale)
                return saveFailure(ESessionError::STALE_CONTENT);
            auto owner = access_.edit(key_);
            if (!owner)
                return saveFailure(owner.error());
            auto &model = owner->get();
            if (request.mode == ESaveMode::SAVE && !model.state_.binding())
                return cxx::unexpected(PersistenceFailure{EPersistenceError::UNBOUND});
            const auto id = request.mode == ESaveMode::SAVE ? model.state_.binding()->asset : request.asset;
            auto frozen = model.freeze(id);
            if (!frozen)
                return saveFailure(frozen.error());
            auto retained = bytes((*frozen)->data());
            for (const auto &payload : (*frozen)->auxiliaryPayloads())
                retained += payload.bytes.size();
            if (retained > limit)
                return cxx::unexpected(PersistenceFailure{EPersistenceError::CAPACITY});
            std::unique_ptr<IPreparedRebind> rebind;
            if (request.mode == ESaveMode::SAVE_AS)
            {
                if (!request.destination)
                    return cxx::unexpected(PersistenceFailure{EPersistenceError::INVALID_ARGUMENT});
                auto permit = model.state_.prepareBindingChange(model.currentContent(), expected.content);
                if (!permit)
                    return saveFailure(permit.error());
                rebind = std::make_unique<Rebind>(access_,
                                                  key_,
                                                  BoundSource{id, request.destination->key.value},
                                                  *request.destination,
                                                  target_,
                                                  revision_,
                                                  std::move(*permit));
            }
            return FrozenSave{
                expected, retained, {code_, std::make_unique<Encoder>(std::move(*frozen))}, std::move(rebind)};
        }
        EAdoption accept(SaveReceipt &&receipt) noexcept override
        {
            auto owner = access_.edit(key_);
            if (!owner)
                return EAdoption::CLOSED;
            auto &model = owner->get();
            auto accepted = model.state_.gate().withEdit(
                [&](EditScope &) -> SessionResult<void> {
                    return model.state_.accept(model.currentContent(), receipt.content, receipt.binding, receipt.order);
                });
            if (!accepted)
            {
                if (accepted.error() == ESessionError::BUSY)
                    return EAdoption::BUSY;
                if (accepted.error() == ESessionError::STALE_BINDING)
                    return EAdoption::STALE_BINDING;
                if (accepted.error() == ESessionError::STALE_PUBLICATION)
                    return EAdoption::OLDER_RECEIPT;
                return EAdoption::STALE_HISTORY;
            }
            target_->expected_version = std::move(receipt.publication.version);
            return EAdoption::APPLIED;
        }

    private:
        contracts::CodeLease code_;
        TSessionAccess<Session> access_;
        TSessionKey<Session> key_;
        std::optional<WriteTarget> target_;
        BindingRevision revision_;
    };
    std::shared_ptr<SessionFactoryEntry> factory(contracts::CodeLease code)
    {
        return std::make_shared<SessionFactoryEntry>(
            code,
            SessionKindDescriptor{
                kind, "Skeleton", {"luxskeleton"}, SourceAuthoring{"lux.skeleton", 1, ".luxskeleton"}},
            [code](const SessionLoadInput &input,
                   std::span<const std::byte> image,
                   std::stop_token stop) -> SessionFactoryResult<SessionPreparation>
            {
                if (stop.stop_requested())
                    return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::CANCELLED});
                if (input.binding && input.binding->asset != input.asset)
                    return cxx::unexpected(
                        SessionFactoryFailure{ESessionFactoryError::INVALID_ARGUMENT, "skeleton.identity"});
                auto decoded = asset::TAssetSerDeser<asset::SkeletonAsset>::decode(
                    input.asset, cxx::SharedBytes<>::copyOf(image), asset::AssetDecodeLimits{max_bytes, max_bytes, 64});
                if (!decoded)
                    return cxx::unexpected(SessionFactoryFailure{ESessionFactoryError::DECODE,
                                                                 "skeleton.codec",
                                                                 static_cast<std::uint64_t>(decoded.error().code)});
                if (input.reload)
                    return SessionPreparation{
                        code,
                        *input.reload,
                        [code,
                         value = std::move(*decoded),
                         expected = *input.reload,
                         binding = input.binding,
                         target =
                             input.target](SessionStore &store) mutable -> SessionFactoryResult<PreparedSessionReload>
                        {
                            auto key = store.key<Session>(expected.session);
                            if (!key)
                                return cxx::unexpected(factoryFailure(key.error()));
                            auto owner = store.access<Session>().edit(*key);
                            if (!owner)
                                return cxx::unexpected(factoryFailure(owner.error()));
                            std::unique_ptr<Session> candidate;
                            SessionResult<void> built;
                            auto prepare = [&]
                            {
                                auto admitted = std::move(value);
                                const auto info = owner->get().describe();
                                if (info.current != expected || info.binding != binding)
                                {
                                    built = cxx::unexpected(ESessionError::STALE_CONTENT);
                                    return;
                                }
                                auto result = Session::create(expected.session, binding, std::move(admitted));
                                if (!result)
                                    built = cxx::unexpected(result.error());
                                else
                                    candidate = std::move(*result);
                            };
                            auto read = owner->get().withRead(prepare);
                            if (!read)
                                return cxx::unexpected(factoryFailure(read.error()));
                            if (!built)
                                return cxx::unexpected(factoryFailure(built.error()));
                            auto source = std::make_unique<SaveSource>(
                                store.access<Session>(), *key, target, owner->get().bindingRevision(), code);
                            return PreparedSessionReload{
                                code,
                                expected.session,
                                [key = *key, expected, candidate = std::move(candidate)](
                                    SessionStore &store) mutable -> SessionFactoryResult<ContentStamp>
                                {
                                    auto model = store.access<Session>().edit(key);
                                    if (!model)
                                        return cxx::unexpected(factoryFailure(model.error()));
                                    auto result = model->get().adopt(expected, *candidate);
                                    if (!result)
                                        return cxx::unexpected(factoryFailure(result.error()));
                                    return model->get().describe().current;
                                },
                                std::move(source)};
                        }};
                return SessionPreparation{
                    code,
                    [code, value = std::move(*decoded), binding = input.binding, target = input.target](
                        SessionStore &store,
                        SaveService &saves) mutable -> SessionFactoryResult<PreparedSessionInstallation>
                    {
                        auto slot = store.reserve<Session>(kind, code);
                        if (!slot)
                            return cxx::unexpected(factoryFailure(slot.error()));
                        auto key = store.key<Session>(*slot);
                        if (!key)
                            return cxx::unexpected(factoryFailure(key.error()));
                        auto candidate = Session::create(slot->id(), binding, std::move(value));
                        if (!candidate)
                            return cxx::unexpected(factoryFailure(candidate.error()));
                        auto prepared = store.prepare(*slot, *candidate);
                        if (!prepared)
                            return cxx::unexpected(factoryFailure(prepared.error()));
                        return PreparedSessionInstallation::prepare(
                            store,
                            saves,
                            std::move(*slot),
                            code,
                            std::make_unique<Actions>(store.access<Session>(), *key),
                            std::make_unique<SaveSource>(
                                store.access<Session>(), *key, target, BindingRevision{1}, code));
                    }};
            });
    }
} // namespace skeleton
