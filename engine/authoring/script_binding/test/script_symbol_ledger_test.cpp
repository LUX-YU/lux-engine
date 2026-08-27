#include <lux/engine/authoring/ScriptSymbolLedger.hpp>

#include <array>
#include <cassert>
#include <vector>

int main()
{
    using namespace lux::authoring;

    ScriptSymbolLedger ledger;
    const auto tick = ledger.assign("method:tick:void(f32)");
    const auto draw = ledger.assign("method:draw:void()");
    assert(tick && draw && *tick != *draw);
    const auto tick_again = ledger.assign("method:tick:void(f32)");
    assert(tick_again && *tick_again == *tick);

    assert(ledger.rename(
        "method:tick:void(f32)",
        "method:update:void(f32)"
    ));
    assert(ledger.find("method:tick:void(f32)") == 0U);
    assert(ledger.find("method:update:void(f32)") == *tick);

    const auto saved = std::vector<ScriptSymbolLedgerEntry>(
        ledger.entries().begin(),
        ledger.entries().end()
    );
    auto restored = ScriptSymbolLedger::restore(
        saved,
        ledger.nextSymbol()
    );
    assert(restored);
    assert(restored->find("method:update:void(f32)") == *tick);
    const auto later = restored->assign("method:later:void()");
    assert(later && *later > *draw);
    return 0;
}
