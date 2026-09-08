#include "CoreCases.hpp"
#include "FailureCases.hpp"
#include "SaveLimitCases.hpp"

int main(int argc, char** argv)
{
    using namespace lux::editor::editing::test;
#if defined(LUX_EDITOR_EDITING_TEST_DIAGNOSTICS)
    if (argc == 2 && std::string_view(argv[1]) == "identity-exhaustion")
    {
        using namespace lux::editor::editing;
        const auto before = detail::allocationStatistics();
        detail::EditHistoryTestAccess::identityCounter((std::numeric_limits<std::uint64_t>::max)() - 1U);
        {
            auto last = EditHistory::create({kLimits});
            assert(last && (*last)->id().value == (std::numeric_limits<std::uint64_t>::max)());
            expectError(EditHistory::create({kLimits}), EEditError::ID_EXHAUSTED);
        }
        expectError(EditHistory::create({kLimits}), EEditError::ID_EXHAUSTED);
        assert(before == detail::allocationStatistics());
        std::cout << "L08 PASS issuer saturated without reuse or leaked factory resources\n";
        return 0;
    }
#endif
    coreCases();
    failureCases();
    saveLimitCases();
    TextSession text;
    writeSnapshot("text.initial.alpha", text);
    change(text, "beta");
    writeSnapshot("text.execute.beta", text);
    assert(text.history->undo() && text.text() == "alpha");
    writeSnapshot("text.undo.alpha", text);
    assert(text.history->redo() && text.text() == "beta");
    writeSnapshot("text.redo.beta", text);
    RecordSession records;
    execute(records, records.patch(7, {}, 42));
    assert(records.select(7));
    writeSnapshot("records.create.7=42.selected=7", records);
    execute(records, records.patch(7, 42, {}));
    assert(records.records().empty() && !records.selected());
    writeSnapshot("records.delete.empty.selected=none", records);
    assert(records.history->undo() && records.records().at(7) == 42 && records.selected() == 7);
    writeSnapshot("records.undo.7=42.selected=7", records);
    assert(records.history->redo() && records.records().empty() && !records.selected());
    writeSnapshot("records.redo.empty.selected=none", records);
}
