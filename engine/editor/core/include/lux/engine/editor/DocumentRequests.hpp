#pragma once

#include <lux/engine/editor/DocumentIdentity.hpp>
#include <lux/cxx/compile_time/expected.hpp>
#include <variant>
#include <any>

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

    struct OpenPending final
    {
    };

    struct OpenCancelled final
    {
    };

    using OpenRequestStatus = std::variant<OpenPending, DocumentHandle, EditorFailure, OpenCancelled>;

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
    };
} // namespace lux::editor
