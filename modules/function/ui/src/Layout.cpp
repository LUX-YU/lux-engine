#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/detail/Contract.hpp>
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <numeric>

namespace lux::ui
{
    namespace
    {
        float insetExtent(float available, float before, float after) noexcept
        {
            return std::max(0.F, available - before - after);
        }
        float alignedOffset(EAlignment alignment, float available, float size) noexcept
        {
            if (alignment == EAlignment::CENTER)
                return (available - size) * 0.5F;
            if (alignment == EAlignment::END)
                return available - size;
            return 0.F;
        }
    }

    Layout::Layout(object::ObjectDispatcherRef dispatcher, ElementId id, ELayoutType type)
        : Element(std::move(dispatcher), std::move(id)), type_(type)
    {}
    Layout::Layout(Pane& parent, ElementId id, ELayoutType type) : Element(parent, std::move(id)), type_(type) {}
    Layout::Layout(Element& parent, ElementId id, ELayoutType type) : Element(parent, std::move(id)), type_(type) {}

    Element* Layout::elementOf(object::LuxObject* object) const noexcept
    {
        auto* element = static_cast<Element*>(object);
        // Replaced owned children remain on Object's ownership chain until its safe point.
        // They have already left this layout and must not be measured or paired in a form.
        return element->element_parent_ == this ? element : nullptr;
    }

    void Layout::setType(ELayoutType type) noexcept
    {
        if (!isOnAffinityThread())
            detail::failContract();
        type_ = type;
    }
    void Layout::setSpacing(Vec2 spacing) noexcept
    {
        const bool valid = std::isfinite(spacing.x) && std::isfinite(spacing.y) && spacing.x >= 0 && spacing.y >= 0;
        if (!isOnAffinityThread() || !valid)
            detail::failContract();
        spacing_ = spacing;
    }
    void Layout::setMargins(Insets margins) noexcept
    {
        const bool finite = std::isfinite(margins.left) && std::isfinite(margins.right) && std::isfinite(margins.top) &&
                            std::isfinite(margins.bottom);
        const bool nonnegative = margins.left >= 0 && margins.right >= 0 && margins.top >= 0 && margins.bottom >= 0;
        if (!isOnAffinityThread() || !finite || !nonnegative)
            detail::failContract();
        margins_ = margins;
    }
    void Layout::setColumns(std::size_t count) noexcept
    {
        if (!isOnAffinityThread() || count == 0)
            detail::failContract();
        column_count_ = count;
    }
    void Layout::setScrollable(bool horizontal, bool vertical) noexcept
    {
        if (!isOnAffinityThread())
            detail::failContract();
        horizontal_scroll_ = horizontal;
        vertical_scroll_ = vertical;
    }

    ELayoutStatus Layout::status() const noexcept
    {
        if (!isOnAffinityThread())
            detail::failContract();
        if (type_ != ELayoutType::FORM)
            return ELayoutStatus::VALID;
        bool unpaired{};
        for (auto* child = firstChild(); child; child = child->nextSibling())
            if (elementOf(child))
                unpaired = !unpaired;
        return unpaired ? ELayoutStatus::INCOMPLETE_FORM : ELayoutStatus::VALID;
    }

