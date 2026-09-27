#pragma once

#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/editor/editing/EditTypes.hpp>
#include <variant>

namespace lux::editor
{
    struct SaveRequestId final
    {
        editing::HistoryId history;
        std::uint64_t serial{};
        friend bool operator==(SaveRequestId, SaveRequestId) = default;
    };

    enum class ESaveStage : std::uint8_t
    {
        ENCODING,
        WAITING_FOR_PROJECT,
        PUBLISHING,
        ABANDONING
    };
    struct SavePending final
    {
        ESaveStage stage;
        std::uint64_t attempt{};
    };
    struct SaveRetryable final
    {
        EditorFailure failure;
        editing::StateId captured;
        std::uint64_t attempt{};
        bool retry_allowed{true};
    };
    struct SaveSucceeded final
    {
        editing::StateId captured;
        editing::Revision revision;
        EditorResult<void> cleanup;
    };
    struct SaveAbandoned final
    {};
    using VSaveRequestStatus = std::variant<SavePending, SaveRetryable, SaveSucceeded, SaveAbandoned>;

}
