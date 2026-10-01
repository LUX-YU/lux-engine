#include <lux/engine/editor/persistence/SaveService.hpp>
#include <algorithm>
#include <thread>
#include <utility>

namespace lux::editor::persistence
{
    namespace
    {
        auto failed(EPersistenceError code)
        {
            return lux::cxx::unexpected(PersistenceFailure{code});
        }
        // Narrow foreign role boundaries: rejected captures still settle their reserved ticket/allowance.
        PersistenceResult<SaveSourceInfo> describeSource(const ISaveSource& source)
        try
        {
            return source.describe();
        }
        catch (const std::bad_alloc&)
        {
            std::terminate();
        }
        catch (...)
        {
            return failed(EPersistenceError::STALE_SOURCE);
        }
        PersistenceResult<FrozenSave> captureSource(
            ISaveSource& source,
            const SaveSourceInfo& info,
            const SaveRequest& request,
            std::size_t allowance
        )
        try
        {
            return source.captureForSave(info, request, allowance);
        }
        catch (const std::bad_alloc&)
        {
            std::terminate();
        }
        catch (...)
        {
            return failed(EPersistenceError::ENCODE);
        }
    }
    struct SaveSourceRegistration::State final
    {
        contracts::CodeLease code;
        std::unique_ptr<ISaveSource> owned_source;
        ISaveSource* source;
        sessions::SessionId session;
        const void* service;
        bool published{};
    };
    SaveSourceRegistration::SaveSourceRegistration(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}
    SaveSourceRegistration::~SaveSourceRegistration()
    {
        if (state_)
            state_->source = nullptr;
    }
    SaveSourceRegistration::SaveSourceRegistration(SaveSourceRegistration&&) noexcept = default;
    SaveSourceRegistration& SaveSourceRegistration::operator=(SaveSourceRegistration&& other) noexcept
    {
        if (this != &other)
        {
            if (state_)
                state_->source = nullptr;
            state_ = std::move(other.state_);
        }
        return *this;
    }
    PreparedSaveSourceRegistration::PreparedSaveSourceRegistration(std::shared_ptr<SaveSourceRegistration::State> state
    ) noexcept
        : state_(std::move(state))
    {}
    PreparedSaveSourceRegistration::~PreparedSaveSourceRegistration()
    {
        if (state_)
            state_->source = nullptr;
    }
    PreparedSaveSourceRegistration::PreparedSaveSourceRegistration(PreparedSaveSourceRegistration&&) noexcept = default;
    PreparedSaveSourceRegistration& PreparedSaveSourceRegistration::operator=(PreparedSaveSourceRegistration&& other
    ) noexcept
    {
        if (this != &other)
        {
            if (state_)
                state_->source = nullptr;
            state_ = std::move(other.state_);
        }
        return *this;
    }
    struct SaveService::Impl final
    {
        // One owner-thread call frame covers role dispatch and destruction of callback-owned inputs.
        // This protects service records only; SessionState remains the sole editing admission gate.
        struct DispatchScope final
        {
            bool& active;
            const bool previous;
            explicit DispatchScope(bool& value) noexcept : active(value), previous(std::exchange(value, true)) {}
            ~DispatchScope()
            {
                active = previous;
            }
            DispatchScope(const DispatchScope&) = delete;
            DispatchScope& operator=(const DispatchScope&) = delete;
        };
        struct Operation final
        {
            SaveId id;
            SaveRequest request;
            std::shared_ptr<SaveSourceRegistration::State> registration;
            FrozenSave frozen;
            WriteTicket ticket;
            ESaveStage stage{ESaveStage::CAPTURED};
            bool cancel_requested{};
            std::optional<SaveOutcome> outcome;
        };
        const std::thread::id owner{std::this_thread::get_id()};
        WriteCoordinator& coordinator;
        SaveLimits limits;
        std::uint64_t next{1};
        std::size_t snapshot_bytes{};
        bool dispatching{};
        std::vector<std::weak_ptr<SaveSourceRegistration::State>> sources;
        std::vector<std::unique_ptr<Operation>> operations;
        // The service's creating module owns allocation/deallocation code for the weak registration
        // directory. A plugin may call prepareSource through its static SDK copy, then unload while
        // expired weak records remain; their control block must not have a plugin-local vtable.
        using AllocateSource = std::shared_ptr<
            SaveSourceRegistration::State> (*)(contracts::CodeLease, ISaveSource*, sessions::SessionId, const void*);
        AllocateSource allocate_source;
        Impl(WriteCoordinator& value, SaveLimits policy)
            : coordinator(value), limits(policy), allocate_source(+[](contracts::CodeLease code,
                                                                      ISaveSource* source,
                                                                      sessions::SessionId id,
                                                                      const void* service) {
                  return std::make_shared<SaveSourceRegistration::State>(std::move(code), nullptr, source, id, service);
              })
        {}
        bool onOwner() const noexcept
        {
            return owner == std::this_thread::get_id();
        }
        auto find(SaveId id)
        {
            return std::ranges::find_if(operations, [id](const auto& value) { return value->id == id; });
        }
        std::shared_ptr<SaveSourceRegistration::State> source(sessions::SessionId id, bool include_prepared = false)
        {
            for (const auto& weak : sources)
                if (auto found = weak.lock();
                    found && found->session == id && found->source && (include_prepared || found->published))
                    return found;
            return {};
        }
        PersistenceResult<PreparedSaveSourceRegistration> prepare(
            sessions::SessionId id,
            ISaveSource& source_value,
            contracts::CodeLease code
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
            auto entry = allocate_source(std::move(code), &source_value, id, this);
            sources.push_back(entry);
            return PreparedSaveSourceRegistration{std::move(entry)};
        }
        void releaseSnapshot(Operation& op)
        {
            snapshot_bytes -= op.frozen.retained_bytes;
            op.frozen.retained_bytes = 0;
            op.frozen.encoding = {};
        }
    };
    SaveService::SaveService(WriteCoordinator& coordinator, SaveLimits limits)
        : impl_(std::make_unique<Impl>(coordinator, limits))
    {}
    SaveService::~SaveService()
    {
        // The scheduler adapter has already drained. Preserve settled/unknown disk records in the coordinator.
        for (auto& op : impl_->operations)
        {
            if (op->stage == ESaveStage::ENCODING)
                std::terminate();
            auto write = impl_->coordinator.status(op->ticket);
            if (write && (write->stage == EWriteStage::RESERVED || write->stage == EWriteStage::READY))
                (void)impl_->coordinator.cancelBeforePublish(op->ticket, {EPersistenceError::CANCELLED});
        }
    }
    PersistenceResult<SaveSourceRegistration> SaveService::registerSource(
        ISaveSource& source,
        contracts::CodeLease code
    )
    {
        if (!impl_->onOwner())
            return failed(EPersistenceError::WRONG_THREAD);
        if (impl_->dispatching)
            return failed(EPersistenceError::BUSY);
        const Impl::DispatchScope dispatch(impl_->dispatching);
        if (!code.valid())
            return failed(EPersistenceError::INVALID_ARGUMENT);
        auto info = describeSource(source);
        if (!info)
            return lux::cxx::unexpected(info.error());
        const bool invalid_identity =
            !info->content.session.valid() || !info->content.state.valid() || info->binding.value == 0;
        if (invalid_identity)
            return failed(EPersistenceError::INVALID_ARGUMENT);
        auto prepared = impl_->prepare(info->content.session, source, std::move(code));
        if (!prepared)
            return lux::cxx::unexpected(prepared.error());
        prepared->state_->published = true;
        return SaveSourceRegistration{std::move(prepared->state_)};
    }
    PersistenceResult<void> SaveService::canPrepareSource() const noexcept
    {
        if (!impl_->onOwner())
            return failed(EPersistenceError::WRONG_THREAD);
        if (impl_->dispatching)
            return failed(EPersistenceError::BUSY);
        return {};
    }
    PersistenceResult<PreparedSaveSourceRegistration> SaveService::prepareSource(
        sessions::SessionId id,
        std::unique_ptr<ISaveSource> source,
        contracts::CodeLease code
    )
    {
        // External code pin encloses rejection cleanup as well as the entire source destructor.
        struct Input final
        {
            contracts::CodeLease code;
            std::unique_ptr<ISaveSource> source;
        };
        Input owned{std::move(code), std::move(source)};
        if (!impl_->onOwner())
            return failed(EPersistenceError::WRONG_THREAD);
        if (impl_->dispatching)
            return failed(EPersistenceError::BUSY);
        const Impl::DispatchScope dispatch{impl_->dispatching};
        if (!owned.source)
            return failed(EPersistenceError::INVALID_ARGUMENT);
        auto prepared = impl_->prepare(id, *owned.source, owned.code);
        if (prepared)
            prepared->state_->owned_source = std::move(owned.source);
        return prepared;
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
    PersistenceResult<SaveId> SaveService::requestSave(SaveRequest request)
    {
        if (!impl_->onOwner())
            return failed(EPersistenceError::WRONG_THREAD);
        if (impl_->dispatching)
            return failed(EPersistenceError::BUSY);
        const Impl::DispatchScope dispatch(impl_->dispatching);
        const auto active =
            std::ranges::count_if(impl_->operations, [](const auto& op) { return op->stage != ESaveStage::TERMINAL; });
        const bool is_full = active >= impl_->limits.max_active_saves ||
                             impl_->operations.size() >= impl_->limits.terminal_records || impl_->next == UINT64_MAX;
        if (is_full)
            return failed(EPersistenceError::CAPACITY);
        auto registration = impl_->source(request.session);
        if (!registration)
            return failed(EPersistenceError::STALE_SOURCE);
        const bool has_active = std::ranges::any_of(impl_->operations, [&](const auto& op) {
            const bool unresolved =
                op->stage != ESaveStage::TERMINAL ||
                (op->outcome && std::holds_alternative<PublicationUnknown>(op->outcome->publication));
            return op->request.session == request.session && unresolved;
        });
        if (request.mode == ESaveMode::SAVE_AS && has_active)
            return failed(EPersistenceError::BUSY);
        auto* const source = registration->source;
        auto info = describeSource(*source);
        if (registration->source != source)
            return failed(EPersistenceError::STALE_SOURCE);
        if (!info)
            return lux::cxx::unexpected(info.error());
        if (info->content.session != request.session)
            return failed(EPersistenceError::STALE_SOURCE);
        auto target = request.mode == ESaveMode::SAVE ? info->target : request.destination;
        if (!target)
            return failed(EPersistenceError::UNBOUND);
        auto ticket = impl_->coordinator.reserve(*target, {request.session, info->binding});
        if (!ticket)
            return lux::cxx::unexpected(ticket.error());
        const auto allowance = impl_->limits.snapshot_bytes - impl_->snapshot_bytes;
        if (allowance == 0)
        {
            (void)impl_->coordinator.cancelBeforePublish(*ticket, {EPersistenceError::CAPACITY});
            (void)impl_->coordinator.acknowledge(*ticket);
            return failed(EPersistenceError::CAPACITY);
        }
        // Reserve the operation and its temporary capture allowance before any extensible codec runs.
        const SaveId id{impl_->next++};
        auto operation = std::make_unique<Impl::Operation>();
        operation->id = id;
        operation->request = request;
        operation->registration = registration;
        operation->ticket = *ticket;
        operation->stage = ESaveStage::CAPTURING;
        operation->frozen.retained_bytes = allowance;
        impl_->snapshot_bytes += allowance;
        auto* captured = operation.get();
        impl_->operations.push_back(std::move(operation));
        auto frozen = captureSource(*source, *info, request, allowance);
        const bool is_revoked = registration->source != source;
        const bool invalid_capture =
            frozen && (frozen->source.content != info->content || frozen->source.binding != info->binding ||
                       frozen->source.target != info->target || !frozen->encoding.job ||
                       !frozen->encoding.code.valid() || frozen->retained_bytes > allowance);
        const bool is_stale_capture = is_revoked || invalid_capture;
        const bool is_rejected_capture = !frozen || is_stale_capture;
        if (is_rejected_capture)
        {
            auto error = is_stale_capture ? PersistenceFailure{EPersistenceError::STALE_SOURCE} : frozen.error();
            (void)impl_->coordinator.cancelBeforePublish(*ticket, error);
            (void)impl_->coordinator.acknowledge(*ticket);
            impl_->snapshot_bytes -= allowance;
            impl_->operations.erase(impl_->find(id));
            return lux::cxx::unexpected(std::move(error));
        }
        impl_->snapshot_bytes -= allowance - frozen->retained_bytes;
        captured->frozen = std::move(*frozen);
        captured->stage = ESaveStage::CAPTURED;
        return id;
    }
    PersistenceResult<std::optional<EncodeWork>> SaveService::takeEncoding()
    {
        if (!impl_->onOwner())
            return failed(EPersistenceError::WRONG_THREAD);
        if (impl_->dispatching)
            return failed(EPersistenceError::BUSY);
        const Impl::DispatchScope dispatch(impl_->dispatching);
        for (auto& op : impl_->operations)
        {
            if (op->stage != ESaveStage::CAPTURED)
                continue;
            op->stage = ESaveStage::ENCODING;
            return std::optional<EncodeWork>{{op->id, std::move(op->frozen.encoding)}};
        }
        return std::optional<EncodeWork>{};
    }
    PersistenceResult<void> SaveService::completeEncoding(SaveId id, PersistenceResult<EncodedArtifact> result)
    {
        if (!impl_->onOwner())
            return failed(EPersistenceError::WRONG_THREAD);
        // Already admitted work must settle even inside a role callback. This leaf updates only
        // its existing operation/ticket; it neither dispatches roles nor erases service records.
        // A nested completion must preserve the outer callback's admission/deletion protection.
        const Impl::DispatchScope dispatch(impl_->dispatching);
        auto found = impl_->find(id);
        if (found == impl_->operations.end())
            return failed(EPersistenceError::UNKNOWN_ID);
        auto& op = **found;
        if (op.stage != ESaveStage::ENCODING)
            return failed(EPersistenceError::BUSY);
        // takeEncoding transferred the job and its lease to the worker. Only the allowance remains;
        // no plugin job, rebind candidate or registration is destroyed by completion absorption.
        impl_->snapshot_bytes -= std::exchange(op.frozen.retained_bytes, 0);
        PersistenceResult<void> supplied;
        if (result && !op.cancel_requested)
            supplied = impl_->coordinator.provideEncoded(op.ticket, std::move(*result));
        if (!result || op.cancel_requested || !supplied)
        {
            const auto failure = op.cancel_requested ? PersistenceFailure{EPersistenceError::CANCELLED}
                                                     : (!result ? result.error() : supplied.error());
            (void)impl_->coordinator.cancelBeforePublish(op.ticket, failure);
        }
        op.stage = ESaveStage::READY;
        return {};
    }
    void SaveService::adoptCompletions()
    {
        if (!impl_->onOwner())
            std::terminate();
        if (impl_->dispatching)
            return;
        const Impl::DispatchScope dispatch(impl_->dispatching);
        // Snapshot stable IDs: role callbacks must not invalidate traversal through vector growth.
        std::vector<SaveId> batch;
        batch.reserve(impl_->operations.size());
        for (const auto& op : impl_->operations)
            batch.push_back(op->id);
        for (auto id : batch)
        {
            auto found = impl_->find(id);
            if (found == impl_->operations.end())
                continue;
            auto& op = **found;
            auto write = impl_->coordinator.status(op.ticket);
            if (!write)
                std::terminate();
            if (op.stage == ESaveStage::TERMINAL)
            {
                // Reconciliation changes the disk fact only. An abandoned rebind is never silently reinstated.
                if (op.outcome && std::holds_alternative<PublicationUnknown>(op.outcome->publication) &&
                    write->stage == EWriteStage::TERMINAL && write->outcome)
                    op.outcome->publication = *write->outcome;
                continue;
            }
            if (write->stage == EWriteStage::PUBLISHING)
                op.stage = ESaveStage::PUBLISHING;
            if (!write->outcome)
                continue;
            op.stage = ESaveStage::AWAITING_ADOPTION;
            op.outcome = SaveOutcome{*write->outcome};
            if (const auto* receipt = std::get_if<CommitReceipt>(&*write->outcome))
            {
                auto target = op.request.mode == ESaveMode::SAVE ? *op.frozen.source.target : *op.request.destination;
                SaveReceipt saved{
                    op.frozen.source.content,
                    op.frozen.source.binding,
                    {op.ticket.value},
                    std::move(target),
                    *receipt
                };
                if (op.request.mode != ESaveMode::EXPORT_COPY)
                {
                    if (!op.registration->source)
                        op.outcome->adoption = EAdoption::CLOSED;
                    else if (op.frozen.rebind)
                        op.outcome->adoption = op.frozen.rebind->apply(std::move(saved));
                    else
                        op.outcome->adoption = op.registration->source->accept(std::move(saved));
                }
            }
            if (op.outcome->adoption == EAdoption::BUSY)
                continue;
            op.frozen.rebind.reset();
            op.stage = ESaveStage::TERMINAL;
        }
    }
    PersistenceResult<SaveStatus> SaveService::status(SaveId id) const
    {
        if (!impl_->onOwner())
            return failed(EPersistenceError::WRONG_THREAD);
        const auto found = impl_->find(id);
        if (found == impl_->operations.end())
            return failed(EPersistenceError::UNKNOWN_ID);
        const auto& op = **found;
        return SaveStatus{op.stage, op.frozen.source.content, op.ticket, op.outcome};
    }
    PersistenceResult<ECancelResult> SaveService::requestCancel(SaveId id)
    {
        if (!impl_->onOwner())
            return failed(EPersistenceError::WRONG_THREAD);
        if (impl_->dispatching)
            return failed(EPersistenceError::BUSY);
        const Impl::DispatchScope dispatch(impl_->dispatching);
        const auto found = impl_->find(id);
        if (found == impl_->operations.end())
            return failed(EPersistenceError::UNKNOWN_ID);
        auto& op = **found;
        if (op.stage == ESaveStage::TERMINAL)
            return ECancelResult::ALREADY_TERMINAL;
        if (op.stage == ESaveStage::CAPTURING)
            return failed(EPersistenceError::BUSY);
        const auto write = impl_->coordinator.status(op.ticket);
        if (!write)
            return lux::cxx::unexpected(write.error());
        const bool has_disk_fact = write->stage == EWriteStage::PUBLISHING || write->stage == EWriteStage::UNKNOWN ||
                                   write->stage == EWriteStage::TERMINAL;
        if (has_disk_fact)
            return ECancelResult::TOO_LATE;
        op.cancel_requested = true;
        if (op.stage == ESaveStage::ENCODING)
            return ECancelResult::REQUESTED;
        auto cancelled = impl_->coordinator.cancelBeforePublish(op.ticket, {EPersistenceError::CANCELLED});
        if (!cancelled)
            return lux::cxx::unexpected(cancelled.error());
        impl_->releaseSnapshot(op);
        op.stage = ESaveStage::READY;
        return ECancelResult::REQUESTED;
    }
    PersistenceResult<void> SaveService::acknowledge(SaveId id)
    {
        if (!impl_->onOwner())
            return failed(EPersistenceError::WRONG_THREAD);
        if (impl_->dispatching)
            return failed(EPersistenceError::BUSY);
        const Impl::DispatchScope dispatch(impl_->dispatching);
        const auto found = impl_->find(id);
        if (found == impl_->operations.end())
            return failed(EPersistenceError::UNKNOWN_ID);
        if ((*found)->stage != ESaveStage::TERMINAL)
            return failed(EPersistenceError::NOT_TERMINAL);
        auto acknowledged = impl_->coordinator.acknowledge((*found)->ticket);
        if (!acknowledged)
            return acknowledged;
        impl_->operations.erase(found);
        return {};
    }
}
