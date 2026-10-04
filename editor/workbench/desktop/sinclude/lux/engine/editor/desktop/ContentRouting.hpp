#pragma once

#include <lux/engine/editor/views/ViewInfo.hpp>
#include <span>

namespace lux::editor::desktop::detail
{
    enum class EContentSelectionError : std::uint8_t
    {
        NOT_FOUND,
        AMBIGUOUS
    };
    struct ContentSelectionFailure final
    {
        EContentSelectionError code;
        std::string candidates;
    };
    // The two factory protocols share this cold content-selection policy during migration.
    // Entries and candidate indices belong to one already-validated immutable catalog.
    template <class Entry>
    [[nodiscard]] cxx::expected<std::size_t, ContentSelectionFailure> selectContent(
        std::span<const Entry> entries,
        std::span<const std::size_t> indices,
        const std::optional<views::ViewTypeId>& preferred
    )
    {
        std::optional<std::size_t> selected;
        std::size_t defaults{};
        std::string candidates;
        for (const auto index : indices)
        {
            const auto& descriptor = entries[index]->descriptor();
            // External preferred names are cold input; equal hashes alone cannot select a provider.
            if (preferred && descriptor.type.name() == preferred->name())
            {
                return index;
            }
            if (descriptor.default_content_view)
            {
                ++defaults;
                selected = index;
            }
            if (!candidates.empty())
            {
                candidates += ", ";
            }
            candidates += descriptor.type.name();
        }
        if (preferred || indices.empty())
        {
            return cxx::unexpected(ContentSelectionFailure{EContentSelectionError::NOT_FOUND});
        }
        if (defaults == 1)
        {
            return *selected;
        }
        if (indices.size() == 1)
        {
            return indices.front();
        }
        return cxx::unexpected(ContentSelectionFailure{EContentSelectionError::AMBIGUOUS, std::move(candidates)});
    }
} // namespace lux::editor::desktop::detail
