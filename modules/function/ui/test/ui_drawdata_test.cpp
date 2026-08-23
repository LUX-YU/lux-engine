#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include <lux/engine/ui/DrawDataSnapshot.hpp>

#include <utility>

int main() {
  auto *context = ImGui::CreateContext();
  ImGui::SetCurrentContext(context);
  ImDrawList list{ImGui::GetDrawListSharedData()};

  ImDrawData data;
  data.Valid = true;
  data.CmdLists.push_back(&list);
  data.CmdListsCount = 1;
  data.TotalVtxCount = list.VtxBuffer.Size;
  data.TotalIdxCount = list.IdxBuffer.Size;
  lux::ui::DrawDataSnapshot snapshot;
  snapshot.capture(data);
  assert(snapshot.drawData().CmdListsCount == 1);
  assert(snapshot.drawData().CmdLists[0] != &list);
  lux::ui::DrawDataSnapshot moved{std::move(snapshot)};
  assert(moved.drawData().CmdListsCount == 1);
  assert(moved.drawData().CmdLists[0] != &list);
  assert(snapshot.drawData().CmdListsCount == 0);

  lux::ui::DrawDataSnapshot assigned;
  assigned = std::move(moved);
  assert(assigned.drawData().CmdListsCount == 1);
  assert(assigned.drawData().CmdLists[0] != &list);
  assert(moved.drawData().CmdListsCount == 0);

  assigned.clear();
  ImGui::DestroyContext(context);
  return 0;
}
