#pragma once

#include <any>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/editor/DocumentIdentity.hpp>
#include <lux/engine/editor/editing/EditTypes.hpp>
#include <variant>

namespace lux::editor
{
    enum class EEditorError : std::uint8_t
    {
        INVALID_ARGUMENT,
        INVALID_STATE,
        CAPACITY,
        STALE_REQUEST,
        STALE_DOCUMENT,
        CLOSING,
        CANCELLED,
        MISSING_PROVIDER,
        SOURCE_FAILURE,
        EXECUTION_FAILURE,
        FRONTEND_FAILURE,
        READ_ONLY,
        BUSY
    };

    // Open-ended providers preserve their exact, owning cause without a global error union.
    struct EditorFailure final
    {
        EEditorError code{};
        std::string domain;
        std::uint64_t reason{};
        std::string message;
        std::any cause;
    };

    template <class T> using EditorResult = lux::cxx::expected<T, EditorFailure>;

    struct OpenDocumentRequest final
    {
        DocumentKey key;
        std::string origin;
    };

    struct ExitReviewId final
    {
        std::uint64_t editor{};
        std::uint64_t serial{};
        friend bool operator==(ExitReviewId, ExitReviewId) = default;
    };

    enum class EDocumentCloseDecision : std::uint8_t
    {
        CLOSE_CLEAN,
        DISCARD_THIS_STATE
    };

    struct DocumentCloseDecision final
    {
        DocumentHandle document;
        editing::StateId state;
        editing::Revision revision;
        EDocumentCloseDecision decision{EDocumentCloseDecision::CLOSE_CLEAN};
    };

    struct OpenPending final
    {
    };

    struct OpenCancelled final
    {
    };

    using OpenRequestStatus = std::variant<OpenPending, DocumentHandle, EditorFailure, OpenCancelled>;

    struct SaveRequestId final
    {
        DocumentHandle document;
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
    {
    };
    using SaveRequestStatus = std::variant<SavePending, SaveRetryable, SaveSucceeded, SaveAbandoned>;

    enum class ECloseState : std::uint8_t
    {
        OPEN,
        CLOSING,
        CLOSED
    };

    struct CloseStatus final
    {
        ECloseState state{ECloseState::OPEN};
        std::string waiting_for;
        EditorResult<void> progress;
    };
} // namespace lux::editor
