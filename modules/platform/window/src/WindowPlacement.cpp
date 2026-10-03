#include <lux/engine/window/WindowPlacement.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace lux::window
{
    namespace
    {
        constexpr int kMaximumDimension = 32768;

        bool validRect(const WindowRect& rect) noexcept
        {
            const bool has_valid_size = rect.width > 0 && rect.height > 0 && rect.width <= kMaximumDimension &&
                                        rect.height <= kMaximumDimension;
            const auto right = std::int64_t{rect.x} + rect.width;
            const auto bottom = std::int64_t{rect.y} + rect.height;
            const bool has_valid_edges =
                right <= std::numeric_limits<int>::max() && bottom <= std::numeric_limits<int>::max();
            return has_valid_size && has_valid_edges;
        }

        bool validDisplay(const DisplayInfo& display) noexcept
        {
            const bool has_valid_scale = std::isfinite(display.scale.x) && std::isfinite(display.scale.y) &&
                                         display.scale.x > 0.f && display.scale.y > 0.f;
            return validRect(display.hint.work_area) && has_valid_scale;
        }

        bool validMode(EWindowMode mode) noexcept
        {
            return mode == EWindowMode::ORDINARY || mode == EWindowMode::MAXIMIZED || mode == EWindowMode::FULLSCREEN;
        }
    } // namespace

    WindowPlacementResult resolveWindowPlacement(
        const WindowPlacementRequest& request,
        std::span<const DisplayInfo> displays,
        WindowInsets insets
    ) noexcept
    {
        const bool has_invalid_size = request.size && !validRect({0, 0, request.size->width, request.size->height});
        const bool has_invalid_mode = request.mode && !validMode(*request.mode);
        const bool has_invalid_insets = insets.left < 0 || insets.top < 0 || insets.right < 0 || insets.bottom < 0 ||
                                        insets.left > 1024 || insets.top > 1024 || insets.right > 1024 ||
                                        insets.bottom > 1024;
        const bool is_invalid_request = has_invalid_size || has_invalid_mode || has_invalid_insets;
        if (is_invalid_request)
            return lux::cxx::unexpected(WindowPlacementFailure{
                EWindowPlacementError::INVALID_REQUEST,
                "Invalid explicit window size, mode or decoration insets"
            });

        const auto* hint = request.display ? &*request.display : (request.saved ? &request.saved->display : nullptr);
        const DisplayInfo* selected{};
        if (hint)
        {
            // A name alone is ambiguous; work area is the persisted disambiguator.
            for (const auto& display : displays)
            {
                const bool is_match = validDisplay(display) && display.hint.name == hint->name &&
                                      display.hint.work_area == hint->work_area;
                if (is_match)
                {
                    selected = &display;
                    break;
                }
            }
        }
        const bool matched_hint = selected != nullptr;
        if (!selected)
        {
            for (const auto& display : displays)
            {
                if (!validDisplay(display))
                    continue;
                if (!selected || display.primary)
                    selected = &display;
                if (display.primary)
                    break;
            }
        }
        if (!selected)
            return lux::cxx::unexpected(
                WindowPlacementFailure{EWindowPlacementError::NO_DISPLAY, "No usable display work area"}
            );

        const auto& area = selected->hint.work_area;
        // On tiny displays reserve as much decoration as fits and keep one content unit.
        const int left = std::min(insets.left, area.width - 1);
        const int top = std::min(insets.top, area.height - 1);
        const int width = std::max(1, area.width - left - insets.right);
        const int height = std::max(1, area.height - top - insets.bottom);
        WindowRect normal{0, 0, std::min(1440, width * 4 / 5), std::min(900, height * 4 / 5)};
        normal.width = std::max(1, normal.width);
        normal.height = std::max(1, normal.height);
        const bool valid_saved = request.saved && validRect(request.saved->normal) && validMode(request.saved->mode);
        if (valid_saved)
            normal = request.saved->normal;
        if (request.size)
        {
            normal.width = request.size->width;
            normal.height = request.size->height;
        }
        const auto requested = normal;
        normal.width = std::min(normal.width, width);
        normal.height = std::min(normal.height, height);
        if (!valid_saved || !matched_hint)
        {
            normal.x = area.x + left + (width - normal.width) / 2;
            normal.y = area.y + top + (height - normal.height) / 2;
        }
        normal.x = std::clamp(normal.x, area.x + left, area.x + left + width - normal.width);
        normal.y = std::clamp(normal.y, area.y + top, area.y + top + height - normal.height);
        const auto mode = request.mode.value_or(valid_saved ? request.saved->mode : EWindowMode::ORDINARY);
        const bool adjusted = (request.saved && (!valid_saved || !matched_hint)) || requested.width != normal.width ||
                              requested.height != normal.height ||
                              (valid_saved && (requested.x != normal.x || requested.y != normal.y));
        return ResolvedWindowPlacement{WindowPlacement{normal, mode, selected->hint}, adjusted};
    }
} // namespace lux::window
