#include <lux/engine/editor/flowforge/FlowSaveSource.hpp>
#include <lux/engine/editor/flowforge/FlowCodec.hpp>
namespace lux::editor::flowforge
{
    using namespace persistence;
    namespace
    {
        auto failure(sessions::ESessionError error)
        {
            return lux::cxx::unexpected(PersistenceFailure{
                error == sessions::ESessionError::BUSY ? EPersistenceError::BUSY : EPersistenceError::STALE_SOURCE
            });
        }
        class PreparedFlowRebind final : public IPreparedRebind
        {
        public:
            PreparedFlowRebind(
                sessions::TSessionAccess<FlowSession> access,
                sessions::TSessionKey<FlowSession> key,
                sessions::SourceBinding binding,
                WriteTarget target,
                std::optional<WriteTarget>& installed,
                sessions::BindingRevision& revision,
                sessions::BindingChangePermit permit
            )
                : access_(access), key_(key), binding_(std::move(binding)), target_(std::move(target)),
                  installed_(installed), revision_(revision), permit_(std::move(permit))
            {}
            EAdoption apply(SaveReceipt&& receipt) noexcept override
            {
                auto session = access_.edit(key_);
                if (!session)
                    return EAdoption::CLOSED;
                auto applied =
                    FlowPersistenceAccess::rebind(session->get(), permit_, std::move(binding_), receipt.order);
                if (!applied)
                    return EAdoption::STALE_BINDING;
                target_.expected_version = std::move(receipt.publication.version);
                installed_ = std::move(target_);
                ++revision_.value;
                return EAdoption::APPLIED;
            }

        private:
            sessions::TSessionAccess<FlowSession> access_;
            sessions::TSessionKey<FlowSession> key_;
            sessions::SourceBinding binding_;
            WriteTarget target_;
            std::optional<WriteTarget>& installed_;
            sessions::BindingRevision& revision_;
            sessions::BindingChangePermit permit_;
        };
        class FlowEncodeJob final : public IEncodeJob
        {
        public:
            FlowEncodeJob(FlowSnapshot snapshot, asset::AssetId id) : snapshot_(std::move(snapshot)), identity_(id) {}
            PersistenceResult<EncodedArtifact> encode(std::stop_token stop) override
            {
                return FlowCodec::encode(snapshot_, identity_, stop);
            }

        private:
            FlowSnapshot snapshot_;
            asset::AssetId identity_;
        };
    }
    PersistenceResult<SaveSourceInfo> FlowSaveSource::describe() const
    {
        auto session = access_.read(key_);
        if (!session)
            return failure(session.error());
        auto state = FlowPersistenceAccess::inspect(session->get());
        if (!state)
            return failure(state.error());
        if (state->revision != target_binding_)
            return failure(sessions::ESessionError::STALE_BINDING);
        return SaveSourceInfo{state->content, state->revision, state->source ? target_ : std::nullopt};
    }
    PersistenceResult<FrozenSave> FlowSaveSource::captureForSave(
        const SaveSourceInfo& expected,
        const SaveRequest& request,
        std::size_t max_bytes
    )
    {
        auto session = access_.edit(key_);
        if (!session)
            return failure(session.error());
        auto state = FlowPersistenceAccess::inspect(session->get());
        if (!state)
            return failure(state.error());
        const bool is_stale = state->content != expected.content || state->revision != expected.binding;
        if (is_stale)
            return failure(sessions::ESessionError::STALE_CONTENT);
        const auto id =
            request.mode != ESaveMode::SAVE ? request.asset : (state->source ? state->source->asset : asset::AssetId{});
        if (id.isNull())
            return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::UNBOUND});
        if (request.mode != ESaveMode::SAVE && !request.destination)
            return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::INVALID_ARGUMENT});
        auto snapshot = session->get().capture({max_bytes});
        if (!snapshot)
            return lux::cxx::unexpected(PersistenceFailure{
                snapshot.error().code == EFlowEditError::BUDGET ? EPersistenceError::CAPACITY
                                                                : EPersistenceError::ENCODE
            });
        std::unique_ptr<IPreparedRebind> rebind;
        if (request.mode == ESaveMode::SAVE_AS)
        {
            sessions::SourceBinding binding = sessions::BoundSource{id, request.destination->key.value};
            auto permit = FlowPersistenceAccess::prepareRebind(session->get(), expected.content, expected.binding);
            if (!permit)
                return failure(permit.error());
            rebind = std::make_unique<PreparedFlowRebind>(
                access_,
                key_,
                std::move(binding),
                *request.destination,
                target_,
                target_binding_,
                std::move(*permit)
            );
        }
        const auto retained = snapshot->retainedBytes();
        auto job = std::make_unique<FlowEncodeJob>(std::move(*snapshot), id);
        return FrozenSave{expected, retained, {contracts::CodeLease::builtin(), std::move(job)}, std::move(rebind)};
    }
    EAdoption FlowSaveSource::accept(SaveReceipt&& receipt) noexcept
    {
        auto session = access_.edit(key_);
        if (!session)
            return EAdoption::CLOSED;
        auto accepted = FlowPersistenceAccess::accept(session->get(), receipt.content, receipt.binding, receipt.order);
        if (!accepted)
        {
            if (accepted.error() == sessions::ESessionError::BUSY)
                return EAdoption::BUSY;
            if (accepted.error() == sessions::ESessionError::STALE_BINDING)
                return EAdoption::STALE_BINDING;
            if (accepted.error() == sessions::ESessionError::STALE_PUBLICATION)
                return EAdoption::OLDER_RECEIPT;
            return EAdoption::STALE_HISTORY;
        }
        if (target_)
            target_->expected_version = std::move(receipt.publication.version);
        return EAdoption::APPLIED;
    }
}
