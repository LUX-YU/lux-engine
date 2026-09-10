#include <lux/engine/simulation/script/ScriptOperationStorage.hpp>
#include <cassert>
#include <iostream>
#include <cstdlib>
#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace
{
    bool reject_payload_allocation{};
    unsigned payload_allocation_failures{};
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    if (reject_payload_allocation)
    {
        ++payload_allocation_failures;
        return nullptr;
    }
#if defined(_MSC_VER)
    return _aligned_malloc(size, static_cast<std::size_t>(alignment));
#else
    const auto a = static_cast<std::size_t>(alignment);
    return std::aligned_alloc(a, (size + a - 1U) / a * a);
#endif
}
void operator delete(void* pointer, std::align_val_t) noexcept
{
#if defined(_MSC_VER)
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}

using namespace lux::simulation::script;
using namespace lux::simulation::script::detail;

namespace
{
    struct Fixture final
    {
        ScriptOperationStorage cells;
        ScriptOperationStorage::Continuations executions{cells};
        ScriptOperationStorage::Awaitables waits{cells};
        Fixture(std::size_t c, std::size_t a)
        {
            cells.prepare(c, a);
            executions.reserve(c);
            waits.reserve(a);
        }
        ~Fixture() { assert(cells.used() == 0U); }
    };
    ScriptExecutionState execution(ScriptAwaitableId waiting) noexcept
    {
        ScriptExecutionState result;
        result.instance = {1, 1};
        result.waiting_on = waiting;
        return result;
    }
    void testPromotionRearmAndStale()
    {
        Fixture f{1, 1};
        ScriptCellTicket scope;
        assert(f.cells.used() == 0U);
        const auto wait = f.waits.admit({1, 1}, {}, false, &scope);
        assert(wait && f.cells.used() == 1U && f.executions.empty());
        auto* stored = f.waits.find(*wait);
        const auto first_ticket = stored->location;
        const auto c = f.executions.tryEmplace(execution(stored->id));
        assert(c && f.cells.used() == 1U && f.executions[*c].cell == scope);
        const auto cid = f.executions[*c].id;
        for (unsigned i = 0; i < 32U; ++i)
        {
            const auto key = ScriptOperationStorage::AwaitableKey{stored->id.slot - 1U, stored->id.generation};
            const auto old = stored->location;
            assert(f.waits.erase(key));
            assert(f.cells.used() == 1U && f.cells.wait(old) == nullptr);
            const auto next = f.waits.admit({1, 1}, {}, false, &scope);
            assert(next && f.cells.used() == 1U);
            stored = f.waits.find(*next);
            assert(stored->location.cell == first_ticket.cell && stored->location.wait_epoch > old.wait_epoch);
        }
        const auto retained = stored->location;
        assert(f.executions.erase(*c));
        assert(f.cells.used() == 1U && f.cells.execution(scope, cid) == nullptr);
        assert(f.cells.wait(retained) == stored); // A alone retains the stable result body.
        assert(f.waits.erase({stored->id.slot - 1U, stored->id.generation}));
        assert(f.cells.used() == 0U && f.cells.wait(retained) == nullptr);
        std::cout << "CASE promotion_rearm_stale rearmed=32 physical_cells=1 wait_only=1\n";
    }
    void testExtraAndForeignConsumer()
    {
        Fixture f{2, 3};
        ScriptCellTicket scope;
        const auto local = f.waits.admit({1, 1}, {}, false, &scope);
        const auto extra = f.waits.admit({1, 1}, {}, false, &scope);
        assert(local && extra && f.cells.used() == 2U);
        auto* local_wait = f.waits.find(*local);
        assert(ScriptOperationStorage::local(*local_wait));
        assert(!ScriptOperationStorage::local(*f.waits.find(*extra)));
        const auto first = f.executions.tryEmplace(execution(local_wait->id));
        const auto second = f.executions.tryEmplace(execution(local_wait->id));
        assert(first && second && f.cells.used() == 3U);
        assert(f.executions[*first].cell == scope && f.executions[*second].cell != scope);
        assert(!local_wait->continuation.valid()); // Hosting execution is not an attached consumer.
        local_wait->continuation = f.executions[*second].id;
        local_wait->attached_execution = f.executions[*second].cell;
        assert(f.cells.execution(local_wait->attached_execution, local_wait->continuation) == &f.executions[*second]);
        assert(f.executions.erase(*first));
        assert(f.cells.wait(local_wait->location) == local_wait);
        assert(f.executions.erase(*second));
        assert(f.waits.erase(*local) && f.waits.erase(*extra));
        std::cout << "CASE extra_foreign_consumer local=1 boxed=1 executions=2 preserved_wait=1\n";
    }
    void testExternalFirstAndQuota()
    {
        Fixture f{1, 1};
        const auto boxed = f.waits.admit({1, 1}, {}, true, nullptr);
        assert(boxed && !ScriptOperationStorage::local(*f.waits.find(*boxed)));
        const auto c = f.executions.tryEmplace(execution(f.waits.find(*boxed)->id));
        assert(c && f.cells.used() == 2U);
        auto scope = f.executions[*c].cell;
        assert(!f.waits.admit({1, 1}, {}, false, &scope)); // Same A quota, despite an empty inline slot.
        assert(f.cells.used() == 2U);
        assert(f.waits.erase(*boxed));
        const auto local = f.waits.admit({1, 1}, {}, false, &scope);
        assert(local && ScriptOperationStorage::local(*f.waits.find(*local)) && f.cells.used() == 1U);
        assert(f.waits.erase(*local));
        assert(f.executions.erase(*c));
        std::cout << "CASE external_first local_after_take=1 shared_capacity_rejection=1\n";
    }

