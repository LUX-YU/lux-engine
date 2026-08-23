#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include <lux/engine/ui_next_drawdata/DrawDataSnapshot.hpp>

int main()
{
    ImDrawData data;
    data.Valid = true;
    data.CmdListsCount = 0;
    data.TotalVtxCount = 0;
    data.TotalIdxCount = 0;
    lux::ui::drawdata::DrawDataSnapshot snapshot;
    snapshot.capture(data);
    const auto summary = lux::ui::drawdata::summarize(snapshot.drawData());
    assert(summary.command_lists == 0);
    return 0;
}
