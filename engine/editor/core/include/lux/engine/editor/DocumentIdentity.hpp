#pragma once

#include <lux/cxx/container/SlotMap.hpp>
#include <lux/engine/resource/identity/AssetId.hpp>
#include <string>

namespace lux::editor
{
    struct DocumentTag;
    using DocumentHandle = lux::cxx::SlotKey<DocumentTag, std::uint32_t, std::uint64_t>;

    struct DocumentKey final
    {
        asset::AssetId project;
        asset::AssetId source;
        std::string type;

        friend bool operator==(const DocumentKey &, const DocumentKey &) = default;
    };

    struct OpenRequestId final
    {
        std::uint64_t value{};
        friend bool operator==(OpenRequestId, OpenRequestId) = default;
    };

    struct PollBudget final
    {
        std::size_t main_completions{64};
        std::size_t object_messages{64};
        std::size_t render_replies{64};
        std::size_t document_steps{32};
        std::size_t render_programs{64};
    };
} // namespace lux::editor
