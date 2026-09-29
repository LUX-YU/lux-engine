#include <lux/engine/editor/persistence/WriteCoordinator.hpp>
#include <cassert>
#include <iostream>
#include <map>
#include <span>
#include <stdexcept>

using namespace lux::editor::persistence;
namespace
{
    struct Store final : IArtifactStore
    {
        std::map<std::string, std::string> files;
        std::vector<std::uint64_t> writes;
        bool retired{};
        bool fail_reconciliation{};
        PersistenceResult<WriteTarget> resolve(std::string_view address) override
        {
            auto key = std::string(address);
            return WriteTarget{{key}, files.contains(key) ? files[key] : "missing"};
        }
        VPublicationOutcome publish(const PublicationQuery& work, std::stop_token) override
        {
            const auto current = resolve(work.target.key.value)->expected_version;
            if (current != work.target.expected_version)
                return NotPublished{{EPersistenceError::CONFLICT}};
            const auto& bytes = work.artifact->bytes;
            auto version = std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            files[work.target.key.value] = version;
            writes.push_back(work.ticket.value);
            return CommitReceipt{version, EDurability::FILE_FLUSHED};
        }
        Reconciliation reconcile(const PublicationQuery& work) override
        {
            if (fail_reconciliation)
                throw std::runtime_error("foreign reconciliation failure");
            if (!retired)
                return {false, PublicationUnknown{{EPersistenceError::IO}, "pending"}};
            return {true, publish(work, {})};
        }
    };
    EncodedArtifact bytes(std::string_view value)
    {
        auto view = std::as_bytes(std::span(value));
        return {{view.begin(), view.end()}};
    }
    void publish(WriteCoordinator& coordinator, Store& store)
    {
        auto work = coordinator.takeReady();
        assert(work && *work);
        auto outcome = store.publish(**work, {});
        assert(coordinator.complete((*work)->ticket, std::move(outcome)));
    }
}
int main()
{
    Store store;
    WriteCoordinator coordinator;
    WriteOrigin origin{{1, 0, 1}, {1}};
    auto target = *store.resolve("a");
    const auto first = *coordinator.reserve(target, origin);
    const auto second = *coordinator.reserve(target, origin);
    assert(coordinator.provideEncoded(second, bytes("S12")));
    assert(!*coordinator.takeReady());
    assert(coordinator.provideEncoded(first, bytes("S10")));
    publish(coordinator, store);
    publish(coordinator, store);
    assert(store.files["a"] == "S12");
    const auto undo = *coordinator.reserve(target, origin);
    assert(coordinator.provideEncoded(undo, bytes("S8")));
    publish(coordinator, store);
    assert(store.files["a"] == "S8" && store.writes == std::vector<std::uint64_t>({1, 2, 3}));
    std::cout << "X05-01 reverse ready + older state intent: PASS\n";
    const auto hole = *coordinator.reserve(*store.resolve("a"), origin);
    const auto follower = *coordinator.reserve(*store.resolve("a"), origin);
    assert(coordinator.provideEncoded(follower, bytes("next")));
    assert(coordinator.cancelBeforePublish(hole, {EPersistenceError::ENCODE}));
    publish(coordinator, store);
    assert(store.files["a"] == "next");
    std::cout << "X05-02 failure hole: PASS\n";
    const auto unknown = *coordinator.reserve(*store.resolve("a"), origin);
    const auto later = *coordinator.reserve(*store.resolve("a"), origin);
    assert(coordinator.provideEncoded(unknown, bytes("unknown")));
    assert(coordinator.provideEncoded(later, bytes("last")));
    auto work = coordinator.takeReady();
    assert(work && *work && (*work)->ticket == unknown);
    assert(coordinator.complete(unknown, PublicationUnknown{{EPersistenceError::IO}, "pending"}));
    assert(!coordinator.acknowledge(unknown));
    assert(!coordinator.reconcile(unknown, store));
    store.fail_reconciliation = true;
    auto callback_failure = coordinator.reconcile(unknown, store);
    assert(!callback_failure && callback_failure.error().code == EPersistenceError::IO);
    assert(coordinator.status(unknown)->stage == EWriteStage::UNKNOWN);
    store.fail_reconciliation = false;
    assert(!*coordinator.takeReady());
    const auto independent = *coordinator.reserve(*store.resolve("b"), origin);
    assert(coordinator.provideEncoded(independent, bytes("independent")));
    publish(coordinator, store);
    assert(store.files["b"] == "independent");
    store.retired = true;
    assert(coordinator.reconcile(unknown, store));
    publish(coordinator, store);
    assert(store.files["a"] == "last");
    std::cout << "X05-03 active late writer quarantines one lane until retirement and verification: PASS\n";
    const auto other = *coordinator.reserve(target, {{1, 1, 1}, {1}});
    assert(coordinator.provideEncoded(other, bytes("must conflict")));
    publish(coordinator, store);
    assert(std::holds_alternative<NotPublished>(*coordinator.status(other)->outcome));
    assert(store.files["a"] == "last");
    std::cout << "X05-06 distinct copy cannot inherit writer version: PASS\n";
    WriteCoordinator bounded{{1, 2}};
    auto ticket = *bounded.reserve({{"bounded"}, "missing"}, origin);
    assert(!bounded.reserve({{"second"}, "missing"}, origin));
    assert(!bounded.provideEncoded(ticket, bytes("large")));
    assert(bounded.cancelBeforePublish(ticket, {EPersistenceError::CANCELLED}));
    assert(bounded.acknowledge(ticket));
    assert(bounded.size() == 0 && bounded.reserve({{"bounded"}, "missing"}, origin));
    std::cout << "X05-09 bounded tickets and bytes: PASS\n";
}
