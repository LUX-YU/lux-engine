#include <lux/engine/render/frame/FrameData.hpp>
#include <algorithm>

namespace lux::render
{
    std::size_t FrameData::findChannel(
        cxx::basic_type_info declaration, cxx::basic_type_info element, std::optional<std::size_t> ordinal
    ) const noexcept
    {
        std::size_t selected = SIZE_MAX;
        auto rows = batches();
        if (ordinal)
        {
            if (*ordinal >= rows.size()) return SIZE_MAX;
            rows = rows.subspan(*ordinal, 1);
        }
        for (const auto& batch : rows)
        {
            if (batch.channels.empty()) continue;
            const auto type = batch.channels.front().declaration;
            if (type != declaration || type.name() != declaration.name()) continue;
            const auto found = std::ranges::find_if(batch.channels, [&](const FrameChannel& channel)
            {
                return channel.element == element && channel.element.name() == element.name();
            });
            if (found == batch.channels.end()) continue;
            if (selected != SIZE_MAX) return SIZE_MAX;
            selected = batch.first_column + static_cast<std::size_t>(found - batch.channels.begin());
        }
        return selected;
    }
} // namespace lux::render
