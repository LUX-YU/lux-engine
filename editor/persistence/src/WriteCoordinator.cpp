#include <lux/engine/editor/persistence/WriteCoordinator.hpp>
#include <algorithm>
#include <limits>
#include <thread>

namespace lux::editor::persistence
{
    namespace
    {
        auto failed(EPersistenceError code)
        {
            return lux::cxx::unexpected(PersistenceFailure{code});
        }
    }
    struct WriteCoordinator::Impl final
    {
        struct Record final
        {
            WriteTicket ticket;
            WriteOrigin origin;
            PublicationQuery work;
            EWriteStage stage{EWriteStage::RESERVED};
            std::optional<VPublicationOutcome> outcome;
        };
        struct Lane final
        {
            WriteTargetKey key;
            std::optional<WriteOrigin> last_writer;
            std::string chain_base;
            std::string version;
            std::uint64_t chain_begin{};
        };
        const std::thread::id owner{std::this_thread::get_id()};
        WriteLimits limits;
        std::uint64_t next{1};
        std::size_t bytes{};
        std::vector<Record> records;
        std::vector<Lane> lanes;
        explicit Impl(WriteLimits value) : limits(value) {}
        bool onOwner() const noexcept
        {
            return owner == std::this_thread::get_id();
        }
        auto find(WriteTicket ticket)
        {
            return std::ranges::find(records, ticket, &Record::ticket);
        }
        auto lane(const WriteTargetKey& key)
        {
            return std::ranges::find(lanes, key, &Lane::key);
        }
        void settle(Record& record, VPublicationOutcome outcome)
        {
            if (const auto* committed = std::get_if<CommitReceipt>(&outcome))
            {
                auto target = lane(record.work.target.key);
                if (target->last_writer != record.origin)
                {
                    target->chain_base = record.work.target.expected_version;
                    target->chain_begin = record.ticket.value;
                }
                target->last_writer = record.origin;
                target->version = committed->version;
                // Carry the chain into already admitted successors before receipts can be acknowledged.
                // No unbounded list of historical file versions is retained by the lane.
                for (auto& pending : records)
                {
                    const bool waiting = pending.stage == EWriteStage::RESERVED || pending.stage == EWriteStage::READY;
                    const bool same_source = record.origin.session.valid() && pending.origin == record.origin &&
                                             pending.work.target.key == target->key;
                    const bool matches_base =
                        pending.work.target.expected_version == record.work.target.expected_version ||
                        pending.work.target.expected_version == target->chain_base;
                    if (waiting && same_source && matches_base && pending.ticket.value > record.ticket.value)
                        pending.work.target.expected_version = committed->version;
                }
            }
            record.stage =
                std::holds_alternative<PublicationUnknown>(outcome) ? EWriteStage::UNKNOWN : EWriteStage::TERMINAL;
            if (record.stage == EWriteStage::TERMINAL && record.work.artifact)
            {
                bytes -= record.work.artifact->bytes.size();
                record.work.artifact.reset();
            }
            if (const auto* unknown = std::get_if<PublicationUnknown>(&outcome))
                record.work.token = unknown->token;
            record.outcome = std::move(outcome);
        }
    };
    WriteCoordinator::WriteCoordinator(WriteLimits limits) : impl_(std::make_unique<Impl>(limits)) {}
    WriteCoordinator::~WriteCoordinator() = default;
    PersistenceResult<WriteTicket> WriteCoordinator::reserve(WriteTarget target, WriteOrigin origin)
    {
        if (!impl_->onOwner())
            return failed(EPersistenceError::WRONG_THREAD);
        if (target.key.value.empty() || target.expected_version.empty())
            return failed(EPersistenceError::INVALID_ARGUMENT);
        if (impl_->records.size() >= impl_->limits.tickets || impl_->next == UINT64_MAX)
            return failed(EPersistenceError::CAPACITY);
        if (impl_->lane(target.key) == impl_->lanes.end())
            impl_->lanes.push_back({target.key});
        const WriteTicket ticket{impl_->next++};
        impl_->records.push_back({ticket, origin, {ticket, std::move(target), {}, {}}});
        return ticket;
    }
    PersistenceResult<void> WriteCoordinator::provideEncoded(WriteTicket ticket, EncodedArtifact artifact)
    {
        if (!impl_->onOwner())
            return failed(EPersistenceError::WRONG_THREAD);
        const auto found = impl_->find(ticket);
        if (found == impl_->records.end())
            return failed(EPersistenceError::UNKNOWN_ID);
        if (found->stage != EWriteStage::RESERVED)
            return failed(EPersistenceError::BUSY);
        if (artifact.bytes.size() > impl_->limits.artifact_bytes - impl_->bytes)
            return failed(EPersistenceError::CAPACITY);
        impl_->bytes += artifact.bytes.size();
        found->work.artifact = std::make_shared<const EncodedArtifact>(std::move(artifact));
        found->stage = EWriteStage::READY;
        return {};
    }
    PersistenceResult<void> WriteCoordinator::cancelBeforePublish(WriteTicket ticket, PersistenceFailure failure)
    {
        if (!impl_->onOwner())
            return failed(EPersistenceError::WRONG_THREAD);
        const auto found = impl_->find(ticket);
        if (found == impl_->records.end())
            return failed(EPersistenceError::UNKNOWN_ID);
        const bool can_cancel = found->stage == EWriteStage::RESERVED || found->stage == EWriteStage::READY;
        if (!can_cancel)
            return failed(EPersistenceError::BUSY);
        impl_->settle(*found, NotPublished{std::move(failure)});
        return {};
    }
    PersistenceResult<std::optional<PublicationQuery>> WriteCoordinator::takeReady()
    {
        if (!impl_->onOwner())
            return failed(EPersistenceError::WRONG_THREAD);
        for (auto it = impl_->records.begin(); it != impl_->records.end(); ++it)
        {
            if (it->stage != EWriteStage::READY)
                continue;
            const bool has_predecessor = std::any_of(impl_->records.begin(), it, [&](const auto& earlier) {
                return earlier.work.target.key == it->work.target.key && earlier.stage != EWriteStage::TERMINAL;
            });
            if (has_predecessor)
                continue;
            const auto lane = impl_->lane(it->work.target.key);
            const bool has_chain_version = std::ranges::any_of(impl_->records, [&](const auto& earlier) {
                const bool is_chain = earlier.ticket.value >= lane->chain_begin &&
                                      earlier.ticket.value < it->ticket.value && earlier.work.target.key == lane->key &&
                                      earlier.origin == it->origin && earlier.outcome;
                if (!is_chain)
                    return false;
                const auto* receipt = std::get_if<CommitReceipt>(&*earlier.outcome);
                return receipt && receipt->version == it->work.target.expected_version;
            });
            const bool is_same_chain = it->origin.session.valid() && lane->last_writer == it->origin &&
                                       (it->work.target.expected_version == lane->chain_base ||
                                        it->work.target.expected_version == lane->version || has_chain_version);
            if (is_same_chain)
                it->work.target.expected_version = lane->version;
            it->stage = EWriteStage::PUBLISHING;
            return std::optional{it->work};
        }
        return std::optional<PublicationQuery>{};
    }
    PersistenceResult<void> WriteCoordinator::complete(WriteTicket ticket, VPublicationOutcome outcome)
    {
        if (!impl_->onOwner())
            return failed(EPersistenceError::WRONG_THREAD);
        auto found = impl_->find(ticket);
        if (found == impl_->records.end())
            return failed(EPersistenceError::UNKNOWN_ID);
        if (found->stage != EWriteStage::PUBLISHING)
            return failed(EPersistenceError::BUSY);
        impl_->settle(*found, std::move(outcome));
        return {};
    }
    PersistenceResult<void> WriteCoordinator::reconcile(WriteTicket ticket, IArtifactStore& store)
    {
        if (!impl_->onOwner())
            return failed(EPersistenceError::WRONG_THREAD);
        auto found = impl_->find(ticket);
        if (found == impl_->records.end())
            return failed(EPersistenceError::UNKNOWN_ID);
        if (found->stage != EWriteStage::UNKNOWN)
            return failed(EPersistenceError::BUSY);
        const auto work = found->work;
        auto result = store.reconcile(work);
        if (!result.writer_retired)
            return failed(EPersistenceError::WRITER_ACTIVE);
        // Reacquire by ID: a backend is not allowed to invalidate a borrowed vector reference by reentry.
        found = impl_->find(ticket);
        if (found == impl_->records.end() || found->stage != EWriteStage::UNKNOWN)
            return failed(EPersistenceError::BUSY);
        impl_->settle(*found, std::move(result.outcome));
        return {};
    }
    PersistenceResult<WriteStatus> WriteCoordinator::status(WriteTicket ticket) const
    {
        if (!impl_->onOwner())
            return failed(EPersistenceError::WRONG_THREAD);
        const auto found = impl_->find(ticket);
        if (found == impl_->records.end())
            return failed(EPersistenceError::UNKNOWN_ID);
        return WriteStatus{found->stage, found->outcome};
    }
    PersistenceResult<void> WriteCoordinator::acknowledge(WriteTicket ticket)
    {
        if (!impl_->onOwner())
            return failed(EPersistenceError::WRONG_THREAD);
        const auto found = impl_->find(ticket);
        if (found == impl_->records.end())
            return failed(EPersistenceError::UNKNOWN_ID);
        if (found->stage != EWriteStage::TERMINAL)
            return failed(EPersistenceError::NOT_TERMINAL);
        const auto key = found->work.target.key;
        impl_->records.erase(found);
        if (std::ranges::none_of(impl_->records, [&](const auto& item) { return item.work.target.key == key; }))
            impl_->lanes.erase(impl_->lane(key));
        return {};
    }
    std::size_t WriteCoordinator::size() const noexcept
    {
        return impl_->records.size();
    }
}
