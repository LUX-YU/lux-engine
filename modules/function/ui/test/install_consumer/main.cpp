#include <lux/engine/ui/UI.hpp>

int main() {
  lux::ui::UISession session;
  session.beginFrame({320.0F, 200.0F}, 1.0F / 60.0F);
  const auto *draw_data = session.endFrame();
  lux::ui::DrawDataSnapshot snapshot;
  snapshot.capture(*draw_data);
  if (snapshot.drawData().CmdListsCount != draw_data->CmdListsCount) {
    return 3;
  }
  if (snapshot.drawData().TotalVtxCount != draw_data->TotalVtxCount)
    return 4;
  if (snapshot.drawData().TotalIdxCount != draw_data->TotalIdxCount)
    return 5;
  return 0;
}
