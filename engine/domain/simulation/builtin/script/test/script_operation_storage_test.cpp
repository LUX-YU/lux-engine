#include <lux/engine/simulation/script/ScriptOperationStorage.hpp>
#include <cassert>
#include <iostream>

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
}
int main()
{
    testPromotionRearmAndStale();
    testExtraAndForeignConsumer();
    testExternalFirstAndQuota();
    std::cout << "LAYOUT cell=" << sizeof(ScriptOperationCell) << " local=" << sizeof(ScriptLocalWait)
        << " boxed=" << sizeof(ScriptBoxedWait) << " execution=" << sizeof(ScriptExecutionState) << '\n';
}
