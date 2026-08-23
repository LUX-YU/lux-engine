#include <lux/engine/ui/DrawDataSnapshot.hpp>

#include <memory>
#include <utility>

namespace lux::ui {
DrawDataSnapshot::~DrawDataSnapshot() { clear(); }

DrawDataSnapshot::DrawDataSnapshot(DrawDataSnapshot &&other) noexcept
    : draw_data_(other.draw_data_),
      owned_lists_(std::move(other.owned_lists_)) {
  rebuildPointers();
  other.draw_data_.Clear();
}

DrawDataSnapshot &
DrawDataSnapshot::operator=(DrawDataSnapshot &&other) noexcept {
  if (this == std::addressof(other))
    return *this;
  clear();
  draw_data_ = other.draw_data_;
  owned_lists_ = std::move(other.owned_lists_);
  rebuildPointers();
  other.draw_data_.Clear();
  return *this;
}

void DrawDataSnapshot::capture(const ImDrawData &draw_data) {
  clear();
  draw_data_ = draw_data;
  owned_lists_.reserve(static_cast<std::size_t>(draw_data.CmdListsCount));
  for (int index = 0; index < draw_data.CmdListsCount; ++index) {
    auto *copy = draw_data.CmdLists[index]->CloneOutput();
    owned_lists_.push_back(copy);
  }
  rebuildPointers();
}

void DrawDataSnapshot::clear() noexcept {
  for (auto *list : owned_lists_)
    IM_DELETE(list);
  owned_lists_.clear();
  draw_data_.Clear();
}

void DrawDataSnapshot::rebuildPointers() noexcept {
  draw_data_.CmdLists.clear();
  draw_data_.CmdLists.reserve(static_cast<int>(owned_lists_.size()));
  for (auto *list : owned_lists_)
    draw_data_.CmdLists.push_back(list);
  draw_data_.CmdListsCount = draw_data_.CmdLists.Size;
}
} // namespace lux::ui