    void Layout::collect() noexcept
    {
        cells_.clear();
        Element* label{};
        std::size_t pair_count{};
        for (auto* object = firstChild(); object; object = object->nextSibling())
        {
            auto* element = elementOf(object);
            if (!element)
                continue;
            if (type_ == ELayoutType::FORM)
            {
                if (!label)
                {
                    label = element;
                    continue;
                }
                if (label->visible() && element->visible())
                {
                    cells_.push_back({label, label->sizeHint(), 0, pair_count});
                    cells_.push_back({element, element->sizeHint(), 1, pair_count});
                    ++pair_count;
                }
                label = nullptr;
            }
            else if (element->visible())
                cells_.push_back({element, element->sizeHint(), 0, 0});
        }
        if (type_ == ELayoutType::FORM)
            return;
        const auto count = type_ == ELayoutType::HORIZONTAL ? cells_.size()
                           : type_ == ELayoutType::GRID     ? column_count_
                                                            : 1;
        for (std::size_t i{}; i < cells_.size(); ++i)
        {
            cells_[i].column = i % count;
            cells_[i].row = i / count;
        }
    }
    void Layout::columns() noexcept
    {
        const auto count = cells_.empty() ? 0
                                          : std::max_element(
                                                cells_.begin(),
                                                cells_.end(),
                                                [](const Cell& a, const Cell& b) { return a.column < b.column; }
                                            )->column +
                                                1;
        columns_.assign(count, {});
        for (const auto& cell : cells_)
        {
            auto& track = columns_[cell.column];
            track.minimum = std::max(track.minimum, cell.hint.minimum.width);
            track.preferred = std::max(track.preferred, cell.hint.preferred.width);
            track.maximum = std::max(track.maximum, cell.hint.maximum.width);
            const auto weight = type_ == ELayoutType::FORM && cell.column == 0 ? 0.F : cell.element->stretch().x;
            track.weight = std::max(track.weight, weight);
        }
    }
    void Layout::rows() noexcept
    {
        rows_.assign(cells_.empty() ? 0 : cells_.back().row + 1, {});
        for (const auto& cell : cells_)
        {
            auto& track = rows_[cell.row];
            track.minimum = std::max(track.minimum, cell.hint.minimum.height);
            track.preferred = std::max(track.preferred, cell.hint.preferred.height);
            track.maximum = std::max(track.maximum, cell.hint.maximum.height);
            track.weight = std::max(track.weight, cell.element->stretch().y);
        }
    }
    float Layout::extent(const std::vector<Track>& tracks, float Track::*member, float spacing) noexcept
    {
        float result = tracks.empty() ? 0.F : float(tracks.size() - 1) * spacing;
        for (const auto& track : tracks)
            result += track.*member;
        return result;
    }
    void Layout::fit(std::vector<Track>& tracks, float available, float spacing, float origin) noexcept
    {
        double total{}, minimum{}, weights{};
        saturated_.clear();
        for (auto& track : tracks)
        {
            track.size = track.preferred;
            total += track.size;
            minimum += track.minimum;
            if (track.weight > 0 && track.size < track.maximum)
                weights += track.weight;
        }
        if (available < total)
        {
            const double fraction = total > minimum ? std::min(1.0, (total - available) / (total - minimum)) : 0;
            for (auto& track : tracks)
                track.size -= static_cast<float>((track.size - track.minimum) * fraction);
        }
        else if (weights > 0)
        {
            double remaining = available - total;
            const double initial_share = remaining / weights;
            for (std::size_t i{}; i < tracks.size(); ++i)
            {
                const auto& track = tracks[i];
                const double capacity = track.maximum - track.size;
                if (track.weight > 0 && capacity > 0 && capacity / track.weight < initial_share)
                    saturated_.push_back(i);
            }
            // The common uncapped case is linear. With finite caps, sorted saturation thresholds
            // avoid repeatedly scanning every track. Sorting indices preserves child order.
            if (!saturated_.empty())
            {
                saturated_.clear();
                for (std::size_t i{}; i < tracks.size(); ++i)
                    if (tracks[i].weight > 0 && tracks[i].size < tracks[i].maximum)
                        saturated_.push_back(i);
                const auto threshold = [&](std::size_t i) noexcept -> double {
                    return (static_cast<double>(tracks[i].maximum) - tracks[i].size) / tracks[i].weight;
                };
                std::sort(saturated_.begin(), saturated_.end(), [&](auto a, auto b) noexcept {
                    return threshold(a) < threshold(b);
                });
                for (const auto i : saturated_)
                {
                    if (weights <= 0 || threshold(i) >= remaining / weights)
                        break;
                    remaining -= static_cast<double>(tracks[i].maximum) - tracks[i].size;
                    weights -= tracks[i].weight;
                }
            }
            const double share =
                weights > 0 ? std::max(0.0, remaining / weights) : std::numeric_limits<double>::infinity();
            for (auto& track : tracks)
                if (track.weight > 0)
                    track.size += static_cast<float>(
                        std::min(static_cast<double>(track.maximum - track.size), share * track.weight)
                    );
        }
        for (auto& track : tracks)
        {
            track.offset = origin;
            origin += track.size + spacing;
        }
    }
    SizeHint Layout::trackHint() const noexcept
    {
        const float bar_width = vertical_scroll_ ? ImGui::GetStyle().ScrollbarSize : 0.F;
        const float bar_height = horizontal_scroll_ ? ImGui::GetStyle().ScrollbarSize : 0.F;
        const auto size = [&](float Track::*member) noexcept -> Size {
            return {
                extent(columns_, member, spacing_.x) + margins_.left + margins_.right + bar_width,
                extent(rows_, member, spacing_.y) + margins_.top + margins_.bottom + bar_height
            };
        };
        return {size(&Track::minimum), size(&Track::preferred), size(&Track::maximum)};
    }
    SizeHint Layout::sizeHintContent() noexcept
    {
        collect();
        columns();
        rows();
        return trackHint();
    }
    SizeHint Layout::measureContent(float width) noexcept
    {
        static_cast<void>(sizeHint());
        // Intrinsic widths do not depend on a previous frame or a previous measurement width.
        for (auto& cell : cells_)
            cell.hint = cell.element->sizeHint();
        columns();
        const float bar_width = vertical_scroll_ ? ImGui::GetStyle().ScrollbarSize : 0.F;
        const float column_spacing = columns_.empty() ? 0.F : float(columns_.size() - 1) * spacing_.x;
        fit(columns_,
            insetExtent(width, margins_.left, margins_.right + bar_width + column_spacing),
            spacing_.x,
            margins_.left);
        for (auto& cell : cells_)
        {
            const bool fill = cell.element->horizontalAlignment() == EAlignment::FILL;
            cell.width = std::clamp(
                fill ? columns_[cell.column].size : cell.hint.preferred.width,
                cell.hint.minimum.width,
                cell.hint.maximum.width
            );
            cell.hint = cell.element->measure(cell.width);
        }
        rows();
        return trackHint();
    }
    void Layout::arrangeContent() noexcept
    {
        // Root has measured this exact width in the current pass; reuse the prepared tracks.
        const float bar_height = horizontal_scroll_ ? ImGui::GetStyle().ScrollbarSize : 0.F;
        const float row_spacing = rows_.empty() ? 0.F : float(rows_.size() - 1) * spacing_.y;
        fit(rows_,
            insetExtent(rect().size.height, margins_.top, margins_.bottom + bar_height + row_spacing),
            spacing_.y,
            margins_.top);
        place();
    }
    void Layout::place() noexcept
    {
        for (const auto& cell : cells_)
        {
            const auto& column = columns_[cell.column];
            const auto& row = rows_[cell.row];
            const auto horizontal = cell.element->horizontalAlignment();
            const auto vertical = cell.element->verticalAlignment();
            const Size size{
                cell.width,
                std::clamp(
                    vertical == EAlignment::FILL ? row.size : cell.hint.preferred.height,
                    cell.hint.minimum.height,
                    cell.hint.maximum.height
                )
            };
            const Point position{
                column.offset + alignedOffset(horizontal, column.size, size.width),
                row.offset + alignedOffset(vertical, row.size, size.height)
            };
            cell.element->arrange({position, size});
        }
    }
    void Layout::draw() noexcept
    {
        const bool scroll = horizontal_scroll_ || vertical_scroll_;
        Point offset{};
        bool shown = true;
        if (scroll)
        {
            ImGuiWindowFlags flags = ImGuiWindowFlags_None;
            if (horizontal_scroll_)
                flags |= ImGuiWindowFlags_AlwaysHorizontalScrollbar;
            if (vertical_scroll_)
                flags |= ImGuiWindowFlags_AlwaysVerticalScrollbar;
            const float width = horizontal_scroll_
                                    ? extent(columns_, &Track::size, spacing_.x) + margins_.left + margins_.right
                                    : std::max(1.F, rect().size.width - ImGui::GetStyle().ScrollbarSize);
            const float height = vertical_scroll_
                                     ? extent(rows_, &Track::size, spacing_.y) + margins_.top + margins_.bottom
                                     : std::max(1.F, rect().size.height - ImGui::GetStyle().ScrollbarSize);
            // Explicit axes reserve their bars during measurement, so adding a
            // scrollbar cannot change wrapping on the following frame.
            ImGui::SetNextWindowContentSize({width, height});
            shown = ImGui::BeginChild("##scroll", {rect().size.width, rect().size.height}, ImGuiChildFlags_None, flags);
            const auto origin = ImGui::GetCursorScreenPos();
            offset = {origin.x - contentOrigin().x, origin.y - contentOrigin().y};
        }
        if (shown)
        {
            for (const auto& cell : cells_)
                drawChild(*cell.element, offset);
            const auto origin = contentOrigin();
            ImGui::SetCursorScreenPos({origin.x + offset.x, origin.y + offset.y});
            ImGui::Dummy(
                {extent(columns_, &Track::size, spacing_.x) + margins_.left + margins_.right,
                 extent(rows_, &Track::size, spacing_.y) + margins_.top + margins_.bottom}
            );
        }
        if (scroll)
            ImGui::EndChild();
    }
}