    void testHeaderOwnerAndExhaustion()
    {
        Fixture first{1, 1}, other{1, 1};
        ScriptCellTicket scope;
        const auto waiting = first.waits.admit({1, 1}, {}, false, &scope);
        assert(waiting);
        const auto ticket = first.waits.find(*waiting)->location;
        assert(other.cells.wait(ticket) == nullptr && !other.cells.valid(scope));
        const auto c = first.executions.tryEmplace(execution(first.waits.find(*waiting)->id));
        assert(c && first.waits.erase(*waiting));
        scope.cell->wait_epoch = UINT64_MAX;
        assert(!first.waits.admit({1, 1}, {}, false, &scope));
        assert(first.executions.find(*c) && first.cells.used() == 1U);
        assert(first.executions.erase(*c));
        assert(first.cells.wait(ticket) == nullptr);
        // Exhausted headers are not wrapped into a capability held by an old source.
        scope.cell->epoch = UINT64_MAX;
        scope = {};
        const auto final_wait = first.waits.admit({1, 1}, {}, false, &scope);
        assert(final_wait && scope.cell != ticket.cell.cell);
        assert(first.waits.erase(*final_wait));
        std::cout << "CASE owner_epoch wrong_owner=1 wait_exhaustion=1 cell_exhaustion=1 stale=1\n";
    }

    void testPublicGenerationAndPayloadFailure()
    {
        struct TestTag;
        ScriptCellDirectory<ScriptCellTicket, TestTag, 4U> directory;
        directory.reserve(1U);
        for (std::uint32_t generation = 1U; generation < 4U; ++generation)
        {
            const auto key = directory.tryEmplace({});
            assert(key && key->gen == generation && directory.erase(*key));
            assert(directory.find(*key) == nullptr);
        }
        assert(directory.empty() && !directory.tryEmplace({}));
        Fixture f{1, 1};
        const PreparedResumeType large{lux::semantic::typeId("lux.test.aligned64"),
            LUX_SCRIPT_VK_STRUCT_REF, 64U, 64U};
        assert(large.valid());
        reject_payload_allocation = true;
        const auto rejected = f.waits.admit({1, 1}, large, false, nullptr);
        reject_payload_allocation = false;
        assert(!rejected && payload_allocation_failures == 1U && f.waits.empty() && f.cells.used() == 0U);
        const auto accepted = f.waits.admit({1, 1}, large, false, nullptr);
        assert(accepted);
        auto* wait = f.waits.find(*accepted);
        const auto bytes = ScriptOperationStorage::bytes(*wait);
        assert(bytes.size() == 64U && reinterpret_cast<std::uintptr_t>(bytes.data()) % 64U == 0U);
        std::memset(bytes.data(), 0x37, bytes.size());
        wait->state = EScriptAwaitableState::READY;
        auto outcome = ScriptOperationStorage::takeValue(*wait);
        assert(f.waits.erase(*accepted) && f.cells.used() == 0U);
        assert(outcome.bytes.size() == 64U && outcome.bytes.data()[63] == std::byte{0x37});
        std::cout << "CASE public_generation_payload exhausted=1 injected_failure=1 recovered=1 aligned64=1\n";
    }
}
int main()
{
    testPromotionRearmAndStale();
    testExtraAndForeignConsumer();
    testExternalFirstAndQuota();
    testHeaderOwnerAndExhaustion();
    testPublicGenerationAndPayloadFailure();
    std::cout << "LAYOUT cell=" << sizeof(ScriptOperationCell) << " local=" << sizeof(ScriptLocalWait)
        << " boxed=" << sizeof(ScriptBoxedWait) << " execution=" << sizeof(ScriptExecutionState) << '\n';
}
