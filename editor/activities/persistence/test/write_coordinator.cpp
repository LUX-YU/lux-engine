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
    void sharedByteLifetime()
    {
        Store store;
        WriteCoordinator coordinator{{4, 8}};
        auto source = std::make_shared<const std::vector<std::byte>>(4, std::byte{'x'});
        std::weak_ptr<const std::vector<std::byte>> lifetime = source;
        const auto* original = source->data();
        auto frozen = lux::cxx::SharedBytes<>::fromOwner(source, *source);
        const auto a = *publishEncodedArtifact(coordinator, *store.resolve("shared-a"), frozen);
        const auto b = *publishEncodedArtifact(coordinator, *store.resolve("shared-b"), frozen);
        const auto rejected = publishEncodedArtifact(coordinator, *store.resolve("shared-c"), frozen);
        assert(!rejected && rejected.error().code == EPersistenceError::CAPACITY && coordinator.size() == 2);
        frozen = {};
        source.reset();
        assert(!lifetime.expired());
        {
            const auto work = coordinator.takeReady();
            assert(work && *work && (*work)->ticket == a && (*work)->artifact->bytes.data() == original);
            assert(coordinator.complete(a, PublicationUnknown{{EPersistenceError::IO}, "still active"}));
        }
        assert(!coordinator.acknowledge(a) && !lifetime.expired());
        assert(coordinator.cancelBeforePublish(b, {EPersistenceError::CANCELLED}));
        assert(coordinator.acknowledge(b) && !lifetime.expired());
        store.retired = true;
        assert(coordinator.reconcile(a, store));
        assert(store.files["shared-a"] == "xxxx" && lifetime.expired());
        assert(coordinator.acknowledge(a) && coordinator.size() == 0);
        std::cout
            << "XQ23 SharedBytes: no copy, source released, logical budget per ticket, Unknown and cancellation PASS\n";
    }
    void acknowledgedChains()
    {
        const WriteOrigin origin{{1, 0, 1}, {1}};
        for (int timing{}; timing < 3; ++timing)
        {
            Store store;
            WriteCoordinator coordinator{{8, 128}};
            auto first = *coordinator.reserve(*store.resolve("chain"), origin);
            assert(coordinator.provideEncoded(first, bytes("V1")));
            publish(coordinator, store);
            const auto observed = *store.resolve("chain");
            const auto second = *coordinator.reserve(observed, origin);
            const auto third = *coordinator.reserve(observed, origin);
            if (timing == 0)
                assert(coordinator.acknowledge(first));
            assert(coordinator.provideEncoded(third, bytes("V3")));
            assert(!*coordinator.takeReady());
            assert(coordinator.provideEncoded(second, bytes("V2")));
            if (timing == 1)
                assert(coordinator.acknowledge(first));
            publish(coordinator, store);
            publish(coordinator, store);
            if (timing == 2)
                assert(coordinator.acknowledge(first));
            assert(!coordinator.status(first) && coordinator.size() == 2);
            auto fourth = *coordinator.reserve(observed, origin);
            assert(coordinator.provideEncoded(fourth, bytes("V4")));
            publish(coordinator, store);
            assert(store.files["chain"] == "V4");
            for (auto ticket : {second, third, fourth})
                assert(coordinator.acknowledge(ticket));
            assert(coordinator.size() == 0);
        }
        std::cout << "R05-05 ack during RESERVED/READY/published, verified consumed premise and reverse encode PASS\n";
    }
    void brokenChains()
    {
        Store store;
        WriteCoordinator coordinator;
        const WriteOrigin origin{{1, 0, 1}, {1}};
        const auto initial = *store.resolve("chain");
        auto commit = [&](WriteTarget target, WriteOrigin writer, std::string_view value, bool success) {
            auto ticket = *coordinator.reserve(std::move(target), writer);
            assert(coordinator.provideEncoded(ticket, bytes(value)));
            publish(coordinator, store);
            assert(std::holds_alternative<CommitReceipt>(*coordinator.status(ticket)->outcome) == success);
            return ticket;
        };
        const auto first = commit(initial, origin, "V1", true);
        const auto v1 = *store.resolve("chain");
        (void)commit(v1, origin, "V2", true);
        assert(coordinator.acknowledge(first));
        (void)commit(v1, {{1, 0, 1}, {2}}, "wrong binding", false);
        (void)commit(v1, {}, "anonymous", false);
        (void)commit(v1, {{1, 1, 1}, {1}}, "other session", false);
        (void)commit({v1.key, "unverified"}, origin, "unverified", false);
        store.files["chain"] = "external";
        (void)commit(v1, origin, "must not overwrite external", false);
        assert(store.files["chain"] == "external");
        // An intentional save based on a freshly observed external version begins a new chain.
        (void)commit(*store.resolve("chain"), origin, "new chain", true);
        (void)commit(v1, origin, "old chain is not proof", false);
        (void)commit(*store.resolve("chain"), {{1, 1, 1}, {1}}, "other success", true);
        (void)commit(v1, origin, "cross-origin stale", false);
        assert(store.files["chain"] == "other success");
        std::cout << "R05-07 consumed proof constrained by origin/binding/chain; external edits still conflict PASS\n";
    }
    void boundedChains()
    {
        Store store;
        WriteCoordinator coordinator{{3, 16}};
        const WriteOrigin origin{{1, 0, 1}, {1}};
        for (int i{}; i < 64; ++i)
        {
            const auto base = *store.resolve("bounded");
            const auto hole = *coordinator.reserve(base, origin);
            const auto unknown = *coordinator.reserve(base, origin);
            const auto successor = *coordinator.reserve(base, origin);
            assert(!coordinator.reserve(base, origin));
            assert(coordinator.provideEncoded(successor, bytes("end")));
            assert(coordinator.provideEncoded(unknown, bytes("middle")));
            assert(!*coordinator.takeReady());
            assert(coordinator
                       .cancelBeforePublish(hole, {i % 2 ? EPersistenceError::ENCODE : EPersistenceError::CANCELLED}));
            assert(coordinator.acknowledge(hole));
            const auto work = coordinator.takeReady();
            assert(work && *work && (*work)->ticket == unknown);
            assert(coordinator.complete(unknown, PublicationUnknown{{EPersistenceError::IO}, "pending"}));
            store.retired = false;
            assert(!coordinator.acknowledge(unknown) && !coordinator.reconcile(unknown, store));
            assert(!*coordinator.takeReady());
            store.retired = true;
            assert(coordinator.reconcile(unknown, store));
            assert(coordinator.acknowledge(unknown));
            publish(coordinator, store);
            assert(store.files["bounded"] == "end");
            assert(coordinator.acknowledge(successor) && coordinator.size() == 0);
        }
        std::cout << "R05-08 64 bounded cycles: failure/cancel holes, Unknown quarantine, early ack and all records "
                     "freed PASS\n";
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
    sharedByteLifetime();
    acknowledgedChains();
    brokenChains();
    boundedChains();
}
