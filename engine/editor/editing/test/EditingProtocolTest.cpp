#include "CoreCases.hpp"
#include "FailureCases.hpp"
#include "SaveLimitCases.hpp"

int main()
{
    using namespace lux::editor::editing::test;
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
