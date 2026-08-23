#include <lux/engine/ui_next/UiNext.hpp>
#include <lux/engine/ui_next_drawdata/DrawDataSnapshot.hpp>

int main()
{
    lux::ui::UISession session;
    session.beginFrame({320.0F, 200.0F}, 1.0F / 60.0F);
    const auto* draw_data = session.endFrame();
    lux::ui::drawdata::DrawDataSnapshot snapshot;
    snapshot.capture(*draw_data);
    const auto summary = lux::ui::drawdata::summarize(snapshot.drawData());
    if (summary.command_lists
        != static_cast<std::uint32_t>(draw_data->CmdListsCount))
    {
        return 3;
    }
    if (summary.vertices != static_cast<std::uint32_t>(draw_data->TotalVtxCount))
        return 4;
    if (summary.indices != static_cast<std::uint32_t>(draw_data->TotalIdxCount))
        return 5;
    return 0;
}
